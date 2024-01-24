#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "libdune/dune.h"

#define N		 10000
#define MAP_ADDR 0x400000000000

static unsigned long tsc;

static void pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	ptent_t *pte;

	dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	*pte |= PTE_P | PTE_W | PTE_U | PTE_A | PTE_D;
}

static void syscall_handler1(struct dune_tf *tf)
{
	dune_ret_from_user(0);
}

static void userlevel_pgflt(void)
{
	char *p = (char *)MAP_ADDR;
	*p = 1;

	syscall(SYS_gettid);
}

static int test_pgflt(void)
{
	int ret;
	unsigned long sp;
	struct dune_tf *tf = malloc(sizeof(struct dune_tf));
	if (!tf)
		return -ENOMEM;

	printf("testing page fault from G3... ");

	ret = dune_vm_map_pages(pgroot, (void *)MAP_ADDR, 1, PERM_R);
	if (ret) {
		printf("failed to setup memory mapping\n");
		return ret;
	}

	dune_register_pgflt_handler(pgflt_handler);
	dune_register_syscall_handler(&syscall_handler1);

	asm("movq %%rsp, %0" : "=r"(sp));

	tf->rip = (unsigned long)&userlevel_pgflt;
	tf->rsp = sp - 10000;
	tf->rflags = 0x02;

	ret = dune_jump_to_user(tf);

	if (!ret)
		printf("[passed]\n");

	return ret;
}

static void userlevel_empty_call(void)
{
	tsc++;
	syscall(SYS_gettid);
}

static void userlevel_syscall(void)
{
	int i;
	for (i = 0; i < N; i++) {
		syscall(SYS_gettid);
	}
}

static void syscall_handler2(struct dune_tf *tf)
{
	static int syscall_count = 0;

	syscall_count++;
	if (syscall_count == N) {
		printf("[took %ld cycles]\n", (dune_get_ticks() - tsc) / N);
		dune_ret_from_user(0);
	}
	dune_passthrough_syscall(tf);
}


static void syscall_handler3(struct dune_tf *tf)
{
	dune_ret_from_user(0);
}

static int test_syscall(void)
{
	int ret;
	unsigned long sp;
	struct dune_tf *tf = malloc(sizeof(struct dune_tf));
	if (!tf)
		return -ENOMEM;

	printf("measuring round-trip G3 syscall performance... ");

	dune_register_syscall_handler(&syscall_handler2);

	asm("movq %%rsp, %0" : "=r"(sp));

	tf->rip = (unsigned long)&userlevel_syscall;
	tf->rsp = sp - 10000;
	tf->rflags = 0x0;

	tsc = dune_get_ticks();
	ret = dune_jump_to_user(tf);

	return ret;
}


static int test_ring_switch(void)
{
	int ret;
	unsigned long sp;
	unsigned long start_tsc;
	struct dune_tf *tf = malloc(sizeof(struct dune_tf));
	if (!tf)
		return -ENOMEM;

	printf("measuring ring switch overhead... ");

	dune_register_syscall_handler(&syscall_handler3);

	start_tsc = dune_get_ticks();
	for (int i = 0; i < N; ++i) {
		asm("movq %%rsp, %0" : "=r"(sp));

		tf->rip = (unsigned long)&userlevel_empty_call;
		tf->rsp = sp - 10000;
		tf->rflags = 0x0;

		ret = dune_jump_to_user(tf);
	}
	
	printf("each ring switch took %ld cycles\n", (dune_get_ticks() - start_tsc) / N);
	free(tf);
	return ret;
}

static void syscall_handler4(struct dune_tf *tf)
{
	if (tf->rax == 555) {
		return;
	}
	dune_ret_from_user(0);
}

static void g3_syscall_bench() {
	for (int i = 0; i < N; ++i) {
		syscall(555); // perform an empty system call
	}
	syscall(666); // exit from userspace
}

static int test_g3_syscall(void)
{
	int ret;
	unsigned long sp;
	unsigned long start_tsc;
	struct dune_tf *tf = malloc(sizeof(struct dune_tf));
	if (!tf)
		return -ENOMEM;

	printf("measuring g3 empty syscall overhead... ");

	dune_register_syscall_handler(&syscall_handler4);

	start_tsc = dune_get_ticks();
	//for (int i = 0; i < N; ++i) {
		asm("movq %%rsp, %0" : "=r"(sp));

		tf->rip = (unsigned long)&g3_syscall_bench;
		tf->rsp = sp - 10000;
		tf->rflags = 0x0;

		ret = dune_jump_to_user(tf);
	//}
	
	printf("each syscall round-trip took %ld cycles\n", (dune_get_ticks() - start_tsc) / N);
	free(tf);
	return ret;
}

int main(int argc, char *argv[])
{
	int ret;

	ret = dune_init_and_enter();
	if (ret) {
		printf("failed to initialize DUNE\n");
		return ret;
	}

	test_pgflt();
	test_syscall();
	test_ring_switch();
	test_g3_syscall();
	return 0;
}
