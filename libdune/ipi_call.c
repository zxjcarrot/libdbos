#define _GNU_SOURCE
#include "ipi_call.h"
#include "cpu.h"

#include <errno.h>
#include <sys/mman.h>
static ipi_percpu_ring_t * cpu_rings = NULL;
static const size_t cpu_rings_size = sizeof(ipi_percpu_ring_t) * IPI_MAX_CPUS;
static ipi_percpu_ring_t * cpu_rings_region_pmem = NULL;
static char * ipi_percpu_working_pmem = NULL;
static char * ipi_percpu_working_vmem = NULL;
static char * ipi_shared_state_page_vmem = NULL;
static char * ipi_shared_state_page_pmem = NULL;
static int n_msgs_collected;
static int n_interrupts;
static ipi_call_batch_func_t batch_ipi_call;

static void dune_ipi_interrupt_handler(struct dune_tf * tf) {
    asm volatile ("cli":::);
    dune_apic_eoi();
    //printf("dune_ipi_interrupt_handler\n");
    int cpu_id = dune_get_cpu_id();
    volatile ipi_percpu_ring_t * rings = &cpu_rings[cpu_id];
    ipi_message_t buf[IPI_MESSAGE_RINGBUF_SIZE * IPI_MAX_CPUS];
    ipi_message_t buf_batchable[IPI_MESSAGE_RINGBUF_SIZE * IPI_MAX_CPUS];
    ipi_message_t buf_nonbatchable[IPI_MESSAGE_RINGBUF_SIZE * IPI_MAX_CPUS];
    
    rings->phase = PHASE1;
    asm volatile("mfence" ::: "memory");
    //assert(cpu_id != -1);
    
    uint64_t i;
    uint64_t n_msgs_batchable = 0;
    uint64_t n_msgs_nonbatchable = 0;
    uint64_t n_msgs = 0;

   
    // read all messages and handle them at once
    for (i = 0; i < IPI_MAX_CPUS; ++i) {
        n_msgs += dune_collect_ipi_messages(cpu_id, i, buf + n_msgs);
    }
    for (i = 0; i < n_msgs; ++i) {
        if (buf[i].type == CALL_TYPE_BATCHABLE) {
            buf_batchable[n_msgs_batchable++] = buf[i];
        } else {
            buf_nonbatchable[n_msgs_nonbatchable++] = buf[i];
        }
    }
    for (i = 0; i < n_msgs_nonbatchable; ++i) {
        buf_nonbatchable[i].func(buf_nonbatchable[i].arg);
    }
    if (n_msgs_batchable > 0) {
        batch_ipi_call(buf_batchable, n_msgs_batchable);
    }
    rings->phase = PHASE2;
    //dune_printf("dune_ipi_interrupt_handler %d msgs\n", n_msgs);
    //n_msgs_collected += n_msgs;
    asm volatile("mfence" ::: "memory");
    n_msgs = 0;
    n_msgs_batchable = 0;
    n_msgs_nonbatchable = 0;
    for (i = 0; i < IPI_MAX_CPUS; ++i) {
        n_msgs += dune_collect_ipi_messages(cpu_id, i, buf + n_msgs);
    }
    for (i = 0; i < n_msgs; ++i) {
        if (buf[i].type == CALL_TYPE_BATCHABLE) {
            buf_batchable[n_msgs_batchable++] = buf[i];
        } else {
            buf_nonbatchable[n_msgs_nonbatchable++] = buf[i];
        }
    }
    for (i = 0; i < n_msgs_nonbatchable; ++i) {
        buf_nonbatchable[i].func(buf_nonbatchable[i].arg);
    }
    if (n_msgs_batchable > 0) {
        batch_ipi_call(buf_batchable, n_msgs_batchable);
    }
    
    rings->phase = NONE;
    //dune_printf("dune_ipi_interrupt_handler %d msgs\n", n_msgs);
    asm volatile("mfence" ::: "memory");
    //dune_apic_eoi(); // ack the interrupt
    asm volatile ("sti":::);
    //n_msgs_collected += n_msgs;
    //n_interrupts++;
    
}

void dune_ipi_print_stats() {
    dune_printf("%d msgs, %d ints\n", n_msgs_collected, n_interrupts);
}

void dune_ipi_set_cpu_id(int cpuid) {
    dune_set_cpu_id(cpuid);
}

int dune_ipi_get_cpu_id() {
    return dune_get_cpu_id();
}

