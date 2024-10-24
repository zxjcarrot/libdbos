/*
 * entry.c - Handles transition into dune mode
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <asm/prctl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <err.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdlib.h>

#include "dune.h"
#include "mmu.h"
#include "cpu-x86.h"
#include "local.h"
#include "debug.h"
#include "fast_spawn.h"

#define BUILD_ASSERT(cond)                                                     \
	do {                                                                       \
		(void)sizeof(char[1 - 2 * !(cond)]);                                   \
	} while (0)

ptent_t *pgroot;
uintptr_t phys_limit;
uintptr_t mmap_base;
uintptr_t stack_base;

int dune_fd;

static struct idtd idt[IDT_ENTRIES];

static uint64_t gdt_template[NR_GDT_ENTRIES] = {
	0,
	0,
	SEG64(SEG_X | SEG_R, 0),
	SEG64(SEG_W, 0),
	0,
	SEG64(SEG_W, 3),
	SEG64(SEG_X | SEG_R, 3),
	0,
	0,
};

struct dune_percpu {
	uint64_t percpu_ptr;
	uint64_t tmp;
	uint64_t kfs_base;
	uint64_t ufs_base;
	uint64_t in_usermode;
	struct Tss tss;
	uint64_t gdt[NR_GDT_ENTRIES];
} __attribute__((packed));

static __thread struct dune_percpu *lpercpu;

struct dynsym {
	char *ds_name;
	int ds_idx;
	int ds_off;
	struct dynsym *ds_next;
};

unsigned long dune_get_user_fs(void)
{
	void *ptr;
	asm("movq %%gs:%c[ufs_base], %0"
		: "=r"(ptr)
		: [ufs_base] "i"(offsetof(struct dune_percpu, ufs_base))
		: "memory");
	return (unsigned long)ptr;
}

void dune_set_user_fs(unsigned long fs_base)
{
	asm("movq %0, %%gs:%c[ufs_base]"
		:
		: "r"(fs_base), [ufs_base] "i"(offsetof(struct dune_percpu, ufs_base)));
}

static void map_ptr(void *p, int len)
{
	unsigned long page = PGADDR(p);
	unsigned long page_end = PGADDR((char *)p + len);
	unsigned long l = (page_end - page) + PGSIZE;
	void *pg = (void *)page;

	dune_vm_map_phys(pgroot, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_W);
}

static int setup_safe_stack(struct dune_percpu *percpu)
{
	int i;
	char *safe_stack;

	safe_stack = mmap(NULL, PGSIZE, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (safe_stack == MAP_FAILED)
		return -ENOMEM;

	map_ptr(safe_stack, PGSIZE);

	safe_stack += PGSIZE;
	percpu->tss.tss_iomb = offsetof(struct Tss, tss_iopb);

	for (i = 1; i < 8; i++)
		percpu->tss.tss_ist[i] = (uintptr_t)safe_stack;

	/* changed later on jump to G3 */
	percpu->tss.tss_rsp[0] = (uintptr_t)safe_stack;

	return dune_vm_map_pages_add_flags(pgroot, safe_stack, PGSIZE, PERM_NOCOW);
}

static void setup_gdt(struct dune_percpu *percpu)
{
	memcpy(percpu->gdt, gdt_template, sizeof(uint64_t) * NR_GDT_ENTRIES);

	percpu->gdt[GD_TSS >> 3] =
		(SEG_TSSA | SEG_P | SEG_A | SEG_BASELO(&percpu->tss) |
		 SEG_LIM(sizeof(struct Tss) - 1));
	percpu->gdt[GD_TSS2 >> 3] = SEG_BASEHI(&percpu->tss);
}

/**
 * dune_boot - Brings the user-level OS online
 * @percpu: the thread-local data
 */
