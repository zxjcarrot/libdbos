#pragma once
#include <malloc.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>


extern void dune_set_cpu_id(int cpuid);
extern int dune_get_cpu_id();
extern void dune_set_max_cores(int n);
extern int dune_get_max_cores();
extern void dune_pv_kick(int cpu);