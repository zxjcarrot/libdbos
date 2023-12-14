#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <memory.h>

#include <vector>
#include <atomic>
#include <stdexcept>
#include "RandomGenerator.hpp"

#include "bench.h"

#ifdef __cplusplus
extern "C"{
#endif 

#include "dune.h"

void *asm_memcpy(void *dest, const void *src, size_t n);
void *asm_memset(void *s, int c, size_t n);

#ifdef __cplusplus
}
#endif

#define SNAPSHOT_CORE 13 
#define ASYNC_COPY_CORE 14
#define MB (1024 * 1024ULL)

unsigned long rdtsc_overhead;
static uint64_t debug_va_addr = 0;
volatile bool writer_ready[128];
uint64_t cow_region_begin;
uint64_t cow_region_end;
volatile uint64_t cycles_start;
enum class thread_state {
	NONE = 0,
	MAIN = 1,
	SNAPSHOT = 2,
	PGTBL_COPY = 3
};

typedef struct {
    char *memory;
    size_t size;
    size_t count;
	int thread_id;
	int num_threads;
	volatile bool *ready_to_go;
} ThreadArg;


typedef struct cow_state_t {
	ptent_t *pgroot;
	ptent_t *shadow_pgroot1; // one for main CPUs
	ptent_t *shadow_pgroot2; // one for snapshot
	ptent_t *pgroot_snapshot;
	bool async_copy_job;
	bool snapshot_worker_started;
	bool async_copy_worker_ready;
	thread_state thread_states[128];
	int tlb_shootdowns;
	int page_faults;
	uint64_t page_fault_cycles;

	thread_state get_thread_state(int cpu_id){return thread_states[cpu_id]; }
	void set_thread_state(int cpu_id, thread_state state){ thread_states[cpu_id] = state; }
}cow_state_t;

static inline uint64_t pgsize(int level) {
	if (level == 0) return PGSIZE;
	else if (level == 1) return PGSIZE * 512UL;
	else if (level == 2) return PGSIZE * 512UL * 512UL;
	assert(false);

	return -1;
}

uint64_t canonical_virtual_address(void * addr) {
	if ((uint64_t)addr < 0x800000000000) return (uint64_t)addr;
	else return (uint64_t)addr | 0xffff000000000000;
}

// states are shared across all page tables
inline cow_state_t* get_shared_cow_state() {
	return (cow_state_t*)dune_ipi_shared_state_page();
}

typedef struct ipi_call_change_pgtbl_arg_t {
	ptent_t *new_pgroot;
	volatile bool done;
} ipi_call_arg_t;

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
	dune_printf("change_pgtable_ipi_call on core %d called with arg %p, new_pgroot %p, rsp %p\n", cpu_id, call_arg, call_arg->new_pgroot, rsp);
	assert(call_arg->done == false);
	load_cr3((unsigned long)call_arg->new_pgroot);
	//dune_printf("async_copy_worker changed pgtbl to %p\n", call_arg->new_pgroot);
	asm volatile("mfence" ::: "memory");
	call_arg->done = true;
	asm volatile("mfence" ::: "memory");
}

bool cow_pipeline_copy_and_ipi = false;
static void snapshot_worker_tlb_flush(void * arg) {
	//static int tlb_flush_counter = 0;
	volatile ipi_call_tlb_flush_arg_t* call_arg = (ipi_call_tlb_flush_arg_t*)arg;

	while (!call_arg->ready_to_flush) {
		//asm volatile("pause");
	}

	call_arg->flushed = true;
	//asm volatile("mfence" ::: "memory");
	dune_flush_tlb_one(call_arg->addr); // we can first respond to the sender first, then flush the address.
}

static void snapshot_worker_tlb_flush_batch(ipi_message_t*msg_buf, uint64_t n_msgs) {
	uint64_t addrs[IPI_MESSAGE_RINGBUF_SIZE * IPI_MAX_CPUS];
	bool flushed[IPI_MESSAGE_RINGBUF_SIZE * IPI_MAX_CPUS];
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
		for (uint64_t i = 0; i < n_msgs; ++i) {
			dune_flush_tlb_one(addrs[i]); // we can first respond to the sender first, then flush the address.
		}
	}
}

// int pgtable_set_cow(const void *arg, ptent_t *pte, void *va, int level) {
// 	int ret;
// 	struct page *pg = dune_pa2page(PTE_ADDR(*pte));
	
// 	uint64_t can_va = canonical_virtual_address(va);

// 	if ((((uint64_t)can_va) >= PAGEBASE && ((uint64_t)can_va) < PAGEBASE_END)) {
		
// 	}  else if (((((uint64_t)can_va) >= IPI_ADDR_RING_BASE && ((uint64_t)can_va) < IPI_ADDR_SHARED_STATE_END)) ||
// 		((uint64_t)can_va) >= APIC_BASE ||
// 		(((uint64_t)can_va) >= POSTED_INTR_DESCS_BASE && (uint64_t)can_va < POSTED_INTR_DESCS_BASE + 104 * PGSIZE) ||
// 		(((uint64_t)can_va) >= dune_pmem_alloc_begin() && (uint64_t)can_va < dune_pmem_alloc_end()) ) {

