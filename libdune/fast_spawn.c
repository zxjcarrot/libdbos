#define _GNU_SOURCE             /* See feature_test_macros(7) */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <memory.h>
#include <sched.h>
#include <errno.h>
#include "ipi_call.h"
#include "fast_spawn.h"

#include <unistd.h>
#include <sys/syscall.h>

#ifndef SYS_gettid
#error "SYS_gettid unavailable on this system"
#endif

#define gettid() ((pid_t)syscall(SYS_gettid))

bool dbos_fast_spawn_enabled = false;
bool dbos_selective_TLB_shootdown = false;

int SNAPSHOT_CORE = 33;
int ASYNC_COPY_CORE = 34;

// __thread uint64_t cycles_spent_in_flt;
// __thread uint64_t cycles_access_start;
// __thread uint64_t cycles_spent_all_access;
// __thread uint64_t cycles_spent_for_interrupt;


static inline unsigned long rdtscll(void)
{
	unsigned int a, d;
	asm volatile("rdtsc" : "=a"(a), "=d"(d) : : "%rbx", "%rcx");
	return ((unsigned long)a) | (((unsigned long)d) << 32);
}

extern uint64_t pgsize(int level);

static bool is_canonical_virtual_address(void * addr) {
	return (uint64_t)addr < 0x800000000000 || (uint64_t)addr >= 0xffff800000000000;
}

static uint64_t canonical_virtual_address(void * addr) {
	if ((uint64_t)addr < 0x800000000000) return (uint64_t)addr;
	else return (uint64_t)addr | 0xffff000000000000;
}

// states are shared across all page tables
fast_spawn_state_t* dune_get_fast_spawn_state() {
	return (fast_spawn_state_t*)dune_ipi_shared_state_page();
}

fast_spawn_state_t* dune_get_fast_spawn_state_host() {
	return (fast_spawn_state_t*)dune_ipi_shared_state_page_nonvirt();
}

void dune_set_thread_state(fast_spawn_state_t * state, int cpu_id, thread_state tstate) {
	state->thread_states[cpu_id] = tstate;
}

pid_t dune_fast_spawn_snapshot_thread_id() {
	return dune_get_fast_spawn_state()->snapshot_thread_tid;
}

thread_state dune_get_thread_state(fast_spawn_state_t * state, int cpu_id) {
	return state->thread_states[cpu_id];
}


typedef struct ipi_call_change_pgtbl_arg_t {
	ptent_t *new_pgroot;
	volatile bool done;
} ipi_call_change_pgtbl_arg_t;

typedef struct ipi_call_tlb_flush_arg_t {
	uint64_t addr;
	volatile bool ready_to_flush;
	volatile bool flushed;
} ipi_call_tlb_flush_arg_t;

static void change_pgtable_ipi_call(void * arg) {
	volatile ipi_call_change_pgtbl_arg_t* call_arg = (ipi_call_change_pgtbl_arg_t*)arg;
	unsigned long rsp;
	int cpu_id = dune_ipi_get_cpu_id();
	asm("mov %%rsp, %0" : "=r"(rsp));
	//dune_printf("change_pgtable_ipi_call on core %d called with arg %p, new_pgroot %p, rsp %p\n", cpu_id, call_arg, call_arg->new_pgroot, rsp);
	assert(call_arg->done == false);
	load_cr3((unsigned long)call_arg->new_pgroot);
	//dune_printf("async_copy_worker changed pgtbl to %p\n", call_arg->new_pgroot);
	asm volatile("mfence" ::: "memory");
	call_arg->done = true;
	asm volatile("mfence" ::: "memory");
}

bool cow_pipeline_copy_and_ipi = false;

static void snapshot_worker_tlb_flush(void * arg) {
	int cpu_id = dune_get_cpu_id();
	fast_spawn_state_t * state = dune_get_fast_spawn_state();
	//static int tlb_flush_counter = 0;
	volatile ipi_call_tlb_flush_arg_t* call_arg = (ipi_call_tlb_flush_arg_t*)arg;

	state->ipi_calls[cpu_id]++;
	dune_printf("snapshot_worker_tlb_flush cpu %d addr %p\n", cpu_id, call_arg->addr);
	while (!call_arg->ready_to_flush) {
		asm volatile("pause");
	}

	//asm volatile("mfence" ::: "memory");
	call_arg->flushed = true;
	dune_flush_tlb_one(call_arg->addr); // we can first respond to the sender first, then flush the address.

}

