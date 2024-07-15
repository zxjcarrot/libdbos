/**
 * dune.h - public header for Dune support
 */

#pragma once

#ifndef __ASSEMBLY__

#include <linux/types.h>

/*
 * IOCTL interface
 */

/* FIXME: this must be reserved in miscdevice.h */
#define DUNE_MINOR 233

#define DUNE_ENTER		  _IOR(DUNE_MINOR, 0x01, struct dune_config)
#define DUNE_GET_SYSCALL  _IO(DUNE_MINOR, 0x02)
#define DUNE_GET_LAYOUT	  _IOW(DUNE_MINOR, 0x03, struct dune_layout)
#define DUNE_TRAP_ENABLE  _IOR(DUNE_MINOR, 0x04, struct dune_trap_config)
#define DUNE_TRAP_DISABLE _IO(DUNE_MINOR, 0x05)

// XXX: Must match libdune/dune.h
#define DUNE_SIGNAL_INTR_BASE 200

struct dune_config {
	__s64 ret;
	__u64 rax;
	__u64 rbx;
	__u64 rcx;
	__u64 rdx;
	__u64 rsi;
	__u64 rdi;
	__u64 rsp;
	__u64 rbp;
	__u64 r8;
	__u64 r9;
	__u64 r10;
	__u64 r11;
	__u64 r12;
	__u64 r13;
	__u64 r14;
	__u64 r15;
	__u64 rip;
	__u64 rflags;
	__u64 cr3;
	__s64 status;
	__u64 vcpu;
} __attribute__((packed));

struct dune_layout {
	__u64 phys_limit;
	__u64 base_map;
	__u64 base_stack;
} __attribute__((packed));

struct dune_trap_regs {
	__u64 rax;
	__u64 rbx;
	__u64 rcx;
	__u64 rdx;
	__u64 rsi;
	__u64 rdi;
	__u64 rsp;
	__u64 rbp;
	__u64 r8;
	__u64 r9;
	__u64 r10;
	__u64 r11;
	__u64 r12;
	__u64 r13;
	__u64 r14;
	__u64 r15;
	__u64 rip;
	__u64 rflags;
} __attribute__((packed));

typedef void (*dune_trap_notify_func)(struct dune_trap_regs *, void *);

struct dune_trap_config {
	__u64 trigger_rip;
	dune_trap_notify_func notify_func;
	struct dune_trap_regs *regs;
	__u64 regs_size;
	void *priv;
	__u8 delay;
} __attribute__((packed));

#define GPA_STACK_SIZE ((unsigned long)1 << 30) /* 1 gigabyte */
#define GPA_MAP_SIZE                                                           \
	(((unsigned long)1 << 35) - GPA_STACK_SIZE) /* 63 gigabytes */
#define LG_ALIGN(addr) ((addr + (1 << 30) - 1) & ~((1 << 30) - 1))

/* FIXME: magic page that maps to APIC of the host */
#define GPA_APIC_PAGE ((1ul<<46)-4096)
#define GPA_POSTED_INTR_DESCS (1ul<<45)

#define GPA_PV_INFO_PAGES ((1ul<<46)- (4096 * 1024))

#define DUNE_PV_TLB_IN_HOST 		 0x00000001
#define DUNE_PV_TLB_FLUSH_ON_REENTRY 0x00000002
#define DUNE_PV_CHANGE_GUEST_CR3 	 0x00000004
#define DUNE_PV_FEATURE_MASK (0x0000000000000007UL)
#define DUNE_PV_CHANGE_GUEST_CR3_MASK (0xFFFFFFFFFFFFFFF8UL)
#define DUNE_PV_TLB_OFFSET_INTO_PI_PAGE 0
typedef struct dune_pv_info {
	__u64 flag;
	__u32 full_flush_count;
}dune_pv_info;

static int dune_pv_info_cmpswap(struct dune_pv_info * info, __u64 old_flag, __u64 new_flag) {
	return __sync_bool_compare_and_swap(&info->flag, old_flag, new_flag);
}

#endif /* __ASSEMBLY__ */

#define IOCTL_DUNE_ENTER 0x80b0e901

#define DUNE_CFG_RET	0x00
#define DUNE_CFG_RAX	0x08
#define DUNE_CFG_RBX	0x10
#define DUNE_CFG_RCX	0x18
#define DUNE_CFG_RDX	0x20
#define DUNE_CFG_RSI	0x28
#define DUNE_CFG_RDI	0x30
#define DUNE_CFG_RSP	0x38
#define DUNE_CFG_RBP	0x40
#define DUNE_CFG_R8		0x48
#define DUNE_CFG_R9		0x50
#define DUNE_CFG_R10	0x58
#define DUNE_CFG_R11	0x60
#define DUNE_CFG_R12	0x68
#define DUNE_CFG_R13	0x70
#define DUNE_CFG_R14	0x78
#define DUNE_CFG_R15	0x80
#define DUNE_CFG_RIP	0x88
#define DUNE_CFG_RFLAGS 0x90
#define DUNE_CFG_CR3	0x98
#define DUNE_CFG_STATUS 0xa0
#define DUNE_CFG_VCPU	0xa8

#define DUNE_RET_EXIT			  1
#define DUNE_RET_EPT_VIOLATION	  2
#define DUNE_RET_INTERRUPT		  3
#define DUNE_RET_SIGNAL			  4
#define DUNE_RET_UNHANDLED_VMEXIT 5
#define DUNE_RET_NOENTER		  6