// 	}  else {
// 		if ((*pte & PTE_W) && (*pte & PTE_P) && !(*pte & PTE_STACK) && !(*pte & PTE_COW)) { // Ignore pages that are originally not writable or pages that are already marked with CoW bit. 
// 			*pte = (*pte & ~(PTE_W)) | PTE_COW ; // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
// 		}
// 	}

// 	return 0;
// }


int pgtable_set_cow(const void *arg, ptent_t *pte, void *va, int level) {
	int ret;
	struct page *pg = dune_pa2page(PTE_ADDR(*pte));
	
	uint64_t can_va = canonical_virtual_address(va);

	if ((((uint64_t)can_va) >= PAGEBASE && ((uint64_t)can_va) < PAGEBASE_END)) {
		
	}  else if (((((uint64_t)can_va) >= IPI_ADDR_RING_BASE && ((uint64_t)can_va) < IPI_ADDR_SHARED_STATE_END)) ||
		((uint64_t)can_va) >= APIC_BASE ||
		(((uint64_t)can_va) >= POSTED_INTR_DESCS_BASE && (uint64_t)can_va < POSTED_INTR_DESCS_BASE + 104 * PGSIZE) ||
		(((uint64_t)can_va) >= dune_pmem_alloc_begin() && (uint64_t)can_va < dune_pmem_alloc_end()) ) {

	}  else {
		if ((uint64_t)va >= cow_region_begin && (uint64_t)va < cow_region_end) { // Ignore pages that are originally not writable or pages that are already marked with CoW bit. 
			*pte = ((*pte & ~(PTE_W)) | PTE_COW) & (~PTE_A) ; // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
		}
	}

	return 0;
}
bool pte_compare_exchange(ptent_t *pte, ptent_t old_pte, ptent_t new_pte) {
	return __atomic_compare_exchange_n(pte, &old_pte, new_pte, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void tlb_shootdown(uint64_t addr, int source_core, int target_core, volatile ipi_call_tlb_flush_arg_t* call_arg, bool wait_for_flush) {
	ipi_message_t msg;
	msg.func = snapshot_worker_tlb_flush;
	msg.type = CALL_TYPE_BATCHABLE;
	msg.arg = (void*)call_arg;
	bool res = dune_queue_ipi_call_fast(source_core, target_core, msg);
	// We have queued the function call in shared memory
	// See if we really need to send interrupt.
	if (dune_queued_ipi_need_interrupt(target_core)) {
		dune_send_ipi(source_core, target_core);
	}
	if (wait_for_flush) {
		while(!call_arg->flushed) {
			//asm volatile("pause");
		}
	}
}
struct page_ {
	char d[4096];
}ts_pages[128];

//static __thread struct page_ ts_p;

void vm_pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	//unsigned long start = rdtscllp();
	retry:

	ptent_t *pte = NULL;
	int rc;

	cow_state_t* state = get_shared_cow_state();

	//state->page_faults++;
	int cpu_id = dune_ipi_get_cpu_id();
	bool snapshot = false;
	if (cpu_id == SNAPSHOT_CORE) {
		snapshot = true;
		//dune_printf("pgflt on snapshot cpu %d pgroot %p va %p fec %lu RIP 0x%016llx\n", SNAPSHOT_CORE, state->pgroot_snapshot, addr, fec, tf->rip);
		rc = dune_vm_lookup(state->pgroot_snapshot, (void *)addr, 0, &pte);
	} else {
		//dune_printf("pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
		rc = dune_vm_lookup(state->pgroot, (void *)addr, 0, &pte);
		//dune_printf("1 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
	}
	if (rc != 0) {
		dune_printf("pgflt on cpu %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
	}
	assert(rc == 0);
	
	ptent_t old_pte = *pte;

	bool need_local_flush = true;
	if ((fec & FEC_W) && (*pte & PTE_COW) && !(old_pte & PTE_W)) {
		//dune_printf("2 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
		ptent_t new_pte = old_pte | PTE_LOCK;
		if (!pte_compare_exchange(pte, old_pte, new_pte)) {
			//dune_printf("cmpexg failed\n");
			goto out;
		}
		//dune_printf("3 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
		physaddr_t pte_addr = PTE_ADDR(old_pte);
		void *newPage;
		struct page *pg = dune_pa2page(pte_addr);
		ptent_t perm = PTE_FLAGS(*pte);
		//assert(!(perm & PTE_W));
		// Compute new permissions
		perm &= ~PTE_COW;
		perm |= PTE_W;
		perm &= ~PTE_LOCK; // Unlock on write

		if (dune_page_isfrompool(pte_addr) && pg->ref == 1) {
			//dune_printf("paddr %p from pool\n", pte_addr);
			__atomic_store_n(pte, pte_addr | perm, __ATOMIC_SEQ_CST);
			goto out;
		}
		//*pte = pte_addr | (perm | PTE_LOCK);

		if (snapshot) {
			// Duplicate page
			newPage = dune_alloc_page_internal();
			memcpy(newPage, (void *)PGADDR(addr), PGSIZE);
			// Map page
			if (dune_page_isfrompool(pte_addr)) {
				dune_page_put(pg);
			}
			//*pte = PTE_ADDR(newPage) | perm;
			__atomic_store_n(pte, PTE_ADDR(newPage) | perm, __ATOMIC_SEQ_CST);
			//dune_printf("snapshot pgflt on va %p fec %lu RIP 0x%016llx COW, new page %p\n", addr, fec, tf->rip, newPage);
		} else {
			//assert(state->get_thread_state(SNAPSHOT_CORE) == thread_state::SNAPSHOT);
			ptent_t *snapshot_pte = NULL;
			//dune_printf("4 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
			rc = dune_vm_lookup(state->pgroot_snapshot, (void *)addr, 0, &snapshot_pte);
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
			bool snapshot_thread_started = state->get_thread_state(SNAPSHOT_CORE) == thread_state::SNAPSHOT;
			// locked snapshote_pte
			ptent_t snapshot_pte_perm = PTE_FLAGS(new_snapshot_pte);
			physaddr_t snapshot_pte_addr = PTE_ADDR(new_snapshot_pte);
			bool accessed = new_snapshot_pte & PTE_A;
			bool need_shootdown = accessed;
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
					tlb_shootdown(addr, cpu_id, SNAPSHOT_CORE, call_arg, false); // send the ipi first
					// now do the copying
					newPage = dune_alloc_page_internal();
					memcpy(newPage, (void *)PGADDR(addr), PGSIZE);
					__atomic_store_n(snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					call_arg->ready_to_flush = true;
					asm volatile("mfence" ::: "memory");
					dune_flush_tlb_one(addr); // flush the tlb while waiting 
					while(!call_arg->flushed) {
						//asm volatile("pause");
					}
					need_local_flush = false;
					//state->tlb_shootdowns++;
				} else {
					newPage = dune_alloc_page_internal();
					//memcpy((void*)&ts_pages[cpu_id], (void *)PGADDR(addr), PGSIZE);
					memcpy((void*)newPage, (void *)PGADDR(addr), PGSIZE);
					//memset(newPage, 0, PGSIZE);
					//__atomic_store_n(snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					if (!pte_compare_exchange(snapshot_pte, new_snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm)) {
						//dune_printf("cmpexg failed\n");
						need_shootdown = true;
						snapshot_pte_perm = PTE_FLAGS(*snapshot_pte);
						snapshot_pte_perm |= PTE_W;
						snapshot_pte_perm &= ~PTE_COW;
						snapshot_pte_perm &= ~PTE_LOCK;
						__atomic_store_n(snapshot_pte, PTE_ADDR(newPage) | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					}
					//__atomic_store_n(snapshot_pte, snapshot_pte_addr | snapshot_pte_perm, __ATOMIC_SEQ_CST);
					if (snapshot_thread_started && need_shootdown) {
						volatile ipi_call_tlb_flush_arg_t* call_arg = (ipi_call_tlb_flush_arg_t*)dune_ipi_percpu_working_page(cpu_id);
						call_arg->ready_to_flush = true;
						call_arg->flushed = false;
						call_arg->addr = addr;
						tlb_shootdown(addr, cpu_id, SNAPSHOT_CORE, call_arg, true);
						//state->tlb_shootdowns++;
					}
				}
				if (dune_page_isfrompool(snapshot_pte_addr)) {
					dune_page_put(dune_pa2page(snapshot_pte_addr));
				}
			}
			//dune_printf("5 pgflt on main cpus %d pgroot %p va %p fec %lu RIP 0x%016llx RSP 0x%016llx\n", cpu_id, state->pgroot, addr, fec, tf->rip, tf->rsp);
			//dune_printf("main pgflt cpu %d on va %p fec %lu RIP 0x%016llx COW, new page %p, snapshot thread started %d, sent ipi\n", cpu_id, addr, fec, tf->rip, newPage, snapshot_thread_started);
			// writer still uses the old physical page 
			here:
			__atomic_store_n(pte, pte_addr | perm, __ATOMIC_SEQ_CST);
		}
	}
out:
	// Invalidate
	if (need_local_flush) {
		dune_flush_tlb_one(addr);
	}
	//state->page_fault_cycles += rdtscllp() - cycles_start;
	return;
}


void *t_snapshot(void *arg) {
	ThreadArg * targ = (ThreadArg*) arg;
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	printf("t_snapshot: started on core %d, pgroot %p\n", cpu_id, pgroot);
	asm volatile("mfence" ::: "memory");
	volatile int ret = dune_enter();
	if (ret) {
		printf("t_snapshot: failed to enter dune\n");
		return NULL;
	}
	assert(sched_getcpu() == dune_ipi_get_cpu_id());
	cow_state_t* state = get_shared_cow_state();
	asm volatile("mfence" ::: "memory");
	
	asm volatile("mfence" ::: "memory");
	dune_printf("Snapshot Thread running on core %d\n", cpu_id);

	assert(cpu_id == SNAPSHOT_CORE);

	assert(state->pgroot_snapshot);

	// set COW bits on pgroot_snapshot
	struct timeval start, end;
    gettimeofday(&start, NULL);
	dune_vm_page_walk(state->pgroot_snapshot, VA_START, VA_END, pgtable_set_cow, NULL);
	// load the new page table
	gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    dune_printf("Setting CoW bits: %.6f seconds\n", elapsed);
	state->set_thread_state(cpu_id, thread_state::SNAPSHOT);
	load_cr3((unsigned long)state->pgroot_snapshot);
	uint64_t s = 0;
	//int scan_count = 0;
	bool scaned = false;
	//sleep(20);
	gettimeofday(&start, NULL);
	while(true) {
		for (int i = 0; i < targ->size; i += 1) {
			s += targ->memory[i];
		}
		if (scaned == false) {
			gettimeofday(&end, NULL);
    		elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
			dune_printf("Snapshot: first scan finished, s %lu, took %.6f seconds\n", s, elapsed);
			scaned = true;
		}
	}
	//sleep(20);
	//dune_printf("tlb shootdowns %d, page_faults %d\n", state->tlb_shootdowns, state->page_faults);

	dune_printf("Snapshot: done sleeping\n");
	state->set_thread_state(cpu_id, thread_state::NONE);
	return NULL;
}

static inline int pteflags_to_perm(uint64_t pte_flags, int level)
{
	int perm = 0;

	if (pte_flags & PTE_P)
		perm |= PERM_R;
	if (pte_flags & PTE_W)
		perm |= PERM_W;
	if (!(pte_flags & PTE_NX))
		perm |= PERM_X;
	if (pte_flags & PTE_U)
		perm |= PERM_U;
	if (pte_flags & PTE_PCD)
		perm |= PERM_UC;
	if (pte_flags & PTE_COW)
		perm |= PERM_COW;
	if (pte_flags & PTE_USR1)
		perm |= PERM_USR1;
	if (pte_flags & PTE_USR2)
		perm |= PERM_USR2;
	if (pte_flags & PTE_USR3)
		perm |= PERM_USR3;
	if (pte_flags & PTE_STACK)
		perm |= PERM_USR4;
	if ((pte_flags & PTE_PS) && level == 1)
		perm |= PERM_BIG;
	if ((pte_flags & PTE_PS) && level == 2)
		perm |= PERM_BIG_1GB;

	return perm;
}



struct async_copy_args {
	ptent_t** root1;
	ptent_t** root2;
};

int pgtable_cow_copy(const void *arg, ptent_t *pte, void *va, int level) {
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
	// if ((debug_va_addr & ~(PGSIZE - 1)) == (uint64_t)va) {
	// 	printf("pgtable_cow_copy debug_va_addr %p *pte %p\n", va, *pte);
	// }
	if ((((uint64_t)can_va) >= PAGEBASE && ((uint64_t)can_va) < PAGEBASE_END)) {
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		ret = dune_vm_map_phys(newRoot1, can_va, pgsize(level), (void*)PTE_ADDR(*pte), pteflags_to_perm(PTE_FLAGS(*pte), level));

		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_map_phys failed with %d\n", ret);
			return ret;
		}
		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);

		ret = dune_vm_map_phys(newRoot2, can_va, pgsize(level), (void*)PTE_ADDR(*pte), pteflags_to_perm(PTE_FLAGS(*pte), level));

		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_map_phys failed with %d\n", ret);
			return ret;
		}

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

		*newPte1 = *pte & (~PTE_A); 

		if (dune_page_isfrompool(PTE_ADDR(*pte)))
			dune_page_get(pg);
		
		ret = dune_vm_lookup(newRoot2, can_va, CREATE_NORMAL, &newPte2);
		if (ret < 0){
			printf("pgtable_cow_copy dune_vm_lookup 1 failed with %d\n", ret);
			return ret;
		}

		*newPte2 = *pte & (~PTE_A);

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
		// if ((*pte & PTE_W) && (*pte & PTE_P) && !(*pte & PTE_STACK) && !(*pte & PTE_COW)) { // Ignore pages that are originally not writable or pages that are already marked with CoW bit. 
		// 	*newPte1 = (*pte & ~(PTE_W)) | PTE_COW; // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
		// 	*newPte2 = (*pte & ~(PTE_W)) | PTE_COW;
		// } else {
		// 	*newPte1 = *pte;
		// 	*newPte2 = *pte;
		// }

		if ((uint64_t)can_va >= cow_region_begin && (uint64_t)can_va < cow_region_end) {
			*newPte1 = ((*pte & ~(PTE_W)) | PTE_COW ) & (~PTE_A); // disable write and set the COW bit. The cow bit is used to determine if copy is needed in page fault.
			*newPte2 = ((*pte & ~(PTE_W)) | PTE_COW) & (~PTE_A);
		} else {
			*newPte1 = *pte & (~PTE_A);
			*newPte2 = *pte & (~PTE_A);
		}
		if (dune_page_isfrompool(PTE_ADDR(*pte))){
			dune_page_get(pg);
			dune_page_get(pg);
		}
	}
	return 0;
}