static bool pte_compare_exchange(ptent_t *pte, ptent_t old_pte, ptent_t new_pte) {
	return __atomic_compare_exchange_n(pte, &old_pte, new_pte, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

int* null_ptr = NULL;
static void dbos_tlb_shootdown(uint64_t addr, int source_core, int target_core, volatile ipi_call_tlb_flush_arg_t* call_arg, bool wait_for_flush) {
	uint64_t start_2= rdtscll();

	volatile struct dune_pv_info * pv_info;
	uint64_t pv_info_flag;
	fast_spawn_state_t * state = dune_get_fast_spawn_state();
	uint64_t wait_loops = 0;
	++wait_loops;
	pv_info = dune_pv_info_for_core(target_core);

	pv_info_flag = pv_info->flag;
	if ((pv_info_flag & DUNE_PV_TLB_IN_HOST) && ((pv_info_flag & DUNE_PV_TLB_FLUSH_ON_REENTRY) || 
		dune_pv_info_cmpswap(pv_info, pv_info_flag, pv_info_flag | DUNE_PV_TLB_FLUSH_ON_REENTRY))) {
		uint64_t end_2 = rdtscll();
		state->cycles_spent_for_shootdown[source_core] += end_2-start_2;
		state->shootdowns[source_core]++;
		return;
	}
	assert(dbos_fast_spawn_enabled);
	ipi_message_t msg;
	msg.func = snapshot_worker_tlb_flush;
	msg.type = CALL_TYPE_BATCHABLE;
	msg.arg = (void*)call_arg;
	
	// uint64_t start = rdtscll();
	uint64_t call_num =  dune_queue_ipi_call_fast(source_core, target_core, msg);
	if (call_num == IPI_INVALID_NUMBER) {
		dune_send_ipi(source_core, target_core);
		pv_info_flag = pv_info->flag;
		uint64_t end = rdtscll();
		//state->cycles_spent_for_shootdown[source_core] += end-start_2;
		state->ipi_call_queue_full[source_core] += 1;
		//state->shootdowns[source_core]++;

		
		if (!(pv_info_flag & DUNE_PV_TLB_IN_HOST)) { // HACK: In guest and somehow we could not trigger the interrupt to drain the buffer, kick the vCPU via hypercall and do a full flush
			uint64_t old_full_flush_count = pv_info->full_flush_count;
			while(!dune_pv_info_cmpswap(pv_info, pv_info_flag, pv_info_flag | DUNE_PV_TLB_FLUSH_ON_REENTRY)) {
				pv_info_flag = pv_info->flag;
			}
			// kick the target core
			dune_pv_kick(target_core);
			// Wait for the target core to finish VMExit and ack that it is about to do a flush
			while (old_full_flush_count == pv_info->full_flush_count) {

			}
			// Now we are sure the remote core will definitely invalidate the TLB @ addr
			uint64_t end = rdtscll();
			state->cycles_spent_for_shootdown[source_core] += end-start_2;
			state->shootdowns[source_core]++;
			return;
		} else {
			// In Host, try setting the full flush bit for the target core
			while (!((pv_info_flag & DUNE_PV_TLB_FLUSH_ON_REENTRY) ||
				dune_pv_info_cmpswap(pv_info, pv_info_flag, pv_info_flag | DUNE_PV_TLB_FLUSH_ON_REENTRY))) {
				pv_info_flag = pv_info->flag;
			}
			uint64_t end = rdtscll();
			state->cycles_spent_for_shootdown[source_core] += end-start_2;
			state->shootdowns[source_core]++;
			return;
		}

	}
	dune_send_ipi(source_core, target_core);
	assert(call_num != IPI_INVALID_NUMBER);

	if (wait_for_flush) {
		wait_loops = 0;
		//pv_info_flag = pv_info->flag;
		while(call_arg->flushed == false) {
			pv_info_flag = pv_info->flag;

			if ((pv_info_flag & DUNE_PV_TLB_IN_HOST) && ((pv_info_flag & DUNE_PV_TLB_FLUSH_ON_REENTRY) ||  // the bit is already on
					dune_pv_info_cmpswap(pv_info, pv_info_flag, pv_info_flag | DUNE_PV_TLB_FLUSH_ON_REENTRY))) { // set the FLUSH on Re-entry bit
				break;
			}

			//asm volatile("pause");
			// if (++wait_loops >= 100000000UL) {
			// 	uint64_t head = dune_queue_ipi_call_head(source_core, target_core);
			// 	uint64_t tail = dune_queue_ipi_call_tail(source_core, target_core);
			// 	bool is_call_retrived = dune_queue_ipi_call_empty(source_core, target_core);
			// 	int target_ipi_call_phase = dune_queued_ipi_phase(target_core);
			// 	dune_printf("Waited for too much time on source core %d to target core %d, is_call_retrived %d, target_ipi_call_phase %d, call_num_collected %d, call_num %lu, head %lu, tail %lu\n", source_core, target_core, is_call_retrived, target_ipi_call_phase, call_num_collected, call_num, head, tail);
			// 	*null_ptr = 1;
			// 	exit(1);
			// 	//assert(false);
			// }
		}
	}
	uint64_t end = rdtscll();
	state->cycles_spent_for_shootdown[source_core] += end-start_2;
	state->shootdowns[source_core]++;
}

extern int pteflags_to_perm(uint64_t pte_flags, int level);
typedef struct async_copy_args {
	ptent_t** root1;
	ptent_t** root2;
}async_copy_args;

static int pgtable_cow_copy(const void *arg, ptent_t *pte, void *va, int level) {
	assert(dbos_fast_spawn_enabled);
	static int cnt = 0;
	if (cnt++ == 0) {
		printf("Regions to avoid cow [%p,%p], [%p,%p], [%p, %p], [%p,%p]\n", IPI_ADDR_RING_BASE, IPI_ADDR_SHARED_STATE_END, 
					APIC_BASE, APIC_BASE + PGSIZE, POSTED_INTR_DESCS_BASE, POSTED_INTR_DESCS_BASE + 104 * PGSIZE,
					dune_pmem_alloc_begin(), dune_pmem_alloc_end());
	}
	int ret;
	async_copy_args * args = (async_copy_args *)arg;
	//ptent_t *newRoot = (ptent_t *)arg;
	ptent_t *newRoot1 = *args->root1;
	ptent_t *newRoot2 = *args->root2;
	ptent_t *newPte1;
	ptent_t *newPte2;
	void* can_va = (void*)canonical_virtual_address(va);
	// if (can_va != va) {
	// 	dune_printf("can_va %p != va %p, *pte %p\n", can_va, va, *pte);
	// }
	if ((((uint64_t)can_va) >= PAGEBASE && ((uint64_t)can_va) < PAGEBASE_END)) {
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		int perm = pteflags_to_perm(PTE_FLAGS(*pte), level);

		ret = dune_vm_map_phys_with_pte_flags(newRoot1, can_va, pgsize(level), (void*)PTE_ADDR(*pte), PTE_FLAGS(*pte), perm);

		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_map_phys failed with %d\n", ret);
			return ret;
		}
		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);

		ret = dune_vm_map_phys_with_pte_flags(newRoot2, can_va, pgsize(level), (void*)PTE_ADDR(*pte), PTE_FLAGS(*pte), perm);

		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_map_phys failed with %d\n", ret);
			return ret;
		}

		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);
	} else if (((((uint64_t)can_va) >= IPI_ADDR_RING_BASE && ((uint64_t)can_va) < IPI_ADDR_SHARED_STATE_END)) ||
		((uint64_t)can_va) >= APIC_BASE ||
		(((uint64_t)can_va) >= POSTED_INTR_DESCS_BASE && (uint64_t)can_va < POSTED_INTR_DESCS_BASE + 104 * PGSIZE) ||
		(((uint64_t)can_va) >= dune_pmem_alloc_begin() && (uint64_t)can_va < dune_pmem_alloc_end()) ) {
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		// We do not apply cow on addr range [IPI_ADDR_RING_BASE, IPI_ADDR_SHARED_STATE_END)
		// as this region is shared by main threads and snapshot threads for IPI purpose.
		//printf("va %p can_va %p in ipi range, pte %lx, level %d\n", va, can_va, *pte, level);
		assert(level == 0);
		ret = dune_vm_lookup(newRoot1, can_va, CREATE_NORMAL, &newPte1);
		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_lookup 1 failed with %d\n", ret);
			return ret;
		}

		*newPte1 = *pte & (~PTE_A) & (~PTE_LOCK); 

		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);
		
		ret = dune_vm_lookup(newRoot2, can_va, CREATE_NORMAL, &newPte2);
		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_lookup 1 failed with %d\n", ret);
			return ret;
		}

		*newPte2 = *pte & (~PTE_A) & (~PTE_LOCK);

		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);
	} else {
		assert(level == 0);
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		ret = dune_vm_lookup(newRoot1, can_va, CREATE_NORMAL, &newPte1);
		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_lookup 2 failed with %d\n", ret);
			return ret;
		}
		ret = dune_vm_lookup(newRoot2, can_va, CREATE_NORMAL, &newPte2);
		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_lookup 2 failed with %d\n", ret);
			return ret;
		}
		if ((*pte & PTE_W) && (*pte & PTE_P) && !(*pte & PTE_STACK) && !(*pte & PTE_COW) && !(*pte & PTE_NOCOW)) { // Ignore pages that are originally not writable or pages that are already marked with CoW bit. 
			*newPte1 = ((*pte & ~(PTE_W)) | PTE_COW) & (~PTE_LOCK); // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
			*newPte2 = ((*pte & ~(PTE_W)) | PTE_COW) & (~PTE_A) & (~PTE_LOCK);
		} else {
			*newPte1 = *pte & (~PTE_LOCK);
			*newPte2 = *pte & (~PTE_A) & (~PTE_LOCK);
		}

		if (dune_page_isfrompool(PTE_ADDR(*pte))){
			dune_page_get(pg);
			dune_page_get(pg);
		}
	}
	return 0;
}

#define OVERLAPS_LR(addr1_start, addr1_end, addr2_start, addr2_end) \
    (((uint64_t)(addr2_start) >= (uint64_t)(addr1_start) && \
      (uint64_t)(addr2_start) < (uint64_t)(addr1_end)))

#define OVERLAPS(addr1_start, addr1_end, addr2_start, addr2_end) \ 
	(OVERLAPS_LR(addr1_start, addr1_end, addr2_start, addr2_end) || \
		OVERLAPS_LR(addr2_start, addr2_end, addr1_start, addr1_end))

struct fast_clone_args{
	ptent_t *newRoot;
	int pages_copied;
	int pages_batch_copied;
};

#define LIBDBOS_MIN(a, b) ((a) < (b) ? (a) : (b))