int dune_ipi_init() {
    int ret;
    // Allocate a contiguous private anonymous virtual memory region and 
    // use it as physical memory for ipi-related structures
    cpu_rings_region_pmem = mmap(NULL, cpu_rings_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
    if (cpu_rings_region_pmem == MAP_FAILED) {
        return -ENOMEM;
    }
    memset(cpu_rings_region_pmem, 0, cpu_rings_size);
    cpu_rings = (ipi_percpu_ring_t*)IPI_ADDR_RING_BASE;
    ret = dune_vm_map_phys(pgroot, cpu_rings, cpu_rings_size, (void*) dune_va_to_pa(cpu_rings_region_pmem), PERM_R|PERM_W|PERM_U | PERM_NOCOW);
    if (ret != 0) {
        return ret;
    }

    ipi_percpu_working_pmem = mmap(NULL, IPI_MAX_CPUS * PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
    memset(ipi_percpu_working_pmem, 0, IPI_MAX_CPUS * PAGE_SIZE);
    if (ipi_percpu_working_pmem == MAP_FAILED) {
        return -ENOMEM;
    }
    ipi_percpu_working_vmem = (char*)IPI_ADDR_WORKING_MEM_BASE;
    ret = dune_vm_map_phys(pgroot, ipi_percpu_working_vmem, IPI_MAX_CPUS * PAGE_SIZE, (void*) dune_va_to_pa(ipi_percpu_working_pmem), PERM_R|PERM_W|PERM_U | PERM_NOCOW);
    if (ret != 0) {
        return ret;
    }

    ipi_shared_state_page_pmem = mmap(NULL, PAGE_SIZE * 10, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
    memset(ipi_shared_state_page_pmem, 0, PAGE_SIZE * 10);
    if (ipi_shared_state_page_pmem == MAP_FAILED) {
        return -ENOMEM;
    }
    ipi_shared_state_page_vmem = (char*)IPI_ADDR_SHARED_STATE_BASE;
    ret = dune_vm_map_phys(pgroot, ipi_shared_state_page_vmem, PAGE_SIZE * 10, (void*) dune_va_to_pa(ipi_shared_state_page_pmem), PERM_R | PERM_W | PERM_U | PERM_NOCOW);
    if (ret != 0) {
        return ret;
    }

    return dune_register_intr_handler(IPI_VECTOR, dune_ipi_interrupt_handler);;
}

char* dune_ipi_percpu_working_page(int core_id) {
    return ipi_percpu_working_vmem + core_id * PAGE_SIZE;
}

char* dune_ipi_shared_state_page() {
    return ipi_shared_state_page_vmem;
}

char* dune_ipi_shared_state_page_nonvirt() {
    return ipi_shared_state_page_pmem;
}

bool dune_queue_ipi_call_empty(int src_core, int target_core) {
    volatile ipi_ring_t * ring = &cpu_rings[target_core].rings[src_core];
    uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    assert(head <= tail)
    return head >= tail;
}

uint64_t dune_queue_ipi_call_head(int src_core, int target_core) {
    volatile ipi_ring_t * ring = &cpu_rings[target_core].rings[src_core];
    uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    return head;
}

uint64_t dune_queue_ipi_call_tail(int src_core, int target_core) {
    volatile ipi_ring_t * ring = &cpu_rings[target_core].rings[src_core];
    uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    return tail;
}

uint64_t dune_queue_ipi_call_fast(int src_core, int target_core, ipi_message_t msg) {
    volatile ipi_ring_t * ring = &cpu_rings[target_core].rings[src_core];
    uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    assert(head <= tail);
    if (tail - head >= IPI_MESSAGE_RINGBUF_SIZE) 
        return IPI_INVALID_NUMBER;
    ring->buf[tail % IPI_MESSAGE_RINGBUF_SIZE] = msg;
    __atomic_add_fetch(&ring->tail, 1, __ATOMIC_RELEASE);
    return tail;
}

int dune_queued_ipi_phase(int target_core) {
    volatile ipi_percpu_ring_t * rings_struct = &cpu_rings[target_core];
    return (rings_struct->phase);
}

bool dune_ipi_call_number_collected(int src_core, int target_core, uint64_t call_num) {
    volatile ipi_ring_t * ring = &cpu_rings[target_core].rings[src_core];
    return __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE) > call_num;
}

bool dune_queued_ipi_need_interrupt(int target_core) {
    return (dune_queued_ipi_phase(target_core) != PHASE1);
}

void dune_register_ipi_batch_call(ipi_call_batch_func_t func) {
    batch_ipi_call = func;
}

void dune_queue_ipi_call(int src_core, int target_core, ipi_message_t msg) {
    bool succeeded = false;
    do {
        succeeded = dune_queue_ipi_call_fast(src_core, target_core, msg);
    } while (succeeded == false);
}

void dune_send_ipi(int src_core, int target_core) {
    dune_apic_send_posted_ipi(IPI_VECTOR, target_core);
}

uint64_t dune_collect_ipi_messages(int target_core, int src_core, ipi_message_t* msgs) {
    ipi_ring_t * ring = &cpu_rings[target_core].rings[src_core];
    //int cpu_id = dune_get_cpu_id();
    uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return 0;
    }
    assert(tail > head);
    uint64_t n_msgs = tail - head;
    for (uint64_t i = 0; i < n_msgs; ++i) {
        msgs[i] = ring->buf[(head + i) % IPI_MESSAGE_RINGBUF_SIZE];
    }
    __atomic_add_fetch(&ring->head, n_msgs, __ATOMIC_RELEASE);
    return n_msgs;
}

struct dune_pv_info * dune_pv_info_for_core(uint32_t core) {
	return (void *)(PV_INFO_PAGES_BASE + (core * PGSIZE) + DUNE_PV_TLB_OFFSET_INTO_PI_PAGE);
}