/**
 * Clone a page root.
 */
void dune_vm_my_clone(ptent_t *root, async_copy_args * args, page_walk_cb cb)
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
}

void* page_table_copy_worker(void*) {
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	volatile int ret = dune_enter();
	if (ret) {
		printf("t_snapshot: failed to enter dune\n");
		return NULL;
	}
	assert(sched_getcpu() == dune_ipi_get_cpu_id());
	volatile cow_state_t * state = get_shared_cow_state();
	((cow_state_t * )state)->set_thread_state(cpu_id, thread_state::PGTBL_COPY);
	state->async_copy_worker_ready = true;
	asm volatile("mfence" ::: "memory");
	printf("Async PGTbl Copy Thread running on core %d, state %p\n", cpu_id, state);

	//dune_procmap_dump();

	while (true) {
		while (!state->async_copy_job);
		state->async_copy_job = false;
		asm volatile("mfence" ::: "memory");
		//ptent_t *pgroot = state->pgroot;
		//struct snapshot_task task = snapshot_task_queue.dequeue();
		ptent_t *pgroot_copy_from = state->pgroot;
		assert(state->shadow_pgroot1 == NULL);
		assert(state->shadow_pgroot2 == NULL);
		printf("start cow_dup_pgtble %p\n", pgroot_copy_from);
		struct timeval start, end;
    	gettimeofday(&start, NULL);
		ptent_t *root1 = NULL;
		ptent_t *root2 = NULL;
		async_copy_args args;
		args.root1 = &root1;
		args.root2 = &root2;
		dune_vm_my_clone(state->pgroot, &args, pgtable_cow_copy);
		state->shadow_pgroot1 = root1;
		state->shadow_pgroot2 = root2;
		gettimeofday(&end, NULL);
    	double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
		printf("done cow_dup_pgtble, took %.6f seconds, new root %p %p, copied from %p\n", elapsed, state->shadow_pgroot1, state->shadow_pgroot2, pgroot_copy_from);
	}

	return NULL;
}