static int pgtable_cow_copy_one(const void *arg, ptent_t *pte, void *va, int level) {
	assert(dbos_fast_spawn_enabled);
	static int cnt = 0;
	if (cnt++ == 0) {
		printf("Regions to avoid cow [%p,%p], [%p,%p], [%p, %p], [%p,%p]\n", IPI_ADDR_RING_BASE, IPI_ADDR_SHARED_STATE_END, 
					APIC_BASE, APIC_BASE + PGSIZE, POSTED_INTR_DESCS_BASE, POSTED_INTR_DESCS_BASE + 104 * PGSIZE,
					dune_pmem_alloc_begin(), dune_pmem_alloc_end());
	}
	int ret;
	ptent_t *newRoot1 = (ptent_t *)arg;
	ptent_t *newPte1;
	void* can_va = (void*)canonical_virtual_address(va);
	// if (can_va != va) {
	// 	dune_printf("can_va %p != va %p, *pte %p\n", can_va, va, *pte);
	// }
	if ((((uint64_t)can_va) >= PAGEBASE && ((uint64_t)can_va) < PAGEBASE_END)) {
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		int perm = pteflags_to_perm(PTE_FLAGS(*pte), level);

		ret = dune_vm_map_phys_with_pte_flags(newRoot1, can_va, pgsize(level), (void*)PTE_ADDR(*pte), PTE_FLAGS(*pte), perm);

		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_map_phys failed with %d\n", ret);
			return ret;
		}
		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);

	} else if (((((uint64_t)can_va) >= IPI_ADDR_RING_BASE && ((uint64_t)can_va) < IPI_ADDR_SHARED_STATE_END)) ||
		((uint64_t)can_va) >= APIC_BASE ||
		(((uint64_t)can_va) >= POSTED_INTR_DESCS_BASE && (uint64_t)can_va < POSTED_INTR_DESCS_BASE + 104 * PGSIZE) ||
		(((uint64_t)can_va) >= dune_pmem_alloc_begin() && (uint64_t)can_va < dune_pmem_alloc_end()) ) {
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		// We do not apply cow on addr range [IPI_ADDR_RING_BASE, IPI_ADDR_SHARED_STATE_END)
		// as this region is shared by main threads and snapshot threads for IPI purpose.
		//printf("va %p can_va %p in ipi range, pte %lx, level %d\n", va, can_va, *pte, level);
		assert(level == 0);
		ret = dune_vm_lookup(newRoot1, can_va, CREATE_NORMAL, &newPte1);
		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_lookup 1 failed with %d\n", ret);
			return ret;
		}

		*newPte1 = *pte & (~PTE_A) & (~PTE_LOCK); 

		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);
	} else {
		assert(level == 0);
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		ret = dune_vm_lookup(newRoot1, can_va, CREATE_NORMAL, &newPte1);
		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_lookup 2 failed with %d\n", ret);
			return ret;
		}
		if ((*pte & PTE_W) && (*pte & PTE_P) && !(*pte & PTE_STACK) && !(*pte & PTE_COW) && !(*pte & PTE_NOCOW)) { // Ignore pages that are originally not writable or pages that are already marked with CoW bit. 
			*newPte1 = ((*pte & ~(PTE_W)) | PTE_COW) & (~PTE_LOCK); // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
		} else {
			*newPte1 = *pte & (~PTE_LOCK);
		}

		if (dune_page_isfrompool(PTE_ADDR(*pte))){
			dune_page_get(pg);
		}
	}
	((struct fast_clone_args *)arg)->pages_copied++;
	return 0;
}
static int static_array[1024*1024*256];
static int __dune_vm_clone_last_level_page_helper(const void *arg, ptent_t *dir, void *start_va, int level)
{
	assert(level == 0);
	// int ret;
	// ptent_t *newRoot = ((struct fast_clone_args *)arg)->newRoot;
	// ptent_t *newDir = NULL;
	// bool created = false;
	// ret = dune_vm_lookup_internal(newRoot, start_va, CREATE_NORMAL, &created, &newDir);

	// // memcpy(newDir, dir, PGSIZE);
	// for (int i = 0; i < 512; ++i) {
	// 	newDir[i] = dir[i];
	// 	newDir[i] |= (PTE_COW);
	// 	newDir[i] &= ~PTE_W;
	// }
	// assert(ret == 0);
	// ((struct fast_clone_args *)arg)->pages_copied++;

	// return ret;

	void *end_va = (char*)start_va + PGSIZE * 512;
	void* can_va_start = (void*)canonical_virtual_address(start_va);
	void* can_va_end = (void*)canonical_virtual_address(end_va);

	//assert(!OVERLAPS(can_va_start, can_va_end, IPI_ADDR_RING_BASE, IPI_ADDR_SHARED_STATE_END));
	assert(!OVERLAPS(can_va_start, can_va_end, APIC_BASE, APIC_BASE + 4095));
	//assert(!OVERLAPS(can_va_start, can_va_end, POSTED_INTR_DESCS_BASE, POSTED_INTR_DESCS_BASE + 104 * PGSIZE));
	assert(!OVERLAPS(can_va_start, can_va_end, PAGEBASE, PAGEBASE_END));
	//assert(!OVERLAPS(can_va_start, can_va_end, dune_pmem_alloc_begin(), dune_pmem_alloc_end()));

	int ret = 0;
	ptent_t *newRoot1 = (ptent_t *)arg;
	ptent_t *newDir = NULL;
	// if (can_va != va) {
	// 	dune_printf("can_va %p != va %p, *pte %p\n", can_va, va, *pte);
	// }

	if (OVERLAPS(can_va_start, can_va_end, POSTED_INTR_DESCS_BASE, POSTED_INTR_DESCS_BASE + 104 * PGSIZE)) {
		for (int i = 0; i < 104; ++i) {
			pgtable_cow_copy_one(arg, &dir[i], (void*)(POSTED_INTR_DESCS_BASE + i * PGSIZE), level);
		}
	} else if (OVERLAPS(can_va_start, can_va_end, dune_pmem_alloc_begin(), dune_pmem_alloc_end())) {
		int i = 0;
		for (uint64_t start_addr = can_va_start; start_addr < LIBDBOS_MIN(dune_pmem_alloc_end(), can_va_end); start_addr += PAGE_SIZE) {
			pgtable_cow_copy_one(arg, &dir[i++], (void*)(start_addr), level);
		}
	} else if (OVERLAPS(can_va_start, can_va_end, IPI_ADDR_RING_BASE, IPI_ADDR_SHARED_STATE_END)) {
		int i = 0;
		for (uint64_t start_addr = can_va_start; start_addr < IPI_ADDR_SHARED_STATE_END; start_addr += PAGE_SIZE) {
			pgtable_cow_copy_one(arg, &dir[i++], (void*)(start_addr), level);
		}
	}
	else {
		bool created = false;
		ret = dune_vm_lookup2(newRoot1, can_va_start, CREATE_NORMAL, &created, &newDir);
		assert(newDir != NULL);
		//memcpy(newDir, dir, PGSIZE);
		for (int i = 0; i < 512; ++i) {
			// physaddr_t pte_addr = PTE_ADDR(dir[i]);
			// static_array[pte_addr % (1024*1024*256)]++;
			ptent_t*pte = &newDir[i];
			*pte = dir[i];
			if ((*pte & PTE_W) && (*pte & PTE_P) && !(*pte & PTE_STACK) && !(*pte & PTE_COW) && !(*pte & PTE_NOCOW)) { // Ignore pages that are originally not writable or pages that are already marked with CoW bit. 
				*pte = ((*pte & ~(PTE_W)) | PTE_COW) & (~PTE_LOCK); // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
			} else {
				*pte = *pte & (~PTE_LOCK);
			}
		}

		((struct fast_clone_args*)arg)->pages_batch_copied += 512;
	}

	return 0;
}

ptent_t * dune_vm_my_fast_clone(ptent_t *root) {
	int ret;
	struct fast_clone_args args;
	ptent_t *newRoot;

	newRoot = dune_alloc_page_internal();
	memset(newRoot, 0, PGSIZE);
	args.newRoot = newRoot;
	args.pages_copied = 0;
	args.pages_batch_copied = 0;

	ret = __dune_vm_page_walk_batch(root, VA_START, VA_END, &pgtable_cow_copy_one, &__dune_vm_clone_last_level_page_helper,
							  &args, 3, CREATE_NONE);
	if (ret < 0) {
		dune_vm_free(newRoot);
		return NULL;
	}
	printf("%d pages batch copied, %d pages copied\n", args.pages_batch_copied, args.pages_copied);

	return newRoot;
}
/**
 * Clone a page root.
 */
struct dbos_snapshot* dbos_fast_spawn(ptent_t *root)
{
	fast_spawn_state_t* state = dune_get_fast_spawn_state();
	struct dbos_snapshot* snapshot = &state->snapshots[state->num_snapshots++];
	snapshot->active = 0;

	
	snapshot->snapshot_table = dune_vm_my_fast_clone(root);
	snapshot->epoch = ++state->E_global;
	return snapshot;
}


/**
 * Clone a page root.
 */
static void dune_vm_my_clone(ptent_t *root, async_copy_args * args, page_walk_cb cb)
{
	int ret;
	//dune_printf("args->root1 %p, args->root2 %p\n", args->root1, args->root2);
	*(args->root1) = (ptent_t*) dune_alloc_page_internal();
	memset(*(args->root1), 0, PGSIZE);
	*(args->root2) = (ptent_t*) dune_alloc_page_internal();
	memset(*(args->root2), 0, PGSIZE);

	ret = dune_vm_page_walk(root, VA_START, VA_END, cb,
							  args);
	if (ret < 0) {
		dune_vm_free(*(args->root1));
		dune_vm_free(*(args->root2));
	}

	dune_printf("args->root1 %p, args->root2 %p\n", args->root1, args->root2);
	// *(args->root1) = dune_vm_my_fast_clone(root);
	// *(args->root2) = dune_vm_my_fast_clone(root);
}

