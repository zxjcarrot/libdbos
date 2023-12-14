#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>

#include "bench.h"

#include "libdune/dune.h"

#define NUM_THREADS 2
#define TARGET_CORE 31
#define TEST_VECTOR 0xF2
#define NMSG_PER_WORKER 100000

unsigned long rdtsc_overhead;
volatile bool t2_ready = false;
volatile bool all_ready = false;
volatile bool done[NUM_THREADS];
volatile bool all_done = false;

typedef struct ipi_call_arg_t {
	int id;
	int ready;
} ipi_call_arg_t;

static void ipi_call1(void * arg) {
	ipi_call_arg_t* call_arg = (ipi_call_arg_t*)arg;
	call_arg->ready++;
	//asm volatile("mfence" ::: "memory");
	//printf("ready %d ipi from %d\n", call_arg->ready, call_arg->id);
}

void * ipi_handler_thread(void*arg) {
	volatile int ret = dune_enter();
	if (ret) {
		printf("posted_ipi: failed to enter dune in thread 2\n");
		return NULL;
	}
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	printf("IPI handling Thread running on core %d\n", sched_getcpu());
	
	asm volatile("mfence" ::: "memory");
	*(volatile bool *)arg = true;

	while (!all_done);
	return NULL;
}

void *t_start(void *arg) {
	volatile int ret = dune_enter();
	if (ret) {
		printf("posted_ipi: failed to enter dune in thread 2\n");
		return NULL;
	}
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	printf("IPI-Sending Thread running on core %d\n", sched_getcpu());
	asm volatile("mfence" ::: "memory");
	*(volatile bool *)arg = true;
	//while (!received_posted_ipi[sched_getcpu()]);
	while(!all_ready);

	unsigned long start_tick = rdtscll();
	// set up a ipi call for the next core and 
	// wait for it be called in the next call which marks the ready flag in arg
	char * page = dune_ipi_percpu_working_page(cpu_id);
	//printf("page %p\n", page);
	ipi_call_arg_t* call_arg = (ipi_call_arg_t*)page;
	call_arg->id = cpu_id;
	call_arg->ready = 0;
	unsigned long ipi_latency_sum = 0;
	unsigned long queued = 0;
	while (queued < NMSG_PER_WORKER) {
		bool buffered_msgs = false;
		ipi_message_t msg;
		msg.func = ipi_call1;
		msg.arg = call_arg;
		msg.type = CALL_TYPE_NON_BATCHABLE;
		if (dune_queue_ipi_call_fast(cpu_id, TARGET_CORE, msg)) { // Buffer as many messages as possible
			buffered_msgs = true;
			queued++;
			while(dune_queue_ipi_call_fast(cpu_id, TARGET_CORE, msg) == true) {
				queued++;
			}
		}
		if (buffered_msgs) {
			unsigned long ipi_start_tick = rdtscllp();
			dune_send_ipi(cpu_id, TARGET_CORE);
			unsigned long ipi_end_tick = rdtscllp();
			ipi_latency_sum += (ipi_end_tick - ipi_start_tick - rdtsc_overhead);
		}
	}

	asm volatile("mfence" ::: "memory");
	done[cpu_id] = true;
	asm volatile("mfence" ::: "memory");
	
	unsigned long end_tick = rdtscllp();
	unsigned long latency = (end_tick - start_tick - rdtsc_overhead) / NMSG_PER_WORKER;
	unsigned long ipi_latency = ipi_latency_sum / NMSG_PER_WORKER;
	printf("Latency on core %d: %ld cycles per ipi call sent %ld send+recv cycles per call, %ld total cycles, ready %d.\n", cpu_id, ipi_latency, latency, end_tick - start_tick, call_arg->ready);

	return NULL;
}

int main(int argc, char *argv[])
{
	volatile int ret;

	printf("bench_ipi: not running dune yet\n");

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
	printf("bench_ipi: now printing from dune mode on core %d\n", sched_getcpu());

	uint64_t cr3 = 0;
    asm("mov %%cr3,%0" : "=r"(cr3));
	printf("cr3 %lx\n", cr3);
	pthread_t pthreads[NUM_THREADS + 1];
	volatile bool ready[NUM_THREADS + 1]; 
	int i;
	for (i = 0; i < NUM_THREADS; i++) {
		pthread_attr_t attr;
		cpu_set_t cpus;
		pthread_attr_init(&attr);
		CPU_ZERO(&cpus);
		CPU_SET(i, &cpus);
		pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
		pthread_create(&pthreads[i], &attr, t_start, (void *)&ready[i]);
	}

	pthread_attr_t attr;
	cpu_set_t cpus;
	pthread_attr_init(&attr);
	CPU_ZERO(&cpus);
	CPU_SET(TARGET_CORE, &cpus);
	pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
	pthread_create(&pthreads[NUM_THREADS], &attr, ipi_handler_thread, (void *)&ready[NUM_THREADS]);

	rdtsc_overhead = measure_tsc_overhead();
	synch_tsc();

	for (i = 0; i < NUM_THREADS + 1; i++) {
		while (!ready[i]);
	}
	asm volatile("mfence" ::: "memory");
	all_ready = true;
	asm volatile("mfence" ::: "memory");

	

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_join(pthreads[i], NULL);
	}
	asm volatile("mfence" ::: "memory");
	all_done = true;
	asm volatile("mfence" ::: "memory");

	pthread_join(pthreads[NUM_THREADS], NULL);
	return 0;
}