pthread_t async_pgtbl_copy_pthread;
pthread_attr_t async_pgtbl_copy_pthread_attr;
cpu_set_t async_pgtbl_copy_pthread_cpus;

pthread_attr_t snapshot_worker_attr;
pthread_t snapshot_worker_pthread;
cpu_set_t snapshot_worker_cpus;
void cow_state_init() {
	volatile cow_state_t* state = get_shared_cow_state();
	state->pgroot = pgroot;
	state->shadow_pgroot1 = NULL;
	state->shadow_pgroot2 = NULL;
	state->pgroot_snapshot = NULL;
	state->async_copy_job = false;
	state->snapshot_worker_started = false;
	state->async_copy_worker_ready = false;
	for (int i = 0 ;i < 128; ++i) {
		((cow_state_t*)state)->set_thread_state(i, thread_state::NONE);
	}
	state->tlb_shootdowns = 0;
	state->page_faults = 0;
	state->page_fault_cycles = 0;
}

void cow_init() {
	volatile cow_state_t* state = get_shared_cow_state();

	pthread_attr_init(&async_pgtbl_copy_pthread_attr);
	CPU_ZERO(&async_pgtbl_copy_pthread_cpus);
	CPU_SET(ASYNC_COPY_CORE, &async_pgtbl_copy_pthread_cpus);
	pthread_attr_setaffinity_np(&async_pgtbl_copy_pthread_attr, sizeof(cpu_set_t), &async_pgtbl_copy_pthread_cpus);
	pthread_create(&async_pgtbl_copy_pthread, &async_pgtbl_copy_pthread_attr, page_table_copy_worker, NULL);
	printf("waiting for async_copy_worker to be ready, state %p\n", state);
	asm volatile("mfence" ::: "memory");
	while(!state->async_copy_worker_ready) {
		asm volatile("pause");
	}
	state->async_copy_job = true;
	asm volatile("mfence" ::: "memory");
}

