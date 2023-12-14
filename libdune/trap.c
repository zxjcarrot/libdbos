/*
 * trap.c - x86 fault handling
 */

#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>

#include "dune.h"
#include "cpu-x86.h"

static dune_syscall_cb syscall_cb;
static dune_pgflt_cb pgflt_cb;
static dune_intr_cb intr_cbs[IDT_ENTRIES];

static inline unsigned long read_cr2(void)
{
	unsigned long val;
	asm volatile("mov %%cr2, %0\n\t" : "=r"(val));
	return val;
}

int dune_register_intr_handler(int vec, dune_intr_cb cb)
{
	if (vec >= IDT_ENTRIES || vec < 0)
		return -EINVAL;
	printf("modify %p vec %d to %p\n", intr_cbs, vec, cb);
	intr_cbs[vec] = cb;
	return 0;
}

int dune_register_signal_handler(int signum, dune_intr_cb cb)
{
	return dune_register_intr_handler(DUNE_SIGNAL_INTR_BASE + signum, cb);
}

void dune_register_syscall_handler(dune_syscall_cb cb)
{
	syscall_cb = cb;
}

void dune_register_pgflt_handler(dune_pgflt_cb cb)
{
	pgflt_cb = cb;
}

static bool addr_is_mapped(void *va)
{
	int ret;
	ptent_t *pte;

	ret = dune_vm_lookup(pgroot, va, CREATE_NONE, &pte);
	if (ret)
		return 0;

	if (!(*pte & PTE_P))
		return 0;

	return 1;
}

#define STACK_DEPTH 20

static void dune_dump_stack(struct dune_tf *tf)
{
	int i;
	unsigned long *sp = (unsigned long *)tf->rsp;

	// we use dune_printf() because this might
	// have to work even if libc doesn't.
	dune_printf("dune: Dumping Stack Contents %llx...\n", tf->rsp);
	for (i = 0; i < STACK_DEPTH; i++) {
		if (!addr_is_mapped(&sp[i])) {
			dune_printf("dune: reached unmapped addr\n");
			break;
		}
		dune_printf("dune: RSP%+-3d 0x%016lx\n", i * sizeof(long), sp[i]);
	}
}

static void dune_hexdump(void *x, int len)
{
	unsigned char *p = x;

	while (len--)
		dune_printf("%.2x ", *p++);

	dune_printf("\n");
}

static void dump_ip(struct dune_tf *tf)
{
	unsigned char *p = (void *)tf->rip;
	int len = 20;

	dune_printf("dune: code before IP\t");
	dune_hexdump(p - len, len);

	dune_printf("dune: code at IP\t");
	dune_hexdump(p, len);
}

static uint64_t search_rip;
void dune_procmap_find_mapping(const struct dune_procmap_entry * e) {
	if (e->begin <= search_rip && search_rip <= e->end) {
		dune_printf("dune: found mapping %s for rip %lx : 0x%016lx-0x%016lx %c%c%c%c %08lx %s %lu KiB\n", e->path, search_rip, e->begin, e->end,
		   e->r ? 'R' : '-', e->w ? 'W' : '-', e->x ? 'X' : '-',
		   e->p ? 'P' : 'S', e->offset, e->path, (uint64_t)(e->end - e->begin) / 1024);
	}
}

extern void dune_procmap_iterate(dune_procmap_cb cb);
void dune_dump_trap_frame(struct dune_tf *tf)
{
	search_rip = tf->rip;
	dune_procmap_iterate(dune_procmap_find_mapping);
	// we use dune_printf() because this might
	// have to work even if libc doesn't.
	dune_printf("dune: --- Begin Trap Dump ---\n");
	dune_printf("dune: RIP 0x%016llx\n", tf->rip);
	dune_printf("dune: CS 0x%02x SS 0x%02x\n", tf->cs, tf->ss);
	dune_printf("dune: ERR 0x%08lx RFLAGS 0x%08lx\n", tf->err, tf->rflags);
	dune_printf("dune: RAX 0x%016lx RCX 0x%016lx\n", tf->rax, tf->rcx);
	dune_printf("dune: RDX 0x%016lx RBX 0x%016lx\n", tf->rdx, tf->rbx);
	dune_printf("dune: RSP 0x%016lx RBP 0x%016lx\n", tf->rsp, tf->rbp);
	dune_printf("dune: RSI 0x%016lx RDI 0x%016lx\n", tf->rsi, tf->rdi);
	dune_printf("dune: R8  0x%016lx R9  0x%016lx\n", tf->r8, tf->r9);
	dune_printf("dune: R10 0x%016lx R11 0x%016lx\n", tf->r10, tf->r11);
	dune_printf("dune: R12 0x%016lx R13 0x%016lx\n", tf->r12, tf->r13);
	dune_printf("dune: R14 0x%016lx R15 0x%016lx\n", tf->r14, tf->r15);
	dune_dump_stack(tf);
	dump_ip(tf);
	dune_printf("dune: --- End Trap Dump ---\n");
}

void dune_syscall_handler(struct dune_tf *tf)
{
	if (syscall_cb) {
		syscall_cb(tf);
	} else {
		dune_printf("missing handler for system call - #%d\n", tf->rax);
		dune_dump_trap_frame(tf);
		//dune_die();
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
	}
}

void dune_trap_handler(int num, struct dune_tf *tf)
{
	// unsigned long ret;
	// asm volatile("movq $500, %%rax\n\t" // print cpu states
	// 			 "movq %1, %%rdi\n\t" // print cpu states
	// 			 "movq %2, %%rsi\n\t"
	// 			 "movq %3, %%rdx\n\t"
	// 			 "movq %4, %%rcx\n\t"
	// 			 "vmcall\n\t"
	// 			 "movq %%rax, %0\n\t"
	// 			 : "=r"((unsigned long)ret)
	// 			 :"r"((unsigned long) num), "r"((unsigned long) tf->rip), "r"((unsigned long)tf->rsp), "r"((unsigned long)tf->err): "rax", "rdi", "rsi", "rdx", "rcx");
	//dune_printf("trap %d\n", num);
	if (intr_cbs[num]) {
		intr_cbs[num](tf);
		return;
	}

	switch (num) {
	case T_PGFLT:
		if (pgflt_cb) {
			pgflt_cb(read_cr2(), tf->err, tf);
		} else {
			//printf("unhandled page fault %lx %lx\n", read_cr2(), tf->err);
			dune_printf("unhandled page fault %lx %lx sp %lx rip %lx\n", read_cr2(), tf->err, tf->rsp, tf->rip);
			//dune_procmap_dump();
			//dune_dump_trap_frame(tf);
			dune_die();
			// asm volatile("movq $60, %rax\n" // exit
			// 	 "vmcall\n");
		}
		break;

	case T_NMI:
	case T_DBLFLT:
	case T_GPFLT:
		dune_printf("fatal exception %d, code %lx - dying...\n", num, tf->err);
		dune_dump_trap_frame(tf);
		dune_die();
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;

	case T_DEBUG:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_DIVIDE:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_TSS:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_STACK:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_OFLOW:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_ILLOP:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_DEVICE:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_FPERR:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	case T_ALIGN:
		asm volatile("movq $60, %rax\n" // exit
				 "vmcall\n");
		break;
	default:
		dune_printf("unhandled exception %d\n", num);
		dune_dump_trap_frame(tf);
		dune_die();
		// asm volatile("movq $60, %rax\n" // exit
		// 		 "vmcall\n");
	}
}
