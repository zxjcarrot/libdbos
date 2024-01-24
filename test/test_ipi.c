#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>

#include "libdune/dune.h"

#define NUM_THREADS 30
#define TEST_VECTOR 0xF2

volatile bool t2_ready = false;
volatile bool all_ready = false;
volatile bool done[NUM_THREADS];

typedef struct ipi_call_arg_t {
	int id;
	int ready;
}ipi_call_arg_t;

static void ipi_call1(void * arg) {
	ipi_call_arg_t* call_arg = (ipi_call_arg_t*)arg;
	call_arg->ready++;
	asm volatile("mfence" ::: "memory");
	dune_printf("call_arg id %d ready on core %d\n", call_arg->id, dune_ipi_get_cpu_id());
}

void *t_start(void *arg) {
	volatile int ret = dune_enter();
	if (ret) {
		printf("posted_ipi: failed to enter dune in thread 2\n");
		return NULL;
	}
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	printf("Thread running on core %d\n", sched_getcpu());
	asm volatile("mfence" ::: "memory");
	*(volatile bool *)arg = true;
	//while (!received_posted_ipi[sched_getcpu()]);
	while(!all_ready);

	if (cpu_id > 0) { // wait for the previous core done with ipi
		asm volatile("mfence" ::: "memory");
		while (done[cpu_id - 1] == false);
	}
	
	// set up a ipi call for the next core and 
	// wait for it be called in the next call which marks the ready flag in arg
	char * page = dune_ipi_percpu_working_page(cpu_id);
	//printf("page %p\n", page);
	volatile ipi_call_arg_t* call_arg = (ipi_call_arg_t*)page;
	call_arg->id = cpu_id;
	call_arg->ready = 0;
	ipi_message_t msg;
	msg.func = ipi_call1;
	msg.arg = call_arg;
	msg.type = CALL_TYPE_NON_BATCHABLE;
	if (cpu_id < NUM_THREADS -1) {
		//dune_queue_ipi_call(cpu_id, cpu_id + 1, msg);
		dune_queue_ipi_call(cpu_id, cpu_id + 1, msg);
		dune_send_ipi(cpu_id, cpu_id + 1);
	} else { // self-ipi
		//dune_queue_ipi_call(cpu_id, cpu_id, msg);
		dune_queue_ipi_call(cpu_id, cpu_id, msg);
		dune_send_ipi(cpu_id, cpu_id);
	}
	
	while (call_arg->ready < 2);

	asm volatile("mfence" ::: "memory");
	done[cpu_id] = true;
	asm volatile("mfence" ::: "memory");
	return NULL;
}

int main(int argc, char *argv[])
{
	volatile int ret;

	printf("posted_ipi: not running dune yet\n");

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
	printf("posted_ipi: now printing from dune mode on core %d\n", sched_getcpu());

	pthread_t pthreads[NUM_THREADS];
	volatile bool ready[NUM_THREADS]; 
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

	for (i = 0; i < NUM_THREADS; i++) {
		while (!ready[i]);
	}
	asm volatile("mfence" ::: "memory");
	all_ready = true;
	asm volatile("mfence" ::: "memory");

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_join(pthreads[i], NULL);
	}

	printf("passed\n");
	return 0;
}