static int dune_boot(struct dune_percpu *percpu)
{
	struct tptr _idtr, _gdtr;

	setup_gdt(percpu);

	_gdtr.base = (uint64_t)&percpu->gdt;
	_gdtr.limit = sizeof(percpu->gdt) - 1;

	_idtr.base = (uint64_t)&idt;
	_idtr.limit = sizeof(idt) - 1;

	asm volatile(
		// STEP 1: load the new GDT
		"lgdt %0\n"

		// STEP 2: initialize data segements
		"mov $" __str(
			GD_KD) ", %%ax\n"
				   "mov %%ax, %%ds\n"
				   "mov %%ax, %%es\n"
				   "mov %%ax, %%ss\n"

				   // STEP 3: long jump into the new code segment
				   "mov $" __str(
					   GD_KT) ", %%rax\n"
							  "pushq %%rax\n"
							  "pushq $1f\n"
							  "lretq\n"
							  "1:\n"
							  "nop\n"

							  // STEP 4: load the task register (for safe stack switching)
							  "mov $" __str(
								  GD_TSS) ", %%ax\n"
										  "ltr %%ax\n"

										  // STEP 5: load the new IDT and enable interrupts
										  "lidt %1\n"
										  "sti\n"

		:
		: "m"(_gdtr), "m"(_idtr)
		: "rax");

	// STEP 6: FS and GS require special initialization on 64-bit
	wrmsrl(MSR_FS_BASE, percpu->kfs_base);
	wrmsrl(MSR_GS_BASE, (unsigned long)percpu);

	return 0;
}

#define ISR_LEN 16

static inline void set_idt_addr(struct idtd *id, physaddr_t addr)
{
	id->low = addr & 0xFFFF;
	id->middle = (addr >> 16) & 0xFFFF;
	id->high = (addr >> 32) & 0xFFFFFFFF;
}

static void setup_idt(void)
{
	int i;

	for (i = 0; i < IDT_ENTRIES; i++) {
		struct idtd *id = &idt[i];
		uintptr_t isr = (uintptr_t)&__dune_intr;

		isr += ISR_LEN * i;
		memset(id, 0, sizeof(*id));

		id->selector = GD_KT;
		id->type = IDTD_P | IDTD_TRAP_GATE;

		switch (i) {
		case T_BRKPT:
			id->type |= IDTD_CPL3;
			/* fallthrough */
		case T_DBLFLT:
		case T_NMI:
		case T_MCHK:
			id->ist = 1;
			break;
		}

		set_idt_addr(id, isr);
	}
}

static int setup_syscall(void)
{
	unsigned long lstar;
	unsigned long lstara;
	unsigned char *page;
	ptent_t *pte;
	size_t off;
	int i;

	assert((unsigned long)__dune_syscall_end - (unsigned long)__dune_syscall <
		   PGSIZE);

	lstar = ioctl(dune_fd, DUNE_GET_SYSCALL);
	if (lstar == -1)
		return -errno;

	page = mmap((void *)NULL, PGSIZE * 2, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANON, -1, 0);

	if (page == MAP_FAILED)
		return -errno;

	lstara = lstar & ~(PGSIZE - 1);
	off = lstar - lstara;

	memcpy(page + off, __dune_syscall,
		   (unsigned long)__dune_syscall_end - (unsigned long)__dune_syscall);

	for (i = 0; i <= PGSIZE; i += PGSIZE) {
		uintptr_t pa = dune_mmap_addr_to_pa(page + i);
		dune_vm_lookup(pgroot, (void *)(lstara + i), 1, &pte);
		*pte = PTE_ADDR(pa) | PTE_P | PTE_NOCOW;
	}

	return dune_vm_map_pages_add_flags(pgroot, page, PGSIZE * 2, PERM_NOCOW);
}

static int setup_apic_mapping(void)
{
	map_ptr((void *) APIC_BASE, 0);
	int ret = dune_vm_map_pages_add_flags(pgroot, APIC_BASE, PGSIZE, PERM_NOCOW);
	if (ret) {
		return ret;
	}
	return 0;
}

static int setup_posted_intr_desc_mapping(void)
{
	int i;
	int ret = 0;
	for (i = 0; i < get_nprocs(); i++) {
		map_ptr((void *) (POSTED_INTR_DESCS_BASE + (i * PAGE_SIZE)), 0);
		ret = dune_vm_map_pages_add_flags(pgroot, (void *) (POSTED_INTR_DESCS_BASE + (i * PAGE_SIZE)), PGSIZE, PERM_NOCOW);
		if (ret) {
			return ret;
		}
	}
	return 0;
}

static int setup_tlb_info_page_mapping(void)
{
	int i;
	int ret = 0;
	for (i = 0; i < get_nprocs(); i++) {
		map_ptr((void *) (PV_INFO_PAGES_BASE+ (i * PAGE_SIZE)), 0);
		ret = dune_vm_map_pages_add_flags(pgroot, (void *) (PV_INFO_PAGES_BASE + (i * PAGE_SIZE)), PGSIZE, PERM_NOCOW);
		if (ret) {
			return ret;
		}
	}
	return 0;
}
#define VSYSCALL_ADDR 0xffffffffff600000

