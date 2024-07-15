#pragma once
#include "dune.h"
#include "ipi_call.h"

typedef enum thread_state {
	DUNE_THREAD_NONE = 0,
	DUNE_THREAD_MAIN = 1,
	DUNE_THREAD_SNAPSHOT = 2,
	DUNE_THREAD_PGTBL_COPY = 3
}thread_state;


struct dbos_snapshot;
typedef SLIST_ENTRY(dbos_snapshot) dbos_snapshot_entry_t;

struct dbos_snapshot {
	ptent_t * snapshot_table;
	uint64_t epoch;
	int active;
};

SLIST_HEAD(dbos_snapshot_head, dbos_snapshot);

#define DUNE_FAST_COW_STAGING_BUFFER_SIZE (5 * 1024 * 1024)
typedef struct fast_spawn_state_t {
	#define MAX_SNAPSHOTS 32
	struct dbos_snapshot snapshots[MAX_SNAPSHOTS];
	uint64_t num_snapshots;
	uint64_t E_global;

	ptent_t *pgroot;
	ptent_t *shadow_pgroot1; // one for main CPUs
	ptent_t *shadow_pgroot2; // one for snapshot
	ptent_t *pgroot_snapshot;
	bool async_copy_job;
	bool snapshot_worker_ready;
	bool async_copy_worker_ready;
	bool tearing_down;
	int snapshot_runs_started_version;
	int snapshot_runs_finished_version;
	thread_state thread_states[128];
	int dbos_tlb_shootdowns;
	int page_faults;
	uint64_t page_fault_cycles;
	int thread_pgfaults[128];
	pid_t snapshot_thread_tid;
	void * cow_staging_buffer[128];
	uint64_t ipi_calls[128];
	uint64_t shootdowns[128];
	uint64_t interrupt_ts[128];
	uint64_t cycles_spent_for_shootdown[128];
	uint64_t ipi_call_queue_full[128];
	uint64_t cycles_spent_in_flt[128];
	uint64_t write_calls[128];
	uint64_t syscalls[128];
	uint64_t cycles_per_write_syscall[128];
	uint64_t ts_write_call_start[128];
	uint64_t cycles_between_consecutive_writes[128];
	uint64_t cycles_access_start[128];
	uint64_t cycles_spent_all_access[128];
	uint64_t cycles_spent_for_interrupt[128];
    pthread_mutex_t mutex;
	pthread_cond_t cond;
	// thread_state get_thread_state(int cpu_id){return thread_states[cpu_id]; }
	// void set_thread_state(int cpu_id, thread_state state){ thread_states[cpu_id] = state; }
	void * (*snapshot_user_work_func)(void*);
	void *snapshot_user_work_func_arg;
}fast_spawn_state_t;

extern int SNAPSHOT_CORE;
extern int ASYNC_COPY_CORE;
extern bool dbos_fast_spawn_enabled;
extern pid_t dune_fast_spawn_snapshot_thread_id();
extern fast_spawn_state_t* dune_get_fast_spawn_state();
extern fast_spawn_state_t* dune_get_fast_spawn_state_host();
extern void dune_set_thread_state(fast_spawn_state_t * state, int cpu_id, thread_state tstate);
extern thread_state dune_get_thread_state(fast_spawn_state_t * state, int cpu_id);
extern void dune_fast_spawn_on_snapshot(void* (*snapshot_worker)(void*), void * arg);
extern bool dune_fast_spawn_snapshot_running();
extern void dune_fast_spawn_configure();
extern int dune_fast_spawn_init(int selective_TLB_shootdown);
extern void dune_fast_spawn_signal_copy_worker();
extern int pgtable_set_cow(const void *arg, ptent_t *pte, void *va, int level);