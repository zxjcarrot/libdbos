# DBOS

# Overview

This repos contains recipes of a new paradigm for DB-OS co-design leveraging privileged kernel bypass.  The DB-OS co-design with privileged kernel bypass aims at providing the DBMS with more power to design new abstractions that are impossible to do in user space.  Privileged Kernel Bypass differs from normal kernel bypass in that it can additionally bypass security sensitive subsystems (virtual memory, scheduler, interrupt.) of Linux kernel. This is achieved with the help of the dune hypervisor for providing privileged process abstraciton.

The directory layout of this repo is as follows:
* apps/tabby -> the DB-OS co-design buffer manager accelerated by virtual memory hadrawre withou TLB shootdown.
* apps/redis -> the moodifed Redis that can take memory snapshots for persistence instantaneously.
* kern/    -> the Dune kernel module of the hypervisor implementation
* libdune/ -> the libdbos library OS adapted from libdune, containing additional enhancements to memory management and interrupts. 
* bench/   -> a series of benchmarks to compare Dune and Linux performance
* test/    -> simple test programs and examples

----
# Requirements
* A 64-bit x86 Linux environment
* A recent Intel CPU with VT-x support.
* A recent kernel version --- We use 6.2 and later, but earlier versions
  may also work.
* Kernel headers must be installed for the running kernel.
* Currently, the hypervisor does not support x2apic and kaslr. You need to add `nox2apic` and `nokaslr` to the kernel boot arguments to disable them. See [dune limitations](README.dune.md#limitations) for more.
* We provide a script called `dune_req.sh` that will attempt to verify
if these requirements are met.

----
# Setup

```
$ make
# insmod kern/dune.ko
# test/hello
```

You'll need to be root to compile and load the module. However, applications can use
Dune without running as root; simply change the permission of '/dev/dune'
accordingly.

Another program worth trying after Dune is setup is the Dune benchmark suite.
It can be run with the following command:

```
$ make -C bench
# bench/bench_dune
```
----
# Programming

1. Call `dbos_init()` to initialize the library os.
2. In a thread, call `dbos_enter()` to enter dune mode.
3. Override page fault handlers with `dune_register_pgflt_handler`; Override system call handler with `dune_register_syscall_handler`;Override interrupt handler with `dune_register_intr_handler`.

----


# Steps
```shell
cd dune
make clean; make # rebuild dune module 
echo 20000 > /proc/sys/vm/nr_hugepages # allocate huge pages for virtualization
``````