void branch_off(void* (*snapshot_worker)(void*), void * arg) {
	int cpu_id = dune_ipi_get_cpu_id();
	cow_state_t* state = get_shared_cow_state();
	printf("branch off on core %d state %p, state->shadow_pgroot %p %p\n", cpu_id, state, state->shadow_pgroot1, state->shadow_pgroot2);
	asm volatile("mfence" ::: "memory");
	while (state->shadow_pgroot1 == NULL) { // wait for the async-copy to complete.
		asm volatile("pause");
	}
	while (state->shadow_pgroot2 == NULL) { // wait for the async-copy to complete.
		asm volatile("pause");
	}
	printf("state->shadow_pgroot %p %p\n", state->shadow_pgroot1, state->shadow_pgroot2);
	asm volatile("mfence" ::: "memory");
	ptent_t* pgroot_snapshot = state->pgroot;
	
	state->pgroot_snapshot = pgroot_snapshot;
	state->pgroot = state->shadow_pgroot1;
	asm volatile("mfence" ::: "memory");

	int switched_count = 0;
	{
		bool sent_ipi[128];
		memset(sent_ipi, false, sizeof(sent_ipi));
		ipi_call_change_pgtbl_arg_t* call_args = (ipi_call_change_pgtbl_arg_t*)dune_ipi_percpu_working_page(cpu_id);
		for (int i = 0; i < 128; ++i) {
			if (i == cpu_id)
				continue;
			if (state->get_thread_state(i) == thread_state::MAIN || state->get_thread_state(i) == thread_state::PGTBL_COPY) {
				ipi_call_change_pgtbl_arg_t* call_arg = &(call_args[i]);
				call_arg->done = false;
				call_arg->new_pgroot = state->shadow_pgroot1;
				ipi_message_t msg;
				msg.func = change_pgtable_ipi_call;
				msg.arg = (void*)call_arg;
				msg.type = CALL_TYPE_NON_BATCHABLE;
				asm volatile("mfence" ::: "memory");
				bool res = dune_queue_ipi_call_fast(cpu_id, i, msg);
				dune_send_ipi(cpu_id, i);
				sent_ipi[i] = true;
			}
		}
		for (int i = 0; i < 128; ++i) {
			if (sent_ipi[i]) {
				volatile ipi_call_change_pgtbl_arg_t* call_arg = &call_args[i];
				while(!call_arg->done);
				asm volatile("mfence" ::: "memory");
				printf("pgtble switch on cpu %d done\n", i);
				++switched_count;
			}
		}
	}
	printf("pgtble switch done on %d workers\n", switched_count);
	// TODO: send ipi to other main CPUs to change their page table

	pgroot = pgroot_snapshot;
	asm volatile("mfence" ::: "memory");
	// TODO: Start the snapshot_worker on a differnt CPU to do its job

	pthread_attr_init(&snapshot_worker_attr);
	CPU_ZERO(&snapshot_worker_cpus);
	CPU_SET(SNAPSHOT_CORE, &snapshot_worker_cpus);
	pthread_attr_setaffinity_np(&snapshot_worker_attr, sizeof(cpu_set_t), &snapshot_worker_cpus);
	pthread_create(&snapshot_worker_pthread, &snapshot_worker_attr, snapshot_worker , arg);

	// while(get_shared_cow_state()->get_thread_state(SNAPSHOT_CORE) != thread_state::SNAPSHOT);
	
	load_cr3((unsigned long)state->shadow_pgroot1); // load the write-protected page table
	// start pre-copying the page table for the next branch-off
	state->shadow_pgroot1 = NULL; // clear shadow pgroot
	state->shadow_pgroot2 = NULL; // clear shadow pgroot
	asm volatile("mfence" ::: "memory");
	//state->async_copy_job = true;
}



