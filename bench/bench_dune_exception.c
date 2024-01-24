#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <memory.h>

#include "libdune/dune.h"
#include "bench.h"

#define MAP_ADDR 0x400000000000

static char *mem;
unsigned long trap_tsc, overhead;
unsigned long time = 0;
unsigned long s = 0;
unsigned long nthreads;
unsigned long nrpgs;
static void prime_memory(void)
{
	int i;

	for (i = 0; i < nrpgs * 2; i++) {
		mem[i * PGSIZE] = i;
	}
}

static void make_writable_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	ptent_t *pte;
	int accessed;

	dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	*pte |= PTE_P | PTE_W | PTE_U | PTE_D | PTE_A;
	//*pte |= PTE_P | PTE_W | PTE_U | PTE_A | PTE_D;
	dune_flush_tlb_one(addr);
}

static void benchmark3(void)
{
	int i;
	
	for (i = 0; i < NRPGS; i++) {
		mem[i * PGSIZE] = i;
	}
}

struct ThreadArg {
	char * mem;
	int thread_id;
};

static void* worker_thread(void*arg) {
	volatile int ret = dune_enter();
	if (ret) {
		printf("worker_thread: failed to enter dune\n");
		return NULL;
	}
	int thread_id = (int)(arg);
	int pages_per_thread = nrpgs / nthreads;
	int begin_page_id = thread_id * pages_per_thread;
	int i;
	if (begin_page_id + pages_per_thread > nrpgs) {
		begin_page_id = nrpgs - pages_per_thread;
	}
	dune_register_pgflt_handler(make_writable_handler);
	unsigned long start = dune_get_ticks();
	unsigned long ts = 0;
	for (i = 0; i < pages_per_thread; i++) {
		mem[(begin_page_id + i) * PGSIZE] = 1;
		ts += mem[(begin_page_id + i) * PGSIZE];
	}
	unsigned long end = dune_get_ticks();

	dune_printf("Clear-wp fault took %ld cycles for worker %d for %ld pages\n", (end - start) / (pages_per_thread), thread_id, pages_per_thread);
	s += ts;
	return NULL;
}

static void benchmark_cow_clear_wp()
{
	int i;
	unsigned long tsc, avg_user_fault = 0;

	dune_register_pgflt_handler(make_writable_handler);
	dune_vm_mprotect(pgroot, (void *)mem, PGSIZE * nrpgs, PERM_R);

	pthread_t threads[nthreads];
	for (i = 0; i < nthreads; ++i) {
		pthread_create(&threads[i], NULL, worker_thread, (void*)i);
	}

	for (i = 0; i < nthreads; ++i) {
		pthread_join(threads[i], NULL);
	}
}


int main(int argc, char *argv[])
{
	int ret;
	if (argc >= 2) {
		nthreads = atoi(argv[1]);
	} else {
		nthreads = 1;
	}

	overhead = measure_tsc_overhead();
	printf("TSC overhead is %ld\n", overhead);

	ret = dune_init_and_enter();
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}

	dune_printf("Benchmarking dune performance...\n");

	
	// ret = dune_vm_map_pages(pgroot, (void *)MAP_ADDR, 2 * NRPGS * PGSIZE,
	// 						PERM_R | PERM_W);
	unsigned long long memory_size = 1024 * 1024UL * 1024 * 4;
	nrpgs = memory_size / PGSIZE / 2;
	mem = memalign(PGSIZE, memory_size);
	if (ret) {
		printf("failed to setup memory mapping\n");
		return ret;
	}

	//mem = (void *)MAP_ADDR;
	prime_memory();

	benchmark_cow_clear_wp();
	printf("s %lu\n", s);
	return 0;
}