static void* dbos_fast_spawn_snapshot_worker(void* arg) {
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	pthread_setname_np(pthread_self(), "dbos_fast_spawn_snapshot_worker");
	printf("dbos_fast_spawn_snapshot_worker: started on core %d, pgroot %p\n", cpu_id, pgroot);
	struct timeval start, end;
	asm volatile("mfence" ::: "memory");
	volatile int ret = dune_enter();
	if (ret) {
		printf("t_snapshot: failed to enter dune\n");
		return NULL;
	}
	printf("dbos_fast_spawn_snapshot_worker: entered dune\n");
	assert(sched_getcpu() == dune_ipi_get_cpu_id());
	volatile fast_spawn_state_t* state = dune_get_fast_spawn_state();
	state->snapshot_thread_tid = gettid();
	state->snapshot_worker_ready = true;
	asm volatile("mfence" ::: "memory");
	printf("Snapshot Thread running on core %d, fast_spawn_state %p\n", cpu_id, state);

	assert(cpu_id == SNAPSHOT_CORE);
	//sleep(10000);
	//pthread_mutex_lock(&state->mutex);
	while(state->tearing_down == false) {
		while(state->snapshot_user_work_func == NULL && state->tearing_down == false) {
			//dune_printf("snapshot_user_work_func waiting for work\n");
			//pthread_cond_wait(&state->cond, &state->mutex);	
		}
		if (state->tearing_down) {
			//pthread_mutex_unlock(&state->mutex);
			break;
		}
		assert(state->pgroot_snapshot);
		dune_pv_enable_record_stats();
		load_cr3((unsigned long)state->pgroot_snapshot);
		asm volatile("mfence" ::: "memory");
		state->snapshot_runs_started_version++;
		dune_set_thread_state(state, cpu_id, DUNE_THREAD_SNAPSHOT);
		asm volatile("mfence" ::: "memory");
		//pthread_mutex_unlock(&state->mutex);
		dune_printf("snapshot_user_work_func started\n");
		gettimeofday(&start, NULL);
		state->snapshot_user_work_func(state->snapshot_user_work_func_arg);
		gettimeofday(&end, NULL);

		//pthread_mutex_lock(&state->mutex);
		state->snapshot_runs_finished_version++;
		dune_set_thread_state(state, cpu_id, DUNE_THREAD_NONE);
		double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
		dune_printf("snapshot_user_work_func done, took %f seconds\n", elapsed);
		state->snapshot_user_work_func = NULL;
		state->snapshot_user_work_func_arg = NULL;
		state->pgroot_snapshot = NULL;
		dune_pv_disable_record_stats();
		asm volatile("mfence" ::: "memory");
	}
	// set COW bits on pgroot_snapshot
	
	//dune_vm_page_walk(state->pgroot_snapshot, VA_START, VA_END, pgtable_set_cow, NULL);
	// load the new page table
	// Now make sure the stack region is writable in the snapshot page table
	//dune_map_stack_with_2nd_pgtable(state->pgroot_snapshot);
	// load_cr3((unsigned long)state->pgroot_snapshot);
	// gettimeofday(&end, NULL);

    // double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
	// dune_printf("Switching page table took %.6f seconds, snapshot_user_work_func %p, arg %p\n", elapsed, state->snapshot_user_work_func, state->snapshot_user_work_func_arg);
	
	return NULL;
}

static void* page_table_copy_worker(void* arg) {
	printf("page_table_copy_worker started\n");
	pthread_setname_np(pthread_self(), "dbos_fast_spawn_page_table_copy_worker");
	assert(dbos_fast_spawn_enabled);
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	volatile int ret = dune_enter();
	if (ret) {
		printf("t_snapshot: failed to enter dune\n");
		return NULL;
	}
	assert(sched_getcpu() == dune_ipi_get_cpu_id());
	volatile fast_spawn_state_t * state = dune_get_fast_spawn_state();
	dune_set_thread_state(state, cpu_id, DUNE_THREAD_PGTBL_COPY);
	state->async_copy_worker_ready = true;
	asm volatile("mfence" ::: "memory");
	printf("Async PGTbl Copy Thread running on core %d, state %p\n", cpu_id, state);

	//dune_procmap_dump();

	//pthread_mutex_lock(&state->mutex);
	while (!state->tearing_down) {
		while (!state->async_copy_job && state->tearing_down == false) {
			//pthread_cond_wait (&state->cond, &state->mutex);
		}
		if (state->tearing_down) {
			//pthread_mutex_unlock(&state->mutex);
			break;
		}
		state->async_copy_job = false;
		asm volatile("mfence" ::: "memory");
		//ptent_t *pgroot = state->pgroot;
		//struct snapshot_task task = snapshot_task_queue.dequeue();
		printf("start cow_dup_pgtble\n");
        // pthread_mutex_lock(&state->mutex);
		ptent_t *pgroot_copy_from = state->pgroot;
		assert(state->shadow_pgroot1 == NULL);
		assert(state->shadow_pgroot2 == NULL);
		printf("start cow_dup_pgtble %p\n", pgroot_copy_from);
		struct timeval start, end;
    	gettimeofday(&start, NULL);
		ptent_t *root1 = NULL;
		ptent_t *root2 = NULL;
		ptent_t *pgroot = state->pgroot;
		async_copy_args args;
		args.root1 = &root1;
		args.root2 = &root2;
		dune_vm_my_clone(state->pgroot, &args, pgtable_cow_copy);
		state->shadow_pgroot1 = root1;
		state->shadow_pgroot2 = root2;
        //pthread_mutex_unlock(&state->mutex);
		pthread_cond_broadcast(&state->cond);
		gettimeofday(&end, NULL);
    	double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
		printf("done cow_dup_pgtble, took %.6f seconds, new root %p %p, copied from %p\n", elapsed, state->shadow_pgroot1, state->shadow_pgroot2, pgroot_copy_from);
		gettimeofday(&start, NULL);
		//dune_vm_clone_fast(state->pgroot);
		dune_vm_my_fast_clone(state->pgroot);
		gettimeofday(&end, NULL);
		elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
		printf("dune_vm_clone, took %.6f seconds\n", elapsed);
	}
	printf("page_table_copy_worker finished\n");
	dune_set_thread_state(state, cpu_id, DUNE_THREAD_NONE);
	return NULL;
}

pthread_t async_pgtbl_copy_pthread;
pthread_attr_t async_pgtbl_copy_pthread_attr;
cpu_set_t async_pgtbl_copy_pthread_cpus;

pthread_attr_t snapshot_worker_attr;
pthread_t snapshot_worker_pthread;
cpu_set_t snapshot_worker_cpus;

static void snapshot_worker_tlb_flush_batch(ipi_message_t*msg_buf, uint64_t n_msgs) {
	assert(dbos_fast_spawn_enabled);
	uint64_t addrs[IPI_MESSAGE_RINGBUF_SIZE * IPI_MAX_CPUS];
	bool flushed[IPI_MESSAGE_RINGBUF_SIZE * IPI_MAX_CPUS];
	int cpu_id = dune_get_cpu_id();
	fast_spawn_state_t * state = dune_get_fast_spawn_state();
	state->ipi_calls[cpu_id] += n_msgs;
	if (cow_pipeline_copy_and_ipi == true) {
		memset(flushed, false, n_msgs);
		while (true) {
			bool all_flushed = true;
			for (uint64_t i = 0; i < n_msgs; ++i) {
				if (flushed[i] == true) {
					continue;
				}
				volatile ipi_call_tlb_flush_arg_t* call_arg =  (ipi_call_tlb_flush_arg_t*)msg_buf[i].arg;
				if (call_arg->ready_to_flush == false) {
					all_flushed = false;
					continue;
				}
				call_arg->flushed = true;
				dune_flush_tlb_one(call_arg->addr);
				flushed[i] = true;
			}
			if (all_flushed == true) {
				break;
			}
		}
	} else {
		for (uint64_t i = 0; i < n_msgs; ++i) {
			volatile ipi_call_tlb_flush_arg_t* call_arg =  (ipi_call_tlb_flush_arg_t*)msg_buf[i].arg;
			addrs[i] = call_arg[i].addr;
			assert(call_arg->ready_to_flush == true);
			//dune_flush_tlb_one(addrs[i]);
			call_arg->flushed = true;
		}
		asm volatile("mfence" ::: "memory");
		for (uint64_t i = 0; i < n_msgs; ++i) {
			//volatile ipi_call_tlb_flush_arg_t* call_arg =  (ipi_call_tlb_flush_arg_t*)msg_buf[i].arg;
			dune_flush_tlb_one(addrs[i]); // we can first respond to the sender first, then flush the address.
			//call_arg->flushed = true;
		}
	}
}