constexpr size_t kHistogramBinCount = 500;

class LatencyHistogram {
private:
    std::vector<int> bins;
    double binWidth;
    double minLatency;

public:
    // Constructor: specify the number of bins, min latency, and the width of each bin
    LatencyHistogram(int numBins, double minLatency, double binWidth)
        : bins(numBins, 0), binWidth(binWidth), minLatency(minLatency) {
    }

    // Add a new latency measurement
    void addLatency(double latency) {
        int index = static_cast<int>((latency - minLatency) / binWidth);
        if (index < 0 || index >= bins.size()) {
            //throw std::out_of_range("Latency" + std::to_string(latency) + " is out of the histogram range.");
			return;
        }
        bins[index] += 1;
    }

    // Get the count of latencies in a specific bin
    int getBinCount(int binIndex) const {
        if (binIndex < 0 || binIndex >= bins.size()) {
            throw std::out_of_range("Bin index is out of range.");
        }
        return bins[binIndex];
    }

    // Calculate and return the percentile value
    double getPercentile(double percentile) {
        int totalLatencies = 0;
        for (const auto& bin : bins) {
            totalLatencies += bin;
        }

        if (totalLatencies == 0) {
            //throw std::runtime_error("No latencies recorded.");
			return 0;
        }

        int targetCount = static_cast<int>(percentile / 100.0 * totalLatencies);
        int runningCount = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            runningCount += bins[i];
            if (runningCount >= targetCount) {
                return minLatency + i * binWidth;
            }
        }

        return minLatency + bins.size() * binWidth; // Maximum latency
    }

    void Merge(const LatencyHistogram & other) {
        for (int i = 0; i < bins.size(); ++i) {
            bins[i] += other.bins[i];
        }
    }


    double getAverageLatency() const {
        double totalLatency = 0;
        int totalCount = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            int count = bins[i];
            totalLatency += (minLatency + i * binWidth) * count;
            totalCount += count;
        }
        return totalCount > 0 ? totalLatency / totalCount : 0;
    }

    double getMedianLatency() const {
        int totalLatencies = 0;
        for (const auto& bin : bins) {
            totalLatencies += bin;
        }

        if (totalLatencies == 0) {
            return 0; // No latencies recorded
        }

        int halfCount = totalLatencies / 2;
        int runningCount = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            runningCount += bins[i];
            if (runningCount >= halfCount) {
                return minLatency + i * binWidth;
            }
        }
        return 0; // Should not reach here
    }

    double getMaxLatency() const {
        for (int i = bins.size() - 1; i >= 0; --i) {
            if (bins[i] > 0) {
                return minLatency + (i + 1) * binWidth;
            }
        }
        return 0; // No latencies recorded
    }

    double getMinLatency() const {
        for (size_t i = 0; i < bins.size(); ++i) {
            if (bins[i] > 0) {
                return minLatency + i * binWidth;
            }
        }
        return 0; // No latencies recorded
    }
};