static void setup_vsyscall(void)
{
	ptent_t *pte;

	dune_vm_lookup(pgroot, (void *)VSYSCALL_ADDR, 1, &pte);
	*pte = PTE_ADDR(dune_va_to_pa(&__dune_vsyscall_page)) | PTE_P | PTE_U | PTE_NOCOW;
}

static void __setup_mappings_cb(const struct dune_procmap_entry *ent)
{
	int perm = PERM_NONE;
	int ret;
	printf("dune map [%p, %p] %s\n", ent->begin, ent->end, ent->path);
	if (strncmp(ent->path, "[heap]", strlen("[heap]")) == 0) {
		printf("dune map heap [%p, %p]\n", ent->begin, ent->end);
	}
	// page region already mapped
	if (ent->begin == (unsigned long)PAGEBASE)
		return;

	if (ent->begin == (unsigned long)VSYSCALL_ADDR) {
		setup_vsyscall();
		return;
	}

	if (ent->type == PROCMAP_TYPE_VDSO) {
		dune_vm_map_phys(pgroot, (void *)ent->begin, ent->end - ent->begin,
						 (void *)dune_va_to_pa((void *)ent->begin),
						 PERM_U | PERM_R | PERM_X | PERM_NOCOW);
		return;
	}

	if (ent->type == PROCMAP_TYPE_VVAR) {
		dune_vm_map_phys(pgroot, (void *)ent->begin, ent->end - ent->begin,
						 (void *)dune_va_to_pa((void *)ent->begin),
						 PERM_U | PERM_R | PERM_NOCOW);
		return;
	}

	if (ent->r)
		perm |= PERM_R;
	if (ent->w)
		perm |= PERM_W;
	if (ent->x)
		perm |= PERM_X;

	ret = dune_vm_map_phys(pgroot, (void *)ent->begin, ent->end - ent->begin,
						   (void *)dune_va_to_pa((void *)ent->begin), perm);
	assert(!ret);
}

static void __setup_lib_mappings_cb(const struct dune_procmap_entry *ent)
{
	int perm = PERM_NONE;
	int ret;

	// page region already mapped
	if (ent->begin >= PAGEBASE && ent->end <= PAGEBASE_END)
		return;

	if (ent->begin == (unsigned long)VSYSCALL_ADDR) {
		return;
	}

	if (ent->type == PROCMAP_TYPE_VDSO) {
		return;
	}

	if (ent->type == PROCMAP_TYPE_VVAR) {
		return;
	}

	if (ent->r)
		perm |= PERM_R;
	if (ent->w)
		perm |= PERM_W;
	if (ent->x)
		perm |= PERM_X;
	if(strlen(ent->path) > 4 && strstr(ent->path, ".so") != NULL) {
		perm |= PERM_NOCOW;
		printf("library %s found, disallow copy-on-write\n", ent->path);
	}
	ret = dune_vm_map_phys(pgroot, (void *)ent->begin, ent->end - ent->begin,
						   (void *)dune_va_to_pa((void *)ent->begin), perm);
	assert(!ret);
}

static int __setup_mappings_precise(struct dune_layout *layout)
{
	int ret;

	ret = dune_vm_map_phys(pgroot, (void *)PAGEBASE, MAX_PAGES * PGSIZE,
						   (void *)dune_va_to_pa((void *)PAGEBASE),
						   PERM_R | PERM_W | PERM_BIG);
	if (ret)
		return ret;

	dune_procmap_iterate(&__setup_mappings_cb);

	return 0;
}

static void setup_vdso_cb(const struct dune_procmap_entry *ent)
{
	if (ent->type == PROCMAP_TYPE_VDSO) {
		dune_vm_map_phys(pgroot, (void *)ent->begin, ent->end - ent->begin,
						 (void *)dune_va_to_pa((void *)ent->begin),
						 PERM_U | PERM_R | PERM_X | PERM_NOCOW);
		return;
	}

	if (ent->type == PROCMAP_TYPE_VVAR) {
		dune_vm_map_phys(pgroot, (void *)ent->begin, ent->end - ent->begin,
						 (void *)dune_va_to_pa((void *)ent->begin),
						 PERM_U | PERM_R | PERM_NOCOW);
		return;
	}
}