bool dune_fast_spawn_snapshot_running() {
	int cpu_id = dune_ipi_get_cpu_id();
	fast_spawn_state_t* state = dune_get_fast_spawn_state();
	//pthread_mutex_lock(state->mutex);
	bool running = false;
	if (state->snapshot_runs_started_version > 0) {
		assert(state->snapshot_runs_finished_version <= state->snapshot_runs_started_version);
		if (state->snapshot_runs_finished_version < state->snapshot_runs_started_version) {
			assert(state->snapshot_runs_finished_version + 1 == state->snapshot_runs_started_version);
		}
		if (state->snapshot_runs_finished_version + 1 == state->snapshot_runs_started_version) {
			running = true;
		}
	}
	//pthread_mutex_unlock(state->mutex);
	return running;
}

void dune_fast_spawn_on_snapshot(void* (*snapshot_worker)(void*), void * arg) {
	int cpu_id = dune_ipi_get_cpu_id();
	fast_spawn_state_t* state = dune_get_fast_spawn_state();
	printf("branch off on core %d state %p, state->shadow_pgroot %p %p\n", cpu_id, state, state->shadow_pgroot1, state->shadow_pgroot2);
	asm volatile("mfence" ::: "memory");
	pthread_mutex_lock(&state->mutex);
	// We wait until copy has been done and the previous snapshot work finish
	while (state->shadow_pgroot1 == NULL || state->shadow_pgroot2 == NULL || state->snapshot_user_work_func) {
		pthread_cond_wait(&state->cond, &state->mutex);
	}
	printf("state->shadow_pgroot %p %p\n", state->shadow_pgroot1, state->shadow_pgroot2);
	asm volatile("mfence" ::: "memory");
    
	ptent_t* pgroot_snapshot = state->pgroot;

	state->pgroot = state->shadow_pgroot1;
	state->pgroot_snapshot = state->shadow_pgroot2;
	asm volatile("mfence" ::: "memory");

	int switched_count = 0;
	{
		bool sent_ipi[128];
		memset(sent_ipi, false, sizeof(sent_ipi));
		ipi_call_change_pgtbl_arg_t* call_args = (ipi_call_change_pgtbl_arg_t*)dune_ipi_percpu_working_page(cpu_id);
		for (int i = 0; i < 128; ++i) {
			if (i == cpu_id)
				continue;
			enum thread_state tstate = dune_get_thread_state(state, i);
			if (tstate == DUNE_THREAD_MAIN || tstate == DUNE_THREAD_PGTBL_COPY) {
				uint64_t new_cr3 = state->pgroot;
				volatile struct dune_pv_info * pv_info;
				uint64_t pv_info_flag;
				pv_info = dune_pv_info_for_core(i);
				pv_info_flag = pv_info->flag;
				if ((pv_info_flag & DUNE_PV_TLB_IN_HOST) && (
					dune_pv_info_cmpswap(pv_info, pv_info_flag, (pv_info_flag & DUNE_PV_FEATURE_MASK) | DUNE_PV_CHANGE_GUEST_CR3 | new_cr3))) {
					printf("pgtble switch (%p) posted to cpu %d\n", new_cr3, i);
					++switched_count;
					continue;
				}
				ipi_call_change_pgtbl_arg_t* call_arg = &(call_args[i]);
				call_arg->done = false;
				call_arg->new_pgroot = state->shadow_pgroot1;
				ipi_message_t msg;
				msg.func = change_pgtable_ipi_call;
				msg.arg = (void*)call_arg;
				msg.type = CALL_TYPE_NON_BATCHABLE;
				asm volatile("mfence" ::: "memory");
				dune_queue_ipi_call_fast(cpu_id, i, msg);
				//assert(res);
				dune_send_ipi(cpu_id, i);
				sent_ipi[i] = true;
			}
		}
		for (int i = 0; i < 128; ++i) {
			if (sent_ipi[i]) {
				volatile ipi_call_change_pgtbl_arg_t* call_arg = &call_args[i];
				while(!call_arg->done) {
					uint64_t new_cr3 = state->pgroot;
					volatile struct dune_pv_info * pv_info;
					uint64_t pv_info_flag;
					pv_info = dune_pv_info_for_core(i);
					pv_info_flag = pv_info->flag;
					if ((pv_info_flag & DUNE_PV_TLB_IN_HOST) && (
						dune_pv_info_cmpswap(pv_info, pv_info_flag, (pv_info_flag & DUNE_PV_FEATURE_MASK) | DUNE_PV_CHANGE_GUEST_CR3 | new_cr3))) {
						printf("pgtble switch (%p) posted to cpu %d\n", new_cr3, i);
						++switched_count;
						break;
					}
				}
				asm volatile("mfence" ::: "memory");
				//printf("pgtble switch on cpu %d done\n", i);
				++switched_count;
			}
		}
	}
	printf("pgtble switch done on %d workers, issued by %d\n", switched_count, cpu_id);
	// TODO: send ipi to other main CPUs to change their page table

	//pgroot = state->shadow_pgroot1;
	asm volatile("mfence" ::: "memory");

	// pthread_attr_init(&snapshot_worker_attr);
	// CPU_ZERO(&snapshot_worker_cpus);
	// CPU_SET(SNAPSHOT_CORE, &snapshot_worker_cpus);
	// pthread_attr_setaffinity_np(&snapshot_worker_attr, sizeof(cpu_set_t), &snapshot_worker_cpus);
	state->snapshot_user_work_func = snapshot_worker;
	state->snapshot_user_work_func_arg = arg;
	//pthread_create(&snapshot_worker_pthread, &snapshot_worker_attr, dbos_fast_spawn_snapshot_worker, NULL);
	// while(dune_get_fast_spawn_state()->get_thread_state(SNAPSHOT_CORE) != thread_state::SNAPSHOT);
	//dune_printf("main thread before load_cr3\n");

	// start pre-copying the page table for the next branch-off
	state->shadow_pgroot1 = NULL; // clear shadow pgroot
	state->shadow_pgroot2 = NULL; // clear shadow pgroot
	load_cr3((unsigned long)state->pgroot); // load the write-protected page table
	pgroot = state->pgroot;
	//printf("main thread after load_cr3\n");

	asm volatile("mfence" ::: "memory");
    pthread_mutex_unlock(&state->mutex);
	pthread_cond_broadcast(&state->cond);

	while (dune_get_thread_state(state, SNAPSHOT_CORE) != DUNE_THREAD_SNAPSHOT) {
		asm volatile("pause");
	}
	if (state->shadow_pgroot1 != NULL) {
		dune_printf("3333 state->shadow_pgroot1 %p\n", state->shadow_pgroot1);
	}
//	assert(state->shadow_pgroot1 == NULL);
//	assert(state->shadow_pgroot2 == NULL);
	state->async_copy_job = true;
	pthread_cond_broadcast(&state->cond);
	printf("dune snapshot work started\n");
}

void dune_fast_spawn_signal_copy_worker() {
	fast_spawn_state_t* state = dune_get_fast_spawn_state();
	state->async_copy_job = true;
	pthread_cond_broadcast(&state->cond);
}

