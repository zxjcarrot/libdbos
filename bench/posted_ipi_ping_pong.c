#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>

#include "bench.h"

#include "libdune/dune.h"
#include "libdune/cpu-x86.h"

#define THREAD_2_CORE 10
#define TEST_VECTOR 0xf2

#define NUM_ITERATIONS 1000000

volatile int THREAD_1_CORE = 0;
volatile bool t2_ready = false;
volatile bool wait1 = true;
volatile bool wait2 = true;
volatile bool done = false;

static void test_handler1(struct dune_tf *tf) {
	dune_apic_eoi();
	wait1 = false;
}

static void test_handler2(struct dune_tf *tf) {
	dune_apic_eoi();
	wait2 = false;
}

void *t2_start(void *arg) {
	volatile int ret = dune_enter();
	if (ret) {
		printf("posted_ipi: failed to enter dune in thread 2\n");
		return NULL;
	}
        
	dune_register_intr_handler(TEST_VECTOR, test_handler2);
	asm volatile("mfence" ::: "memory");
	t2_ready = true;
	while (true) {
		while (wait2 && !done);
		if (done) break;
		wait2 = true;
		dune_apic_send_posted_ipi(TEST_VECTOR, THREAD_1_CORE);
	};
	return NULL;
}

int main(int argc, char *argv[])
{
	volatile int ret;

	printf("posted_ipi: not running dune yet\n");

	ret = dune_init_and_enter();
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}
	printf("posted_ipi: now printing from dune mode\n");

	THREAD_1_CORE = sched_getcpu();

	pthread_t t2;
        pthread_attr_t attr;
        cpu_set_t cpus;
        pthread_attr_init(&attr);
        CPU_ZERO(&cpus);
        CPU_SET(THREAD_2_CORE, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
        pthread_create(&t2, &attr, t2_start, NULL);

	while (!t2_ready);
	asm volatile("mfence" ::: "memory");
	printf("posted_ipi: about to send posted IPI\n");

	dune_register_intr_handler(TEST_VECTOR, test_handler1);
	
	unsigned long rdtsc_overhead = measure_tsc_overhead();
	synch_tsc();
	unsigned long start_tick = rdtscll();

	int i;
	for (i = 0; i < NUM_ITERATIONS; i++) {
		dune_apic_send_posted_ipi(TEST_VECTOR, THREAD_2_CORE);
		while (wait1);
		wait1 = true;
	}

	unsigned long end_tick = rdtscllp();
	unsigned long latency = (end_tick - start_tick - rdtsc_overhead) / NUM_ITERATIONS;
	printf("Latency: %ld cycles per ping pong.\n", latency);

	done = true;
	pthread_join(t2, NULL);

	return 0;
}