static int __setup_mappings_full(struct dune_layout *layout)
{
	int ret;

	ret = dune_vm_map_phys(pgroot, (void *)0, HEAPEND / 4, (void *)0,
						   PERM_R | PERM_W | PERM_U);
	if (ret)
		return ret;
	printf("dune: start_v_addr %lx start_p_addr %lx, size %lu MiB\n", 0UL,  0UL, (HEAPEND) / 1024 / 1024);

	//uint64_t map_size = 1ULL << 32;
	ret =
		dune_vm_map_phys(pgroot, (void *)layout->base_map, GPA_MAP_SIZE,
						 (void *)dune_mmap_addr_to_pa((void *)layout->base_map),
						 PERM_R | PERM_W | PERM_X | PERM_U);
	if (ret)
		return ret;
	printf("dune: map_v_addr %lx map_p_addr %lx, map_size %lu MiB, [%lx, %lx]\n", (uintptr_t)layout->base_map,  dune_mmap_addr_to_pa((void *)layout->base_map), GPA_MAP_SIZE / 1024 / 1024, (uintptr_t)layout->base_map, (uintptr_t)layout->base_map + GPA_MAP_SIZE);

	ret = dune_vm_map_phys(
		pgroot, (void *)layout->base_stack, GPA_STACK_SIZE,
		(void *)dune_stack_addr_to_pa((void *)layout->base_stack),
		PERM_R | PERM_W | PERM_X | PERM_U | PERM_NOCOW);
	printf("dune: stack_v_addr %lx stack_p_addr %lx, stack_size %lu MiB, [%lx, %lx]\n", (uintptr_t)layout->base_stack, dune_stack_addr_to_pa((void *)layout->base_stack), GPA_STACK_SIZE/1024/1024, (uintptr_t)layout->base_stack, (uintptr_t)layout->base_stack + GPA_STACK_SIZE);
	if (ret)
		return ret;

	ret = dune_vm_map_phys(pgroot, (void *)PAGEBASE, MAX_PAGES * PGSIZE,
						   (void *)dune_va_to_pa((void *)PAGEBASE),
						   PERM_R | PERM_W | PERM_BIG | PERM_NOCOW);
	if (ret)
		return ret;
	printf("dune: pgbase_v_addr %lx pgbase_p_addr [%lx,%lx], PAGE %lu MiB, [%lx, %lx]\n", PAGEBASE, dune_va_to_pa((void *)PAGEBASE), dune_va_to_pa((void *)PAGEBASE) + (MAX_PAGES * PGSIZE), (MAX_PAGES * PGSIZE)/1024/1024, PAGEBASE, PAGEBASE + (MAX_PAGES * PGSIZE));

	dune_procmap_iterate(setup_vdso_cb);
	setup_vsyscall();

	// Now iterate through process mappings to handle shared libraries
	dune_procmap_iterate(&__setup_lib_mappings_cb);
	return 0;
}

static int setup_mappings(bool full)
{
	struct dune_layout layout;
	int ret = ioctl(dune_fd, DUNE_GET_LAYOUT, &layout);
	if (ret)
		return ret;

	phys_limit = layout.phys_limit;
	mmap_base = layout.base_map;
	stack_base = layout.base_stack;
	printf("dune: phys_limit %lx mmap_base %lx stack_base %lx\n", phys_limit, mmap_base, stack_base);
	if (full)
		ret = __setup_mappings_full(&layout);
	else
		ret = __setup_mappings_precise(&layout);

	return ret;
}

static struct dune_percpu *create_percpu(void)
{
	struct dune_percpu *percpu;
	int ret;
	unsigned long fs_base;

	if (arch_prctl(ARCH_GET_FS, &fs_base) == -1) {
		printf("dune: failed to get FS register\n");
		return NULL;
	}