void *random_access_thread(void *arg) {
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	volatile int ret = dune_enter();
	if (ret) {
		printf("random_access_thread: failed to enter dune\n");
		return NULL;
	}
	assert(sched_getcpu() == dune_ipi_get_cpu_id());
	dune_printf("random_access_thread started on worker %d\n", cpu_id);
	volatile ThreadArg *thread_arg = (ThreadArg *)arg;
	char* memory = thread_arg->memory;
	size_t size = thread_arg->size;
	int cnt = thread_arg->count; 
	int num_threads = thread_arg->num_threads;
	int thread_id = thread_arg->thread_id;
	size_t num_pages = size / PGSIZE;
	size_t pages_per_worker = num_pages / num_threads;
	struct timeval start, end;
	gettimeofday(&start, NULL);
    dune_prefault_pages();
	memset(memory, 0, size);
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

	auto state = get_shared_cow_state();
	state->set_thread_state(cpu_id, thread_state::MAIN);
	writer_ready[cpu_id] = true;
	asm volatile("mfence" ::: "memory");
    
	printf("Time taken for dune_prefault_pages on thread %d: %.6f seconds\n", thread_id, elapsed);

	while(!(*thread_arg->ready_to_go));
    //sleep(1);
	LatencyHistogram *hist = new LatencyHistogram(kHistogramBinCount, 0, 1);
	//return hist;
	// if (thread_id == 1) {
	// 	dune_procmap_dump();
	// }
    gettimeofday(&start, NULL);
	//dune_printf("3 random_access_thread started on worker %d, t %lu\n", cpu_id, t);
    uint64_t s = 0;
	uint64_t accesses = 0;
	unsigned long ts = rdtscllp();
	int start_p = thread_id * (pages_per_worker);
	int end_p = (thread_id + 1) * (pages_per_worker);
	if (end_p > num_pages) {
		end_p = num_pages;
	}
	for (size_t i = start_p; i < end_p; i++) {
		// uint64_t x = RandomGenerator::getRandU64();
		// size_t index = (x % (end_p - start_p) + start_p) * PGSIZE;
		size_t index = i * PGSIZE;
		memory[index] += 1; // Simple write operation to trigger CoW
		s += memory[index];
		++accesses;
	}
    // for (size_t i = 0; cnt--; i ++) { // Accessing in page size increments
    //     uint64_t x = RandomGenerator::getRandU64();
    //     size_t index = x % size;
    //     //unsigned long ts = rdtscllp();
	// 	//cycles_start = rdtscllp();
    //     memory[index] += 1; // Simple write operation to trigger CoW
    //     s += memory[index];
    //     //unsigned long te = rdtscllp();
    //     //hist->addLatency(te - ts);
	//     ++accesses;
    // }
	// for (int i = 0; i < size; i += 4096) {
	// 	//cycles_start = rdtscll();
	// 	memory[i] += 1;
	// 	s += memory[i];
	// 	++accesses;
	// }
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Thread %d time taken for random memory access: %.6f seconds s %lu, cycles per fault %lu, num_pages %lu, pages per worker %lu\n", thread_id, elapsed, s, (rdtscllp() - ts)/(accesses), num_pages, pages_per_worker);
	//dune_printf("random_access_thread tlb shootdowns %d, page_faults %d\n", state->tlb_shootdowns, state->page_faults);
	dune_ipi_print_stats();
	state->set_thread_state(cpu_id, thread_state::NONE);
	sleep(30);
    return hist;
}

LatencyHistogram hist(kHistogramBinCount, 0, 1);