void dbos_fast_spawn_pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	unsigned long start = rdtscll();
	ptent_t *pte = NULL;

	uint64_t cr3;
    __asm__ __volatile__ (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %0\n\t"
    : "=m" (cr3)
    : /* no input */
    : "%rax"
    );
	ptent_t *pgtbl_root = (ptent_t *)cr3;
	int rc;
	fast_spawn_state_t* state = dune_get_fast_spawn_state();
	
	int cpu_id = dune_ipi_get_cpu_id();
	bool snapshot = false;
	//state->interrupt_ts[cpu_id] = start;
	int retries = 0;
	retry:
	++retries;
	//dune_printf("pgflt on core %d addr %p ip %p fec %d rsp %p retries %d, pgtbl_root %p, state->pgroot %p, state->pgroot_snapshot %p\n", cpu_id, addr, tf->rip, fec, tf->rsp, retries, pgtbl_root, state->pgroot, state->pgroot_snapshot);
	ptent_t * pgroot_snapshot = state->pgroot_snapshot;
	bool snapshot_pgtable_exists = pgroot_snapshot != NULL;
	if (cpu_id == SNAPSHOT_CORE) {
		snapshot = true;
		if (snapshot_pgtable_exists) {
			if (pgroot_snapshot != pgtbl_root) {
				dune_printf("pgroot_snapshot %px, pgtbl_root %px\n", pgroot_snapshot, pgtbl_root);
			}
			assert(pgroot_snapshot == pgtbl_root);
		}
		//pgtbl_root = state->pgroot_snapshot;
		assert(pgtbl_root != state->pgroot);
		//dune_printf("pgflt on snapshot cpu %d pgroot %p va %p fec %lu RIP 0x%016llx\n", SNAPSHOT_CORE, state->pgroot_snapshot, addr, fec, tf->rip);
		rc = dune_vm_lookup(pgtbl_root, (void *)addr, 0, &pte);
	} else {
		//pgtbl_root = state->pgroot; 
		assert(pgtbl_root == state->pgroot);
		//dune_printf("pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
		rc = dune_vm_lookup(pgtbl_root, (void *)addr, 0, &pte);
		//dune_printf("1 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
	}
	assert(rc == 0);
	ptent_t old_pte = *pte;
	//dune_printf("pgflt on core %d addr %p ip %p fec %d old_pte flags %p pgtbl_root %p\n", cpu_id, addr, tf->rip, fec, PTE_FLAGS(old_pte), pgtbl_root);

	bool need_local_flush = true;
	if ((fec & FEC_W) && (old_pte & PTE_COW) && !(old_pte & PTE_W) && !(old_pte & PTE_LOCK)) {
		//dune_printf("2 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
		ptent_t new_pte = old_pte | PTE_LOCK;
		if (!pte_compare_exchange(pte, old_pte, new_pte)) {
			goto out;
		}
		//*pte = new_pte;
		//dune_printf("3 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
		physaddr_t pte_addr = PTE_ADDR(old_pte);
		void *newPage;
		struct page *pg = dune_pa2page(pte_addr);
		ptent_t perm = PTE_FLAGS(old_pte);
		//assert(!(perm & PTE_W));
		// Compute new permissions
		perm &= ~PTE_COW;
		perm |= PTE_W;
		perm |= PTE_U | PTE_A | PTE_D;
		perm &= ~PTE_LOCK; // Unlock on write

		if (dune_page_isfrompool(pte_addr) && pg->ref == 1) {
			//dune_printf("paddr %p from pool\n", pte_addr);
			__atomic_store_n(pte, pte_addr | perm, __ATOMIC_SEQ_CST);
			goto out;
		}
		//*pte = pte_addr | (perm | PTE_LOCK);

		bool snapshot_thread_started = dune_get_thread_state(state, SNAPSHOT_CORE) == DUNE_THREAD_SNAPSHOT;
		if (snapshot) {
			// Duplicate page
			newPage = dune_alloc_page_internal();
			//dune_printf("snapshot core %d pgflt on va %p fec %lu RIP 0x%016llx COW, new page %p, snapshot_pgroot %p, pgroot %p\n",  cpu_id, addr, fec, tf->rip, PTE_ADDR(newPage), pgtbl_root, state->pgroot);
			if (PGADDR(addr) < 0x1000) {
				dune_printf("call_stack");
				void* callstack[128];
				int i, frames = backtrace(callstack, 128);
				char** strs = backtrace_symbols(callstack, frames);
				for (i = 0; i < frames; ++i) {
					dune_printf("%s\n", strs[i]);
				}
			}
			memcpy(newPage, (void *)PGADDR(addr), PGSIZE);
			// Map page
			//*pte = PTE_ADDR(newPage) | perm;
			//*pte = 0;
			__atomic_store_n(pte, PTE_ADDR(newPage) | perm, __ATOMIC_SEQ_CST);
			
			if (dune_page_isfrompool(pte_addr)) {
				dune_page_put(pg);
			}
		} else if (snapshot_pgtable_exists) { // if there is not snapshot table in the system, simply remove the write-protection bit
			//dune_flush_tlb_one(addr);
			//assert(state->get_thread_state(SNAPSHOT_CORE) == thread_state::SNAPSHOT);
			ptent_t *snapshot_pte = NULL;
			//dune_printf("4 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
			rc = dune_vm_lookup(pgroot_snapshot, (void *)addr, 0, &snapshot_pte);
			assert(pte != snapshot_pte);
			//assert(rc == 0);
			ptent_t old_snapshot_pte = *snapshot_pte;
			ptent_t new_snapshot_pte = old_snapshot_pte | PTE_LOCK;
			if (!pte_compare_exchange(snapshot_pte, old_snapshot_pte, new_snapshot_pte)) { // snapshot pte was locked somehow, retry
				perm |= PTE_COW; // restore cow bit and write-protection bit for retry
				perm &= ~PTE_W;
				perm &= ~PTE_LOCK;
				__atomic_store_n(pte, pte_addr | perm, __ATOMIC_SEQ_CST);
				goto retry;
			}
			// locked snapshote_pte
			ptent_t snapshot_pte_perm = PTE_FLAGS(new_snapshot_pte);
			physaddr_t snapshot_pte_addr = PTE_ADDR(new_snapshot_pte);
			bool accessed = new_snapshot_pte & PTE_A;
			bool need_shootdown = accessed;
			snapshot_pte_perm |= PTE_D | PTE_A;
			snapshot_pte_perm &= ~PTE_LOCK;
			if (pte_addr != snapshot_pte_addr) { // already diverged, skip
				__atomic_store_n(snapshot_pte, snapshot_pte_addr | snapshot_pte_perm, __ATOMIC_SEQ_CST);
			} else {
				// TODO decrement ref-count of physical page pointed by snapshot_pte
				snapshot_pte_perm |= PTE_W;
				snapshot_pte_perm &= ~PTE_COW;
				if (cow_pipeline_copy_and_ipi && snapshot_thread_started) {
					volatile ipi_call_tlb_flush_arg_t* call_arg = (ipi_call_tlb_flush_arg_t*)dune_ipi_percpu_working_page(cpu_id);
					call_arg->ready_to_flush = false;
					call_arg->flushed = false;
					call_arg->addr = addr;
					dbos_tlb_shootdown(addr, cpu_id, SNAPSHOT_CORE, call_arg, false); // send the ipi first
					// now do the copying
					newPage = dune_alloc_page_internal();
					memcpy(newPage, (void *)PGADDR(addr), PGSIZE);
					__atomic_store_n(snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					call_arg->ready_to_flush = true;
					asm volatile("mfence" ::: "memory");
					dune_flush_tlb_one(addr); // flush the tlb while waiting 
					while(!call_arg->flushed) {
						asm volatile("pause");
					}
					need_local_flush = false;
					//state->dbos_tlb_shootdowns++;
				} else {
					// if (cpu_id == 0) {
					// 	dune_printf("1 pgflt dune_alloc_page_internal on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
					// }
					
					newPage = dune_alloc_page_internal();
					// if (cpu_id == 0) {
					// 	dune_printf("1 pgflt after dune_alloc_page_internal on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
					// }
					//dune_printf("1 pgflt after dune_alloc_page_internal on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
					//memcpy((void*)&ts_pages[cpu_id], (void *)PGADDR(addr), PGSIZE);
					assert(newPage != NULL);
					if ((void *)PGADDR(addr) == NULL) {
						dune_printf("1 pgflt after dune_alloc_page_internal on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx RDX 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp, tf->rdx);
					}
					assert((void *)PGADDR(addr) != NULL);
					memcpy((void*)newPage, (void *)PGADDR(addr), PGSIZE);
					//memset(newPage, 0, PGSIZE);
					//__atomic_store_n(snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					if (!pte_compare_exchange(snapshot_pte, new_snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm)) {
						//dune_printf("cmpexg failed\n");
						need_shootdown = true;
						snapshot_pte_perm = PTE_FLAGS(*snapshot_pte);
						snapshot_pte_perm |= PTE_W;
						snapshot_pte_perm |= PTE_D | PTE_A;
						snapshot_pte_perm &= ~PTE_COW;
						snapshot_pte_perm &= ~PTE_LOCK;
						__atomic_store_n(snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					}
					//__atomic_store_n(snapshot_pte, snapshot_pte_addr | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					if (snapshot_thread_started && (need_shootdown || !dbos_selective_TLB_shootdown )) {
						volatile ipi_call_tlb_flush_arg_t* call_arg = (ipi_call_tlb_flush_arg_t*)dune_ipi_percpu_working_page(cpu_id);
						call_arg->ready_to_flush = true;
						call_arg->flushed = false;
						call_arg->addr = addr;
						//uint64_t start = rdtscll();
						dbos_tlb_shootdown(addr, cpu_id, SNAPSHOT_CORE, call_arg, true);
						//uint64_t end = rdtscll();
						//dune_printf("flt %p cpu %d shootdown to %d state->pgroot %p state->pgroot_snapshot %p after\n", addr, cpu_id, SNAPSHOT_CORE, state->pgroot, state->pgroot_snapshot);
						//state->dbos_tlb_shootdowns++;
						//state->cycles_spent_for_shootdown[cpu_id] += end-start;
						//state->shootdowns[cpu_id]++;
					}
					state->thread_pgfaults[cpu_id]++;
				}
				if (dune_page_isfrompool(snapshot_pte_addr)) {
					dune_page_put(dune_pa2page(snapshot_pte_addr));
				}
			}
			//dune_printf("5 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
			//dune_printf("main pgflt cpu %d on va %p fec %lu RIP 0x%016llx COW, new page %p, snapshot thread started %d, sent ipi\n", cpu_id, addr, fec, tf->rip, newPage, snapshot_thread_started);
			// writer still uses the old physical page 
			
			//*pte = pte_addr | perm;

			//dune_flush_tlb_one(addr);
			//dune_printf("1 flt %p ip %p pte_addr %p cpu %d state->pgroot %p state->pgroot_snapshot %p before, old_perm %p new_perm %p\n", addr, tf->rip, pte_addr, cpu_id, state->pgroot, state->pgroot_snapshot, PTE_FLAGS(old_pte), perm);
			//dune_flush_tlb_one(addr);
			__atomic_store_n(pte, pte_addr | perm, __ATOMIC_SEQ_CST);
		} else {
			//dune_flush_tlb_one(addr);
			//dune_printf("2 flt %p ip %p pte_addr %p cpu %d state->pgroot %p state->pgroot_snapshot %p before, old_perm %p new_perm %p\n", addr, tf->rip, pte_addr, cpu_id, state->pgroot, state->pgroot_snapshot, PTE_FLAGS(old_pte), perm);
			//*pte = 0;
			//dune_flush_tlb_one(addr);
			__atomic_store_n(pte, pte_addr | perm, __ATOMIC_SEQ_CST);
		}
	}
out:
	// Invalidate
	if (need_local_flush) {
		dune_flush_tlb_one(addr);
	}
	//dune_flush_tlb();
	//dune_flush_tlb_one(addr);
	//state->page_fault_cycles += rdtscllp() - cycles_start;
	//uint64_t now = rdtscll();
	// state->cycles_spent_in_flt[cpu_id] += now - start;
	// assert(now > state->cycles_access_start[cpu_id]);
	// state->cycles_spent_all_access[cpu_id] += now - state->cycles_access_start[cpu_id];
	// state->cycles_spent_for_interrupt[cpu_id] += state->interrupt_ts[cpu_id] - state->cycles_access_start[cpu_id];
	return;
}