	percpu = mmap(NULL, PGSIZE, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (percpu == MAP_FAILED)
		return NULL;

	map_ptr(percpu, sizeof(*percpu));

	percpu->kfs_base = fs_base;
	percpu->ufs_base = fs_base;
	percpu->in_usermode = 0;

	if ((ret = setup_safe_stack(percpu))) {
		munmap(percpu, PGSIZE);
		return NULL;
	}

	if ((ret = dune_vm_map_pages_add_flags(pgroot, percpu, PGSIZE, PERM_NOCOW))) {
		assert(false);
		return NULL;
	}

	return percpu;
}

static void free_percpu(struct dune_percpu *percpu)
{
	/* XXX free stack */
	munmap(percpu, PGSIZE);
}

static void map_stack_cb(const struct dune_procmap_entry *e)
{
	unsigned long esp;
	fast_spawn_state_t * state = NULL;
	asm("mov %%rsp, %0" : "=r"(esp));

	if (esp >= e->begin && esp < e->end) {
		void *p = (void *)e->begin;
		int len = e->end - e->begin;
		unsigned long page = PGADDR(p);
		unsigned long page_end = PGADDR((char *)p + len);
		unsigned long l = (page_end - page);
		void *pg = (void *)page;
		if (dbos_fast_spawn_enabled) {
			state = dune_get_fast_spawn_state_host();
			printf("map_stack_cb dbos_fast_spawn_enabled, state %p\n", state);
			int cpu_id = dune_get_cpu_id();
			pthread_mutex_lock(&state->mutex);
			printf("map_stack_cb dbos_fast_spawn_enabled, state %p, locked\n", state);
			if (state->shadow_pgroot1 != NULL) {
				assert(state->shadow_pgroot2);
				//dune_vm_map_phys(pgroot, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_X | PERM_W | PERM_USR4);
				dune_vm_map_phys(state->shadow_pgroot1, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_X | PERM_W | PERM_USR4 | PERM_NOCOW);
				dune_vm_map_phys(state->shadow_pgroot2, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_X | PERM_W | PERM_USR4 | PERM_NOCOW);
			} else {
				assert(state->pgroot);
				//assert(state->pgroot == pgroot);
				//dune_vm_map_phys(pgroot, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_X | PERM_W | PERM_USR4);
				dune_vm_map_phys(state->pgroot, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_X | PERM_W | PERM_USR4 | PERM_NOCOW);
				if (state->pgroot_snapshot) {
					dune_vm_map_phys(state->pgroot_snapshot, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_X | PERM_W | PERM_USR4 | PERM_NOCOW);
				}
			}
			pthread_mutex_unlock(&state->mutex);
		} else {
			dune_vm_map_phys(pgroot, pg, l, (void *)dune_va_to_pa(pg), PERM_R | PERM_X | PERM_W | PERM_USR4 | PERM_NOCOW);
		}
		printf("mapped stack [%lx-%lx], page %p, page_end %p, l %lu\n", (uintptr_t)e->begin, (uintptr_t)e->end, page, page_end, l);
	}
}


static void map_stack(void)
{
	dune_procmap_iterate(map_stack_cb);
}

static int do_dune_enter(struct dune_percpu *percpu, void * arg)
{
	struct dune_config *conf;
	int ret;

	map_stack();

	conf = memalign(PGSIZE, sizeof(struct dune_config));

	if ((ret = dune_vm_map_pages_add_flags(pgroot, (void*)conf, PGSIZE, PERM_NOCOW))) {
		printf("dune: entry to Dune mode failed by vm_map, ret is %d\n", ret);
		return ret;
	}
	conf->vcpu = 0;
	conf->rip = (__u64)&__dune_ret;
	conf->rsp = 0;
	conf->cr3 = (physaddr_t)pgroot;
	conf->rflags = 0x2;

	/* NOTE: We don't setup the general purpose registers because __dune_ret
	 * will restore them as they were before the __dune_enter call */
	printf("dune_config at %lx with pgroot %lu, percpu %p\n", (uintptr_t)conf, conf->cr3, percpu);
	ret = __dune_enter(dune_fd, conf);
	if (ret) {
		printf("dune: entry to Dune mode failed, ret is %d\n", ret);
		return -EIO;
	}

	ret = dune_boot(percpu);
	if (ret) {
		printf("dune: problem while booting, unrecoverable\n");
		dune_die();
	}

	dune_apic_init_rt_entry();
	return 0;
}

// static void dune_dump_config(struct dune_config *conf)
// {
// 	printf("dune: --- Begin Config Dump of conf at %lx ---\n", (uintptr_t)conf);
// 	printf("dune: RET %lld STATUS %lld\n", conf->ret, conf->status);
// 	printf("dune: RFLAGS 0x%016llx CR3 0x%016llx RIP 0x%016llx\n", conf->rflags, conf->cr3, conf->rip);
// 	printf("dune: RAX 0x%016llx RCX 0x%016llx\n", conf->rax, conf->rcx);
// 	printf("dune: RDX 0x%016llx RBX 0x%016llx\n", conf->rdx, conf->rbx);
// 	printf("dune: RSP 0x%016llx RBP 0x%016llx\n", conf->rsp, conf->rbp);
// 	printf("dune: RSI 0x%016llx RDI 0x%016llx\n", conf->rsi, conf->rdi);
// 	printf("dune: R8  0x%016llx R9  0x%016llx\n", conf->r8, conf->r9);
// 	printf("dune: R10 0x%016llx R11 0x%016llx\n", conf->r10, conf->r11);
// 	printf("dune: R12 0x%016llx R13 0x%016llx\n", conf->r12, conf->r13);
// 	printf("dune: R14 0x%016llx R15 0x%016llx\n", conf->r14, conf->r15);
// 	printf("dune: --- End Config Dump ---\n");
// }

static __attribute__((unused)) void dune_dump_config(struct dune_config *conf)
{
	printf("dune: --- Begin Config Dump ---\n");
	printf("dune: RET %lld STATUS %lld\n", conf->ret, conf->status);
	printf("dune: RFLAGS 0x%016llx CR3 0x%016llx RIP 0x%016llx\n", conf->rflags, conf->cr3, conf->rip);
	printf("dune: RAX 0x%016llx RCX 0x%016llx\n", conf->rax, conf->rcx);
	printf("dune: RDX 0x%016llx RBX 0x%016llx\n", conf->rdx, conf->rbx);
	printf("dune: RSP 0x%016llx RBP 0x%016llx\n", conf->rsp, conf->rbp);
	printf("dune: RSI 0x%016llx RDI 0x%016llx\n", conf->rsi, conf->rdi);
	printf("dune: R8  0x%016llx R9  0x%016llx\n", conf->r8, conf->r9);
	printf("dune: R10 0x%016llx R11 0x%016llx\n", conf->r10, conf->r11);
	printf("dune: R12 0x%016llx R13 0x%016llx\n", conf->r12, conf->r13);
	printf("dune: R14 0x%016llx R15 0x%016llx\n", conf->r14, conf->r15);
	printf("dune: --- End Config Dump ---\n");
}
/**
 * on_dune_exit - handle Dune exits
 *
 * This function must not return. It can either exit(), __dune_go_dune() or
 * __dune_go_linux().
 */
void on_dune_exit(struct dune_config *conf)
{
	//dune_dump_config(conf);
	if (conf->ret != DUNE_RET_EXIT) {
		//printf("on_dune_exit: tid: %d, ret %lld\n", gettid(), conf->ret);
	}
	switch (conf->ret) {
	case DUNE_RET_EXIT:
		syscall(SYS_exit, conf->status);
	case DUNE_RET_EPT_VIOLATION:
		printf("dune: exit due to EPT violation\n");
		break;
	case DUNE_RET_INTERRUPT:
		dune_debug_handle_int(conf);
		printf("dune: exit due to interrupt %lld\n", conf->status);
		break;
	case DUNE_RET_SIGNAL:
		printf("dune: reenter dune due to signal %lld\n", conf->status);
		__dune_go_dune(dune_fd, conf);
		break;
	case DUNE_RET_UNHANDLED_VMEXIT:
		printf("dune: exit due to unhandled VM exit\n");
		//dune_procmap_dump();
		break;
	case DUNE_RET_NOENTER:
		printf("dune: re-entry to Dune mode failed, status is %lld\n",
			   conf->status);
		break;
	default:
		printf("dune: unknown exit from Dune, ret=%lld, status=%lld\n",
			   conf->ret, conf->status);
		break;
	}

	exit(EXIT_FAILURE);
}

/**
 * dune_enter - transitions a process to "Dune mode"
 *
 * Can only be called after dune_init().
 * 
 * Use this function in each forked child and/or each new thread
 * if you want to re-enter "Dune mode".
 * 
 * Returns 0 on success, otherwise failure.
 */
int dune_enter(void)
{
	struct dune_percpu *percpu;
	int ret;

	// check if this process already entered Dune before a fork...
	if (lpercpu)
		return do_dune_enter(lpercpu, NULL);

	percpu = create_percpu();
	if (!percpu)
		return -ENOMEM;

	ret = do_dune_enter(percpu, NULL);

	if (ret) {
		free_percpu(percpu);
		return ret;
	}

	lpercpu = percpu;
	return 0;
}

int dune_enter_with_2nd_pgtable(void* arg)
{
	struct dune_percpu *percpu;
	int ret;

	// check if this process already entered Dune before a fork...
	if (lpercpu)
		return do_dune_enter(lpercpu, arg);

	percpu = create_percpu();
	if (!percpu)
		return -ENOMEM;

	ret = do_dune_enter(percpu, arg);

	if (ret) {
		free_percpu(percpu);
		return ret;
	}

	lpercpu = percpu;
	return 0;
}

static void dune_default_syscall_handler(struct dune_tf *tf)
{
  	int syscall_num = (int) tf->rax;
	//int cpu_id = dune_get_cpu_id();
	//dune_printf("Default syscall handler, cpu_id %d got syscall number %d, ip %p\n", cpu_id, syscall_num, tf->rip);
    dune_passthrough_syscall(tf);
	//dune_printf("Default syscall handler, cpu_id %d got syscall number %d, done, ret %d\n", cpu_id, syscall_num, tf->rax);
}

int dune_cnt = 0;


static inline int prot_to_perm(int prot)
{
	int perm = PERM_U;

	if (prot & PROT_READ)
		perm |= PERM_R;
	if (prot & PROT_WRITE)
		perm |= PERM_W;
	if (prot & PROT_EXEC)
		perm |= PERM_X;

	return perm;
}
static void dune_default_g0_syscall_handler(struct dune_tf *tf)
{
  	int syscall_num = (int) tf->rax;
	int cpu_id = dune_get_cpu_id();
	int ret;
	if (syscall_num == SYS_mmap) {
		// for mapping region larger than 2MB, use huge pages
		struct dune_tf new_tf = *tf;
		int flags = (int)tf->r10;
		int prot = (int)tf->rdx;
		size_t len = (size_t)tf->rsi;
		// if (len >= 2 * 1024 * 1024 && (prot & PROT_EXEC) == 0) {
		// 	new_tf.r10 |= MAP_HUGETLB;
		// }
		dune_printf("got mmap(addr=%lx, length=%lx, prot=%lx, flags=%lx, fd=%lx, offset=%lx), hugetlb?%s\n", new_tf.rdi, new_tf.rsi, new_tf.rdx, new_tf.r10, new_tf.r8, new_tf.r9, (new_tf.r10 & MAP_HUGETLB) ? "yes" : "no");
		dune_passthrough_g0_syscall(&new_tf);
		dune_printf("mmap(addr=%lx, length=%lx, prot=%lx, flags=%lx, fd=%lx, offset=%lx), hugetlb?%s, return %lx\n", new_tf.rdi, new_tf.rsi, new_tf.rdx, new_tf.r10, new_tf.r8, new_tf.r9, (new_tf.r10 & MAP_HUGETLB) ? "yes" : "no",new_tf.rax);
		tf->rax = new_tf.rax;
		if (new_tf.rax != (unsigned long)MAP_FAILED) {
			// if (len >= 2 * 1024 * 1024 && (prot & PROT_EXEC) == 0) {
			// 	ret = dune_vm_map_phys(pgroot, new_tf.rax, len,
			// 		(void *)dune_va_to_pa((void *)new_tf.rax),
			// 		prot_to_perm(prot) | PERM_BIG);
			// 	if (ret) {
			// 		printf("dune: failed to map dune huge page\n");
			// 	} else {
			// 		printf("dune: mapped dune huge page\n");
			// 	}
			// }
		}
		
	} else {
		dune_passthrough_g0_syscall(tf);
	}
}

/**
 * dune_init - initializes libdune
 * 
 * @map_full: determines if the full process address space should be mapped
 * 
 * Call this function once before using libdune.
 *
 * Dune supports two memory modes. If map_full is true, then every possible
 * address in the process address space is mapped. Otherwise, only addresses
 * that are used (e.g. set up through mmap) are mapped. Full mapping consumes
 * a lot of memory when enabled, but disabling it incurs slight overhead
 * since pages will occasionally need to be faulted in.
 * 
 * Returns 0 on success, otherwise failure.
 */
int dune_init(bool map_full)
{
	int ret, i;

	BUILD_ASSERT(IOCTL_DUNE_ENTER == DUNE_ENTER);
	BUILD_ASSERT(DUNE_CFG_RET == offsetof(struct dune_config, ret));
	BUILD_ASSERT(DUNE_CFG_RAX == offsetof(struct dune_config, rax));
	BUILD_ASSERT(DUNE_CFG_RBX == offsetof(struct dune_config, rbx));
	BUILD_ASSERT(DUNE_CFG_RCX == offsetof(struct dune_config, rcx));
	BUILD_ASSERT(DUNE_CFG_RDX == offsetof(struct dune_config, rdx));
	BUILD_ASSERT(DUNE_CFG_RSI == offsetof(struct dune_config, rsi));
	BUILD_ASSERT(DUNE_CFG_RDI == offsetof(struct dune_config, rdi));
	BUILD_ASSERT(DUNE_CFG_RSP == offsetof(struct dune_config, rsp));
	BUILD_ASSERT(DUNE_CFG_RBP == offsetof(struct dune_config, rbp));
	BUILD_ASSERT(DUNE_CFG_R8 == offsetof(struct dune_config, r8));
	BUILD_ASSERT(DUNE_CFG_R9 == offsetof(struct dune_config, r9));
	BUILD_ASSERT(DUNE_CFG_R10 == offsetof(struct dune_config, r10));
	BUILD_ASSERT(DUNE_CFG_R11 == offsetof(struct dune_config, r11));
	BUILD_ASSERT(DUNE_CFG_R12 == offsetof(struct dune_config, r12));
	BUILD_ASSERT(DUNE_CFG_R13 == offsetof(struct dune_config, r13));
	BUILD_ASSERT(DUNE_CFG_R14 == offsetof(struct dune_config, r14));
	BUILD_ASSERT(DUNE_CFG_R15 == offsetof(struct dune_config, r15));
	BUILD_ASSERT(DUNE_CFG_RIP == offsetof(struct dune_config, rip));
	BUILD_ASSERT(DUNE_CFG_RFLAGS == offsetof(struct dune_config, rflags));
	BUILD_ASSERT(DUNE_CFG_CR3 == offsetof(struct dune_config, cr3));
	BUILD_ASSERT(DUNE_CFG_STATUS == offsetof(struct dune_config, status));
	BUILD_ASSERT(DUNE_CFG_VCPU == offsetof(struct dune_config, vcpu));

	dune_fd = open("/dev/dune", O_RDWR);
	if (dune_fd <= 0) {
		printf("dune: failed to open Dune device\n");
		ret = -errno;
		goto fail_open;
	}

	//pgroot = dune_alloc_page_internal();
	pgroot = memalign(PGSIZE, PGSIZE);
	if (!pgroot) {
		ret = -ENOMEM;
		goto fail_pgroot;
	}
	memset(pgroot, 0, PGSIZE);

	if ((ret = dune_page_init())) {
		printf("dune: unable to initialize page manager\n");
		goto err;
	}


	if ((ret = setup_mappings(map_full))) {
		printf("dune: unable to setup memory layout\n");
		goto err;
	}
	printf("dune: mapping %d finished\n", map_full);
	if ((ret = setup_syscall())) {
		printf("dune: unable to setup system calls\n");
		goto err;
	}

	if ((ret = setup_apic_mapping())) {
		printf("dune: unable to setup APIC\n");
		goto err;
	}

	if ((ret = setup_posted_intr_desc_mapping())) {
		printf("dune: unable to setup posted interrupt descriptors mappings\n");
		goto err;
	}

	if ((ret = setup_tlb_info_page_mapping())) {
		printf("dune: unable to setup tlb info page mappings\n");
		goto err;
	}

	// disable signals for now until we have better support
	for (i = 1; i < 32; i++) {
		struct sigaction sa;

		switch (i) {
		case SIGTSTP:
		case SIGSTOP:
		case SIGKILL:
		case SIGCHLD:
		case SIGINT:
		case SIGTERM:
			continue;
		}

		memset(&sa, 0, sizeof(sa));

		sa.sa_handler = SIG_IGN;

		if (sigaction(i, &sa, NULL) == -1)
			err(1, "sigaction() %d", i);
	}

	setup_idt();

	dune_setup_apic();

	ret = dune_vm_map_pages_add_flags(pgroot, dune_pmem_alloc_begin(), dune_pmem_alloc_end() - dune_pmem_alloc_begin(), PERM_NOCOW);
	if (ret != 0) {
		printf("dune: failed to disable cow for dune allocator memory\n");
		return ret;
	}


	ret = dune_vm_map_pages_add_flags(pgroot, PAGEBASE, PAGEBASE_END - PAGEBASE, PERM_NOCOW);
	if (ret != 0) {
		printf("dune: failed to disable cow for dune kernel memory\n");
		return ret;
	}

	dune_register_syscall_handler(dune_default_syscall_handler);
	dune_register_g0_syscall_handler(dune_default_g0_syscall_handler);
	printf("dune: initialization succeeded\n");
	return 0;

err:
	// FIXME: need to free memory
fail_pgroot:
	close(dune_fd);
fail_open:
	return ret;
}

int dune_init_with_ipi(bool map_full) {
	int ret;
	if ((ret = dune_init(map_full))) {
		return ret;
	}

	return dune_ipi_init();
}


int dbos_init(bool with_ipi)
{
	if (with_ipi) {
		return dune_init_with_ipi(true);
	} else {
		return dune_init(true);
	}
}

int dbos_enter() {
	return dune_enter();
}