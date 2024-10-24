#define _GNU_SOURCE
#include "dune.h"
#include "cpu.h"

static __thread int cpu_id = -1;

static int max_n_cores = 32; // maximum number of cores used by the application

void dune_set_cpu_id(int cpuid) {
    cpu_id = cpuid;
}

int dune_get_cpu_id() {
    return cpu_id;
}

void dune_set_max_cores(int n) {
	assert(n <= 128);
	max_n_cores = n;
}

int dune_get_max_cores() {
    return	max_n_cores;
}

void dune_pv_kick(int cpu) {
    int ret = syscall(800, cpu);
    assert(ret == 0);
}

void dune_pv_enable_record_stats(int cpu) {
    int ret = syscall(801);
    assert(ret == 0);
}

void dune_pv_disable_record_stats(int cpu) {
    int ret = syscall(802);
    assert(ret == 0);
}