int pgtable_set_cow(const void *arg, ptent_t *pte, void *va, int level) {
	int ret;
	struct page *pg = dune_pa2page(PTE_ADDR(*pte));
	//dune_printf("pgtble_set_cow PTE_ADDR(*pte) %p, pte %p, va %p, level %d\n", PTE_ADDR(*pte), pte, va, level);
	uint64_t can_va = canonical_virtual_address(va);
	// if (can_va != va) {
	// 	dune_printf("can_va %p != va %p, *pte %p\n", can_va, va, *pte);
	// }
	if ((((uint64_t)can_va) >= PAGEBASE && ((uint64_t)can_va) < PAGEBASE_END)) {
		
	}  else if (((((uint64_t)can_va) >= IPI_ADDR_RING_BASE && ((uint64_t)can_va) < IPI_ADDR_SHARED_STATE_END)) ||
		((uint64_t)can_va) >= APIC_BASE ||
		(((uint64_t)can_va) >= POSTED_INTR_DESCS_BASE && (uint64_t)can_va < POSTED_INTR_DESCS_BASE + 104 * PGSIZE) ||
		(((uint64_t)can_va) >= dune_pmem_alloc_begin() && (uint64_t)can_va < dune_pmem_alloc_end()) ) {

	}  else {
		if ((*pte & PTE_W) && (*pte & PTE_P) && !(*pte & PTE_STACK) && !(*pte & PTE_COW) && !(*pte & PTE_NOCOW)) { // Ignore pages that are originally not writable or pages that are already marked with CoW bit. 
			*pte = ((*pte & ~(PTE_W)) | PTE_COW) & (~PTE_LOCK); // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
		}
	}

	return 0;
}