int main(int argc, char *argv[])
{
	volatile int ret;
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);

   	pthread_t main_thread = pthread_self();    
   	pthread_setaffinity_np(main_thread, sizeof(cpu_set_t), &cpuset);

	printf("fastcow: not running dune yet\n");


	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);

	ret = dune_init_with_ipi(1);
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}
	ret = dune_enter();
	if (ret) {
		printf("failed to enter dune\n");
		return ret;
	}
	dune_register_pgflt_handler(vm_pgflt_handler);
	dune_register_ipi_batch_call(snapshot_worker_tlb_flush_batch);
	assert(sched_getcpu() == dune_ipi_get_cpu_id());

	printf("fastcow: now printing from dune mode on core %d, pgroot %p\n", cpu_id, pgroot);

	if (argc != 5) {
        fprintf(stderr, "Usage: %s <memory_size_in_MB> <ratio_of_memory_to_access> <threads> <fork_or_not> \n", argv[0]);
        return 1;
    }
    int num_threads = 0;
    size_t memory_size = atoi(argv[1]) * MB;
    float perc = atof(argv[2]);
    size_t count = memory_size / 4096 * perc;
    num_threads = atoi(argv[3]);
    bool do_fork = atoi(argv[4]);
	//volatile char *memory = (char*)malloc(memory_size);
	volatile char *memory = (volatile char *)mmap(NULL, memory_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	madvise((void*)memory, memory_size, MADV_HUGEPAGE);
	debug_va_addr = ((uint64_t)memory);
    printf("memory size %lu, perc %f, count %lu, debug_va_addr %p\n", memory_size, perc, count, debug_va_addr);
	
	volatile ThreadArg thread_arg = {(char*)memory, memory_size, count, false};
	if (!memory) {
        perror("Failed to allocate memory");
        return 1;
    }
	unsigned long rsp;

	asm("mov %%rsp, %0" : "=r"(rsp));
	printf("main thread rsp %p\n", rsp);
	rdtsc_overhead = measure_tsc_overhead();
	synch_tsc();

	struct timeval start, end;
    gettimeofday(&start, NULL);
    memset((char*)memory, 0, memory_size);
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    
	cow_region_begin = (uint64_t)memory;
	cow_region_end = ((uint64_t)memory) + memory_size;
	printf("Time taken for memset(fault): %.6f seconds, cow region[%p, %p]\n", elapsed, cow_region_begin, cow_region_end);


	// gettimeofday(&start, NULL);
    // dune_prefault_pages();
    // gettimeofday(&end, NULL);
    // elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
	// printf("Time taken for dune_prefault_pages: %.6f seconds\n", elapsed);


	cow_state_init();

    pthread_t threads[num_threads];
	pthread_attr_t attrs[num_threads];
	cpu_set_t cpus[num_threads];
	ThreadArg targs[num_threads + 1];
	volatile bool ready_to_go = false;
    for (int i = 0; i < num_threads; ++i) { // Random access after fork
		pthread_attr_init(&attrs[i]);
		CPU_ZERO(&cpus[i]);
		CPU_SET(i + 1, &cpus[i]);
		pthread_attr_setaffinity_np(&attrs[i], sizeof(cpu_set_t), &cpus[i]);
		targs[i].memory = (char*)memory;
		targs[i].ready_to_go = &ready_to_go;
		targs[i].thread_id = i;
		targs[i].size = memory_size;
		targs[i].count = count;
		targs[i].num_threads = num_threads;
        pthread_create(&threads[i], &attrs[i], random_access_thread, (void*)&targs[i]);
    }
	
	for (int i = 1; i <= num_threads; ++i) {
		while(!writer_ready[i]);
	}
	printf("All ready\n");
	cow_init();
	volatile auto state = get_shared_cow_state();
	state->set_thread_state(cpu_id, thread_state::MAIN);
	printf("cow inited\n");
	//dune_procmap_dump();
	asm volatile("mfence" ::: "memory");
	
	gettimeofday(&start, NULL);
	targs[num_threads].memory = (char*)memory;
	targs[num_threads].ready_to_go = &ready_to_go;
	targs[num_threads].thread_id = num_threads;
	targs[num_threads].size = memory_size;
	targs[num_threads].count = count;
	targs[num_threads].num_threads = num_threads;
    branch_off(t_snapshot, (void*)&targs[num_threads]);
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for fast copy-on-write: %.6f seconds\n", elapsed);
	while(state->get_thread_state(SNAPSHOT_CORE) != thread_state::SNAPSHOT);
	assert(state->get_thread_state(SNAPSHOT_CORE) == thread_state::SNAPSHOT);
	//sleep(2);
	ready_to_go = true;
	asm volatile("mfence" ::: "memory");
	void *ret_val;
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], &ret_val);
        LatencyHistogram * lHist = (LatencyHistogram *)ret_val;
        hist.Merge(*lHist);
        delete lHist;
    }
    double minLatency = hist.getMinLatency();
    double avgLatency = hist.getAverageLatency();
    double maxLatency = hist.getMaxLatency();
    double p50Latency = hist.getPercentile(50);
    double p75Latency = hist.getPercentile(75);
    double p90Latency = hist.getPercentile(90);
    double p95Latency = hist.getPercentile(95);
    double p99Latency = hist.getPercentile(99);
    double p999Latency = hist.getPercentile(99.9);
    printf("min %.2f  avg %.2f max %.2f p50 %.2f p75 %.2f p90 %.2f p95 %.2f p99 %.2f p99.9 %.2f\n", minLatency, avgLatency, maxLatency, p50Latency, p75Latency, p90Latency, p95Latency, p99Latency, p999Latency);
	printf("main tlb shootdowns %d, page_faults %d, cycles/fault %lu\n", state->tlb_shootdowns, state->page_faults, state->page_fault_cycles / state->page_faults);
	sleep(50);
	//exit(0);
	return 0;
}
