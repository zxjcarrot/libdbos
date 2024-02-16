#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "libdune/dune.h"
#include "libdune/cpu-x86.h"

static void recover(void)
{
	printf("hello: recovered from divide by zero\n");
	exit(0);
}

static void divide_by_zero_handler(struct dune_tf *tf)
{
	printf("hello: caught divide by zero!\n");
	tf->rip = (uintptr_t)&recover;
}

// __thread void* workspace = NULL;
// static size_t workspace_size = 4096 * 10;

#include <sys/epoll.h>

static void dune_sys_patch_open(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	const char *filename = (const char *)ARG1(tf);
	size_t len = strlen(filename);
	assert(len + 1 <= kWorkspaceSize);
	strncpy(workspace, filename, len);
	ARG1(new_tf) = workspace;
	dune_passthrough_g0_syscall(new_tf);
	tf->rax = new_tf->rax;
}

static void dune_sys_patch_write(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	const void *buf = (const void *)ARG1(tf);
	size_t count = (size_t)ARG2(tf);
	assert(count <= kWorkspaceSize);
	memcpy(workspace, buf, count);
	ARG1(new_tf) = workspace;
	dune_passthrough_g0_syscall(new_tf);
	tf->rax = new_tf->rax;
}

static void dune_sys_patch_read(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	void *source_buf = (const void *)ARG1(tf);
	size_t count = (size_t)ARG2(tf);
	assert(count <= kWorkspaceSize);
	ARG1(new_tf) = workspace;
	dune_passthrough_g0_syscall(new_tf);
	ssize_t ret = new_tf->rax;
	if (ret > 0) {
		memcpy(source_buf, workspace, count);
	}
	tf->rax = new_tf->rax;
}

static void dune_sys_patch_epoll_wait(void * workspace, size_t kWorkspaceSize, struct dune_tf *new_tf, struct dune_tf *tf) {
	assert(workspace != NULL);
	struct epoll_event * events = (struct epoll_event *)ARG1(tf);
	int maxevents = (size_t)ARG2(tf);
	assert(sizeof(struct epoll_event) * maxevents <= kWorkspaceSize);
	ARG1(new_tf) = (uint64_t)workspace;
	dune_passthrough_g0_syscall(new_tf);
	ssize_t ret = new_tf->rax;
	if (ret > 0) {
		memcpy(events, workspace, sizeof(struct epoll_event) * ret);
	}
	tf->rax = new_tf->rax;
}

static void my_syscall_handler(struct dune_tf *tf)
{
	static const size_t kWorkspaceSize = 4096 * 2;
	char workspace[kWorkspaceSize];

  	int syscall_num = (int) tf->rax;

	dune_printf("Got system call %d, workspace %p, SYS_write %d\n", syscall_num, workspace, SYS_write);

	struct dune_tf new_tf = *tf;
	struct dune_tf * new_tfp = &new_tf;

	switch (syscall_num)
	{
	case SYS_open:
		dune_sys_patch_open(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	case SYS_write:
		dune_sys_patch_write(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	case SYS_read: 
		dune_sys_patch_read(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	case SYS_epoll_wait:
		dune_sys_patch_epoll_wait(workspace, kWorkspaceSize, new_tfp, tf);
		break;
	default:
		dune_passthrough_g0_syscall(tf);
		break;
	}
}

int main(int argc, char *argv[])
{
	volatile int ret;

	printf("hello: not running dune yet\n");


	ret = dune_init_and_enter();
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}
	dune_register_g0_syscall_handler(my_syscall_handler);

	
	printf("hello: now printing from dune mode\n");

	dune_register_intr_handler(T_DIVIDE, divide_by_zero_handler);

	printf("hello: now printing from dune mode again\n");

	ret = 1 / ret; /* divide by zero */

	printf("hello: we won't reach this call\n");

	return 0;
}