void dune_fast_spawn_configure() {
	if (dune_get_max_cores() < 3) {
		printf("dune_fast_spawn_init: at least 3 cores are needed for fast spawn");
		return -1;
	}
	ASYNC_COPY_CORE = dune_get_max_cores() - 1;
	SNAPSHOT_CORE = dune_get_max_cores() - 2;
	volatile fast_spawn_state_t* state = dune_get_fast_spawn_state();
	for (int i = 0 ;i < 128; ++i) {
		dune_set_thread_state(state, i, DUNE_THREAD_NONE);
		state->thread_pgfaults[i] = 0;
		state->cycles_spent_for_shootdown[i] = 0;
		state->ipi_call_queue_full[i] = 0;
		state->shootdowns[i] = 0;
		state->ipi_calls[i] = 0;
		state->cycles_spent_in_flt[i] = 0;
		state->write_calls[i] = 0;
		state->cycles_per_write_syscall[i] = 0;
		state->syscalls[i] = 0;
		state->cycles_between_consecutive_writes[i] = 0;
		state->ts_write_call_start[i] = 0;
		state->cycles_access_start[i] = 0;
		state->cycles_spent_all_access[i] = 0;
		state->cycles_spent_for_interrupt[i] = 0;
		state->interrupt_ts[i] = 0;
		state->cow_staging_buffer[i] = NULL;
		if (i == SNAPSHOT_CORE) {
			state->cow_staging_buffer[i] = mmap(0, DUNE_FAST_COW_STAGING_BUFFER_SIZE,  PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
			if (state->cow_staging_buffer[i] == MAP_FAILED) {
				dune_printf("dune: failed to allocate cow staging buffer\n");
				exit(1);
			}

			if (dune_vm_map_phys(pgroot, (void *)state->cow_staging_buffer[i], DUNE_FAST_COW_STAGING_BUFFER_SIZE,
						 (void *)dune_mmap_addr_to_pa(state->cow_staging_buffer[i]),
						 PERM_R | PERM_W | PERM_U | PERM_NOCOW)) {
				dune_printf("dune: failed to map cow staging buffer\n");
				exit(1);
			}
		}
	}
}


#include <sys/epoll.h>

static void dune_sys_patch_open(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	const char *filename = (const char *)ARG0(tf);
	size_t len = strlen(filename);
	assert(len + 1 <= kWorkspaceSize);

	if (filename == NULL) {
		dune_passthrough_g0_syscall(new_tf);
		tf->rax = new_tf->rax;
	} else {
		memcpy(workspace, filename, len);
		((char*)workspace)[len] = '\0';
		ARG0(new_tf) = workspace;
		dune_passthrough_g0_syscall(new_tf);
		tf->rax = new_tf->rax;
	}
	dune_printf("dune_sys_patch_open filename %s, len %d, return %d\n", filename, len, tf->rax);

}

static void dune_sys_patch_write(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	const void *buf = (const void *)ARG1(tf);
	size_t count = (size_t)ARG2(tf);
	assert(count <= kWorkspaceSize);
	memcpy(workspace, buf, count);
	ARG1(new_tf) = workspace;
	dune_passthrough_g0_syscall(new_tf);
	tf->rax = new_tf->rax;
}

static void dune_sys_patch_read(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	void *source_buf = (const void *)ARG1(tf);
	size_t count = (size_t)ARG2(tf);
	assert(count <= kWorkspaceSize);
	ARG1(new_tf) = workspace;
	dune_passthrough_g0_syscall(new_tf);
	ssize_t ret = new_tf->rax;
	if (ret > 0) {
		memcpy(source_buf, workspace, count);
	}
	tf->rax = ret;
}

static void dune_sys_patch_epoll_wait(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	struct epoll_event * events = (struct epoll_event *)ARG1(tf);
	int maxevents = (size_t)ARG2(tf);
	if (sizeof(struct epoll_event) * maxevents > kWorkspaceSize) {
		dune_printf("sizeof(struct epoll_event) %lu * maxevents %d > kWorkspaceSize %lu\n", sizeof(struct epoll_event), maxevents , kWorkspaceSize);
	}
	assert(sizeof(struct epoll_event) * maxevents <= kWorkspaceSize);
	ARG1(new_tf) = (uint64_t)workspace;
	dune_passthrough_g0_syscall(new_tf);
	ssize_t ret = new_tf->rax;
	if (ret > 0) {
		memcpy(events, workspace, sizeof(struct epoll_event) * ret);
	}
	tf->rax = new_tf->rax;
}

static void dune_fast_spawn_syscall_handler(struct dune_tf *tf)
{

	static const size_t kWorkspaceSize = DUNE_FAST_COW_STAGING_BUFFER_SIZE;
	//char workspace[kWorkspaceSize];
	int cpu_id = dune_get_cpu_id();
	fast_spawn_state_t* state = dune_get_fast_spawn_state();
	
	//state->syscalls[cpu_id]++;
	int syscall_num = (int) tf->rax;
	// if (syscall_num == 9) {
	// 	dune_printf("got mmap(addr=%lx, length=%lx, prot=%lx, flags=%lx, fd=%lx, offset=%lx), hugetlb?%s\n", tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9, (tf->r10 & MAP_HUGETLB) ? "yes" : "no");
	// }
	// if (syscall_num == 12) {
	// 	dune_printf("got brk(addr=%lx)\n", tf->rdi);
	// }
	// if (syscall_num == 10) {
	// 	dune_printf("Core %d got mprotect call %d, rip %p , arg0 %p , arg1 %p , arg2 %p\n", cpu_id, syscall_num, tf->rip, ARG0(tf), ARG1(tf), ARG2(tf));
	// }
	// we only patch system calls coming from the snapshot worker
	if (cpu_id != SNAPSHOT_CORE) { 
		dune_passthrough_g0_syscall(tf);
		return;
	}
	char * workspace = state->cow_staging_buffer[cpu_id];
	assert(workspace != NULL);
	// if (syscall_num != SYS_read && syscall_num != SYS_write) {
	// 	dune_printf("Core %d got system call %d, workspace %p, rip %p, arg0 %p , arg1 %p , arg2 %p\n", cpu_id, syscall_num, workspace, tf->rip, ARG0(tf), ARG1(tf), ARG2(tf));
	// }
	// if (!(tf->rflags & 0x200)) {
	// 	dune_printf("Core %d got system call %d, workspace %p, rip %p, arg0 %p , arg1 %p , arg2 %p, interrupt disabled, trying to set it\n", cpu_id, syscall_num, workspace, tf->rip, ARG0(tf), ARG1(tf), ARG2(tf));
	// 	//tf->rflags |= 0x200;
	// }
	
	struct dune_tf new_tf = *tf;
	struct dune_tf * new_tfp = &new_tf;
	uint64_t now = rdtscll();
	if (syscall_num == SYS_write) {
		if (state->ts_write_call_start[cpu_id] != 0) {
			state->cycles_between_consecutive_writes[cpu_id] += now - state->ts_write_call_start[cpu_id];
		}
		state->ts_write_call_start[cpu_id] = now;
		state->write_calls[cpu_id]++;
	}
	switch (syscall_num)
	{
	case SYS_open:
		dune_sys_patch_open(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	case SYS_write:
		dune_sys_patch_write(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	case SYS_read: 
		dune_sys_patch_read(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	case SYS_epoll_wait:
		dune_sys_patch_epoll_wait(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	default:
		//dune_printf("fast_spawn snapshot core %d passthrough syscall %d\n", cpu_id, syscall_num);
		dune_passthrough_g0_syscall(tf);
		break;
	}
	uint64_t syscall_latency = rdtscll() - now;
	if (syscall_num == SYS_write) {
		state->cycles_per_write_syscall[cpu_id] += syscall_latency;
		if (tf->rax == -1) {
			dune_printf("Core %d got system call %d, workspace %p, rip %p , arg0 %p , arg1 %p , arg2 %p, ret %p\n", cpu_id, syscall_num, workspace, tf->rip, ARG0(tf), ARG1(tf), ARG2(tf), tf->rax);
		}
	}
	assert(tf->rflags & 0x200);
	//dune_printf("Core %d got system call %d, workspace %p, rip %p , arg0 %p , arg1 %p , arg2 %p, ret %p\n", cpu_id, syscall_num, workspace, tf->rip, ARG0(tf), ARG1(tf), ARG2(tf), tf->rax);
	
}

int dune_fast_spawn_init(int selective_TLB_shootdown) {
	dbos_selective_TLB_shootdown = selective_TLB_shootdown;
	printf("setting async_copy_core to %d, snapshot_core to %d\n", ASYNC_COPY_CORE, SNAPSHOT_CORE);
	dune_register_pgflt_handler(dbos_fast_spawn_pgflt_handler);
	dune_register_ipi_batch_call(snapshot_worker_tlb_flush_batch);
	dune_register_g0_syscall_handler(dune_fast_spawn_syscall_handler);

	volatile fast_spawn_state_t* state = dune_get_fast_spawn_state();
    pthread_mutex_init(&state->mutex, NULL);
	pthread_cond_init(&state->cond, NULL);
	state->pgroot = pgroot;

	memset(state->snapshots, 0, sizeof(state->snapshots));
	state->num_snapshots = 0;
	state->E_global = 0;

	state->shadow_pgroot1 = NULL;
	state->shadow_pgroot2 = NULL;
	state->pgroot_snapshot = NULL;
	state->async_copy_job = false;
	state->snapshot_worker_ready = false;
	state->async_copy_worker_ready = false;
	state->dbos_tlb_shootdowns = 0;
	state->page_faults = 0;
	state->page_fault_cycles = 0;
	state->tearing_down = false;
	state->snapshot_runs_started_version = 0;
	state->snapshot_runs_finished_version = 0;
	state->snapshot_user_work_func = NULL;
	state->snapshot_user_work_func_arg = NULL;

	dbos_fast_spawn_enabled = true;
	asm volatile("mfence" ::: "memory");

	pthread_attr_init(&async_pgtbl_copy_pthread_attr);
	CPU_ZERO(&async_pgtbl_copy_pthread_cpus);
	CPU_SET(ASYNC_COPY_CORE, &async_pgtbl_copy_pthread_cpus);
	pthread_attr_setaffinity_np(&async_pgtbl_copy_pthread_attr, sizeof(cpu_set_t), &async_pgtbl_copy_pthread_cpus);
	int ret = pthread_create(&async_pgtbl_copy_pthread, &async_pgtbl_copy_pthread_attr, page_table_copy_worker, NULL);
	if (ret) {
		dune_printf("failed to create page_table_copy_worker %s", strerror(errno));
		exit(1);
	}
	printf("waiting for async_copy_worker to be ready, state %p\n", state);
	//sleep(100);

	asm volatile("mfence" ::: "memory");
	while(!state->async_copy_worker_ready) {
		//asm volatile("pause");
	}

	printf("async_copy_worker ready, start copying\n");

	pthread_attr_init(&snapshot_worker_attr);
	CPU_ZERO(&snapshot_worker_cpus);
	CPU_SET(SNAPSHOT_CORE, &snapshot_worker_cpus);
	pthread_attr_setaffinity_np(&snapshot_worker_attr, sizeof(cpu_set_t), &snapshot_worker_cpus);
	pthread_create(&snapshot_worker_pthread, &snapshot_worker_attr, dbos_fast_spawn_snapshot_worker, NULL);
	while (!state->snapshot_worker_ready) {

	}
	return 0;
}
