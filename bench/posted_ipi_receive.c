#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>

#include "bench.h"

#include "libdune/dune.h"
#include "libdune/cpu-x86.h"

#define THREAD_CORE 2
#define THREAD_2_CORE 3
#define TEST_VECTOR 0xf2

#define NUM_ITERATIONS 1000000

volatile bool t2_ready = false;
volatile bool wait = true;
volatile bool done = false;
volatile unsigned long start = 0;
volatile unsigned long total = 0;
volatile unsigned long rdtsc_overhead = 0;

static void test_handler(struct dune_tf *tf) {
	unsigned long end = rdtscllp();
	total += end - start - rdtsc_overhead;
	dune_apic_eoi();
	wait = false;
}

void *t2_start(void *arg) {
	volatile int ret = dune_enter();
	if (ret) {
		printf("posted_ipi: failed to enter dune in thread 2\n");
		return NULL;
	}
	dune_register_intr_handler(TEST_VECTOR, test_handler);
	asm volatile("mfence" ::: "memory");
	rdtsc_overhead = measure_tsc_overhead();
	synch_tsc();
        start = rdtscll();
	t2_ready = true;
	while (!done) {
                start = rdtscll();
        }
	return NULL;
}

int main(int argc, char *argv[])
{
	volatile int ret;

	printf("posted_ipi: not running dune yet\n");

        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(THREAD_CORE, &cpus);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpus) != 0 ||
            sched_getcpu() != THREAD_CORE) {
                printf("Could not pin thread to core %d.\n", THREAD_CORE);
                return 1;
        } else {
                printf("Thread pinned to core %d.\n", THREAD_CORE);
        } 

	ret = dune_init_and_enter();
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}
	printf("posted_ipi: now printing from dune mode\n");

	pthread_t t2;
        pthread_attr_t attr;
        cpu_set_t cpus2;
        pthread_attr_init(&attr);
        CPU_ZERO(&cpus2);
        CPU_SET(THREAD_2_CORE, &cpus2);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus2);
        pthread_create(&t2, &attr, t2_start, NULL);

	while (!t2_ready);
	asm volatile("mfence" ::: "memory");
	printf("posted_ipi: about to send posted IPI on core %d\n", sched_getcpu());

	int i;
	for (i = 0; i < NUM_ITERATIONS; i++) {
		dune_apic_send_posted_ipi(TEST_VECTOR, THREAD_2_CORE);
		while (wait);
		wait = true;
	}

	unsigned long latency = total / NUM_ITERATIONS;
	printf("Latency: %ld cycles.\n", latency);

	done = true;
	pthread_join(t2, NULL);

	return 0;
}
