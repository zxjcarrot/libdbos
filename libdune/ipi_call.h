#pragma once
#include <malloc.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>

#include "dune.h"

#define IPI_INVALID_NUMBER (0xffffffffffffffffULL)
#define IPI_VECTOR 0xf2
#define IPI_MAX_CPUS 128
#define IPI_MESSAGE_RINGBUF_SIZE 2

typedef void (*ipi_call_func_t)(void*);

typedef enum IPI_CALL_MESSAGE_TYPE {
    CALL_TYPE_NON_BATCHABLE=0,
    CALL_TYPE_BATCHABLE
}IPI_CALL_MESSAGE_TYPE_T;
typedef struct ipi_message_t {
    ipi_call_func_t func;
    IPI_CALL_MESSAGE_TYPE_T type;
    void* arg;
} ipi_message_t;

typedef void (*ipi_call_batch_func_t)(ipi_message_t*, uint64_t);

typedef struct ipi_ring_t {
    uint64_t head;
    char padding1[64 - sizeof(uint64_t)];
    uint64_t tail;
    char padding2[64 - sizeof(uint64_t)];
    ipi_message_t buf[IPI_MESSAGE_RINGBUF_SIZE];
} ipi_ring_t;

typedef enum IPI_FUNC_PHASE {
    NONE = 0,
    PHASE1,
    PHASE2
}IPI_FUNC_PHASE_T;

typedef struct ipi_percpu_ring_t {
    ipi_ring_t rings[IPI_MAX_CPUS]; // stores messages posted by each CPUs, indexed by cpu id
    IPI_FUNC_PHASE_T phase;
} ipi_percpu_ring_t;

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

#define IPI_ADDR_RING_BASE 0xffff800000000000 // virtual address to ipi data structures shared by all dune threads
#define IPI_ADDR_RING_END ROUND_UP((IPI_ADDR_RING_BASE + sizeof(ipi_percpu_ring_t) * IPI_MAX_CPUS), PGSIZE)
#define IPI_ADDR_WORKING_MEM_BASE IPI_ADDR_RING_END
#define IPI_ADDR_WORKING_MEM_END (IPI_ADDR_WORKING_MEM_BASE + IPI_MAX_CPUS * PAGE_SIZE)
#define IPI_ADDR_SHARED_STATE_BASE (IPI_ADDR_WORKING_MEM_END) // states shared by all vCPUs
#define IPI_ADDR_SHARED_STATE_END (IPI_ADDR_WORKING_MEM_END + PGSIZE)

extern int dune_queued_ipi_phase(int target_core);
extern bool dune_ipi_call_number_collected(int source_core, int target_core, uint64_t call_num);
extern bool dune_queued_ipi_need_interrupt(int target_core) ;
extern void dune_queue_ipi_call(int src_core, int target_core, ipi_message_t msg);
extern bool dune_queue_ipi_call_empty(int src_core, int target_core);
extern uint64_t dune_queue_ipi_call_head(int src_core, int target_core);
extern uint64_t dune_queue_ipi_call_tail(int src_core, int target_core);
extern uint64_t dune_queue_ipi_call_fast(int src_core, int target_core, ipi_message_t msg);
extern void dune_send_ipi(int src_core, int target_core);
extern int dune_ipi_init();
extern uint64_t dune_collect_ipi_messages(int target_core, int src_core, ipi_message_t* msgs);
extern char* dune_ipi_shared_state_page();
extern char* dune_ipi_percpu_working_page(int core_id);
extern void dune_ipi_set_cpu_id(int cpuid);
extern int dune_ipi_get_cpu_id();
extern void dune_ipi_print_stats();
extern void dune_register_ipi_batch_call(ipi_call_batch_func_t func);