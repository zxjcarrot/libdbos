#define _GNU_SOURCE
#include "cpu.h"

static __thread int cpu_id = -1;

void dune_set_cpu_id(int cpuid) {
    cpu_id = cpuid;
}

int dune_get_cpu_id() {
    return cpu_id;
}