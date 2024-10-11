/**
 * ept.c - Support for Intel's Extended Page Tables
 *
 * Authors:
 *   Adam Belay <abelay@stanford.edu>
 *
 * We support the EPT by making a sort of 'shadow' copy of the Linux
 * process page table. Mappings are created lazily as they are needed.
 * We keep the EPT synchronized with the process page table through
 * mmu_notifier callbacks.
 *
 * Some of the low-level EPT functions are based on KVM.
 * Original Authors:
 *   Avi Kivity   <avi@qumranet.com>
 *   Yaniv Kamay  <yaniv@qumranet.com>
 */

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <asm/pgtable.h>

#include "dune.h"
#include "vmx.h"
#include "compat.h"

#define EPT_LEVELS	   4 /* 0 through 3 */
#define HUGE_PAGE_SIZE 2097152

static inline bool cpu_has_vmx_ept_execute_only(void)
{
	return vmx_capability.ept & VMX_EPT_EXECUTE_ONLY_BIT;
}

static inline bool cpu_has_vmx_eptp_uncacheable(void)
{
	return vmx_capability.ept & VMX_EPTP_UC_BIT;
}

static inline bool cpu_has_vmx_eptp_writeback(void)
{
	return vmx_capability.ept & VMX_EPTP_WB_BIT;
}

static inline bool cpu_has_vmx_ept_2m_page(void)
{
	return vmx_capability.ept & VMX_EPT_2MB_PAGE_BIT;
}

static inline bool cpu_has_vmx_ept_1g_page(void)
{
	return vmx_capability.ept & VMX_EPT_1GB_PAGE_BIT;
}

static inline bool cpu_has_vmx_ept_4levels(void)
{
	return vmx_capability.ept & VMX_EPT_PAGE_WALK_4_BIT;
}

#define VMX_EPT_FAULT_READ	0x01
#define VMX_EPT_FAULT_WRITE 0x02
#define VMX_EPT_FAULT_INS	0x04

typedef unsigned long epte_t;

#define __EPTE_READ	   0x01
#define __EPTE_WRITE   0x02
#define __EPTE_EXEC	   0x04
#define __EPTE_IPAT	   0x40
#define __EPTE_SZ	   0x80
#define __EPTE_A	   0x100
#define __EPTE_D	   0x200
#define __EPTE_PFNMAP  0x400 /* ignored by HW */
#define __EPTE_TYPE(n) (((n)&0x7) << 3)

enum {
	EPTE_TYPE_UC = 0, /* uncachable */
	EPTE_TYPE_WC = 1, /* write combining */
	EPTE_TYPE_WT = 4, /* write through */
	EPTE_TYPE_WP = 5, /* write protected */
	EPTE_TYPE_WB = 6, /* write back */
};

#define __EPTE_NONE 0
#define __EPTE_FULL (__EPTE_READ | __EPTE_WRITE | __EPTE_EXEC)

#define EPTE_ADDR  (~(PAGE_SIZE - 1))
#define EPTE_FLAGS (PAGE_SIZE - 1)

static inline uintptr_t epte_addr(epte_t epte)
{
	return (epte & EPTE_ADDR);
}

static inline uintptr_t epte_page_vaddr(epte_t epte)
{
	return (uintptr_t)__va(epte_addr(epte));
}

static inline epte_t epte_flags(epte_t epte)
{
	return (epte & EPTE_FLAGS);
}

static inline int epte_present(epte_t epte)
{
	return (epte & __EPTE_FULL) > 0;
}

static inline int epte_big(epte_t epte)
{
	return (epte & __EPTE_SZ) > 0;
}

#define ADDR_INVAL ((unsigned long)-1)

static unsigned long hva_to_gpa(struct vmx_vcpu *vcpu, struct mm_struct *mm,
								unsigned long hva)
{
	uintptr_t mmap_start, stack_start;
	uintptr_t phys_end = (1ULL << boot_cpu_data.x86_phys_bits);
	uintptr_t gpa;

	BUG_ON(!mm);

	mmap_start = LG_ALIGN(mm->mmap_base) - GPA_MAP_SIZE;
	stack_start = LG_ALIGN(mm->start_stack) - GPA_STACK_SIZE;

	if (hva >= stack_start) {
		if (hva - stack_start >= GPA_STACK_SIZE)
			return ADDR_INVAL;
		gpa = hva - stack_start + phys_end - GPA_STACK_SIZE;
	} else if (hva >= mmap_start) {
		if (hva - mmap_start >= GPA_MAP_SIZE)
			return ADDR_INVAL;
		gpa = hva - mmap_start + phys_end - GPA_STACK_SIZE - GPA_MAP_SIZE;
	} else {
		if (hva >= phys_end - GPA_STACK_SIZE - GPA_MAP_SIZE)
			return ADDR_INVAL;
		gpa = hva;
	}

	return gpa;
}

static unsigned long gpa_to_hva(struct vmx_vcpu *vcpu, struct mm_struct *mm,
								unsigned long gpa)
{
	uintptr_t phys_end = (1ULL << boot_cpu_data.x86_phys_bits);

	/* fake success when translating APIC page */
	if ((gpa & PAGE_MASK) == GPA_APIC_PAGE) {
		return 0;
	}

	/* fake success when translating posted interrupt descriptors pages */
	if ((gpa & PAGE_MASK) >= GPA_POSTED_INTR_DESCS &&
	    (gpa & PAGE_MASK) <  GPA_POSTED_INTR_DESCS + (PAGE_SIZE * 256)) {
		return 0;
	}

	/* fake success when translating posted interrupt descriptors pages */
	if ((gpa & PAGE_MASK) >= GPA_PV_INFO_PAGES &&
	    (gpa & PAGE_MASK) <  GPA_PV_INFO_PAGES + (PAGE_SIZE * 256)) {
		return 0;
	}


	if (gpa < phys_end - GPA_STACK_SIZE - GPA_MAP_SIZE)
		return gpa;
	else if (gpa < phys_end - GPA_STACK_SIZE)
		return gpa - (phys_end - GPA_STACK_SIZE - GPA_MAP_SIZE) +
			   LG_ALIGN(mm->mmap_base) - GPA_MAP_SIZE;
	else if (gpa < phys_end)
		return gpa - (phys_end - GPA_STACK_SIZE) + LG_ALIGN(mm->start_stack) -
			   GPA_STACK_SIZE;
	else
		return ADDR_INVAL;
}

#define ADDR_TO_IDX(la, n)                                                     \
	((((unsigned long)(la)) >> (12 + 9 * (n))) & ((1 << 9) - 1))

static int ept_lookup_gpa(struct vmx_vcpu *vcpu, void *gpa, int level,
						  int create, epte_t **epte_out)
{
	int i;
	epte_t *dir = (epte_t *)__va(vcpu->vmx_instance->ept_root);

	for (i = EPT_LEVELS - 1; i > level; i--) {
		int idx = ADDR_TO_IDX(gpa, i);

		if (!epte_present(dir[idx])) {
			void *page;

			if (!create)
				return -ENOENT;

			page = (void *)__get_free_page(GFP_ATOMIC);
			if (!page)
				return -ENOMEM;
			vcpu->pgtbl_pages_created += 1;
			memset(page, 0, PAGE_SIZE);
			dir[idx] = epte_addr(virt_to_phys(page)) | __EPTE_FULL;
		}

		if (epte_big(dir[idx])) {
			if (i != 1 && i != 2)
				return -EINVAL;
			level = i;
			break;
		}

		dir = (epte_t *)epte_page_vaddr(dir[idx]);
	}

	*epte_out = &dir[ADDR_TO_IDX(gpa, level)];
	return 0;
}

static int ept_lookup(struct vmx_vcpu *vcpu, struct mm_struct *mm, void *hva,
					  int level, int create, epte_t **epte_out)
{
	void *gpa = (void *)hva_to_gpa(vcpu, mm, (unsigned long)hva);

	if (gpa == (void *)ADDR_INVAL) {
		printk(KERN_ERR "ept: hva %p is out of range\n", hva);
		printk(KERN_ERR "ept: mem_base %lx, stack_start %lx\n", mm->mmap_base,
			   mm->start_stack);
		return -EINVAL;
	}

	return ept_lookup_gpa(vcpu, gpa, level, create, epte_out);
}

static void free_ept_page(epte_t epte)
{
	struct page *page = pfn_to_page(epte_addr(epte) >> PAGE_SHIFT);

	/* PFN mapppings are not backed by pages. */
	if (epte & __EPTE_PFNMAP)
		return;

	if (epte & __EPTE_WRITE)
		set_page_dirty(page);
	put_page(page);
}

static void free_ept_page_lock(epte_t epte)
{
	struct page *page = pfn_to_page(epte_addr(epte) >> PAGE_SHIFT);

	/* PFN mapppings are not backed by pages. */
	if (epte & __EPTE_PFNMAP)
		return;

	if (epte & __EPTE_WRITE)
		set_page_dirty_lock(page);
	put_page(page);
}

static void vmx_free_ept(unsigned long ept_root)
{
	epte_t *pgd = (epte_t *)__va(ept_root);
	int i, j, k, l;

	for (i = 0; i < PTRS_PER_PGD; i++) {
		epte_t *pud = (epte_t *)epte_page_vaddr(pgd[i]);
		if (!epte_present(pgd[i]))
			continue;

		for (j = 0; j < PTRS_PER_PUD; j++) {
			epte_t *pmd = (epte_t *)epte_page_vaddr(pud[j]);
			if (!epte_present(pud[j]))
				continue;
			if (epte_flags(pud[j]) & __EPTE_SZ) {
				free_ept_page_lock(pud[j]);
				continue;
			}

			for (k = 0; k < PTRS_PER_PMD; k++) {
				epte_t *pte = (epte_t *)epte_page_vaddr(pmd[k]);
				if (!epte_present(pmd[k]))
					continue;
				if (epte_flags(pmd[k]) & __EPTE_SZ) {
					free_ept_page_lock(pmd[k]);
					continue;
				}

				for (l = 0; l < PTRS_PER_PTE; l++) {
					if (!epte_present(pte[l]))
						continue;

					free_ept_page_lock(pte[l]);
				}

				free_page((unsigned long)pte);
			}

			free_page((unsigned long)pmd);
		}

		free_page((unsigned long)pud);
	}

	free_page((unsigned long)pgd);
}

static int ept_clear_epte(epte_t *epte)
{
	if (*epte == __EPTE_NONE)
		return 0;

	free_ept_page(*epte);
	*epte = __EPTE_NONE;

	return 1;
}

static int ept_clear_l1_epte(epte_t *epte)
{
	int i;
	epte_t *pte = (epte_t *)epte_page_vaddr(*epte);

	if (*epte == __EPTE_NONE)
		return 0;

	for (i = 0; i < PTRS_PER_PTE; i++) {
		if (!epte_present(pte[i]))
			continue;

		free_ept_page(pte[i]);
	}

	free_page((uintptr_t)pte);
	*epte = __EPTE_NONE;

	return 1;
}

static int ept_clear_l2_epte(epte_t *epte)
{
	int i, j;
	epte_t *pmd = (epte_t *)epte_page_vaddr(*epte);

	if (*epte == __EPTE_NONE)
		return 0;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		epte_t *pte = (epte_t *)epte_page_vaddr(pmd[i]);
		if (!epte_present(pmd[i]))
			continue;
		if (epte_flags(pmd[i]) & __EPTE_SZ) {
			free_ept_page(pmd[i]);
			continue;
		}

		for (j = 0; j < PTRS_PER_PTE; j++) {
			if (!epte_present(pte[j]))
				continue;

			free_ept_page(pte[j]);
		}

		free_page((uintptr_t)pte);
	}

	free_page((uintptr_t)pmd);

	*epte = __EPTE_NONE;

	return 1;
}

static int ept_map_apic_page(struct vmx_vcpu *vcpu, int make_write, unsigned long gpa)
{
	int ret;
	epte_t *epte, flags;

	write_lock(&vcpu->vmx_instance->ept_lock);
	ret = ept_lookup_gpa(vcpu, (void *) gpa, 0, 1, &epte);
	if (ret) {
		write_unlock(&vcpu->vmx_instance->ept_lock);
		printk(KERN_ERR "ept: failed to lookup EPT entry\n");
		return ret;
	}

	flags = __EPTE_READ | __EPTE_TYPE(EPTE_TYPE_UC) |
		__EPTE_IPAT | __EPTE_PFNMAP;
	if (make_write)
		flags |= __EPTE_WRITE;
	if (vcpu->vmx_instance->ept_ad_enabled) {
		/* premark A/D to avoid extra memory references */
		flags |= __EPTE_A;
		if (make_write)
			flags |= __EPTE_D;
	}

	if (epte_present(*epte))
		ept_clear_epte(epte);

	*epte = epte_addr(APIC_DEFAULT_PHYS_BASE) | flags;
	write_unlock(&vcpu->vmx_instance->ept_lock);

	return 0;
}

static int ept_map_posted_intr_desc_page(struct vmx_vcpu *vcpu, int make_write, unsigned long gpa)
{
	int ret;
	epte_t *epte, flags;
	long addr;

	write_lock(&vcpu->vmx_instance->ept_lock);
	ret = ept_lookup_gpa(vcpu, (void *) gpa, 0, 1, &epte);
	if (ret) {
		write_unlock(&vcpu->vmx_instance->ept_lock);
		printk(KERN_ERR "ept: failed to lookup EPT entry\n");
		return ret;
	}

	flags = __EPTE_READ | __EPTE_WRITE | __EPTE_TYPE(EPTE_TYPE_WB);
	
	addr = (long)__pa(posted_interrupt_desc_region) + (gpa - GPA_POSTED_INTR_DESCS);
	*epte = epte_addr(addr) | flags;
	write_unlock(&vcpu->vmx_instance->ept_lock);

	return 0;
}

static int ept_map_tlb_info_page(struct vmx_vcpu *vcpu, int make_write, unsigned long gpa)
{
	int ret;
	epte_t *epte, flags;
	long addr;

	write_lock(&vcpu->vmx_instance->ept_lock);
	ret = ept_lookup_gpa(vcpu, (void *) gpa, 0, 1, &epte);
	if (ret) {
		write_unlock(&vcpu->vmx_instance->ept_lock);
		printk(KERN_ERR "ept: failed to lookup EPT entry\n");
		return ret;
	}

	flags = __EPTE_READ | __EPTE_WRITE | __EPTE_TYPE(EPTE_TYPE_WB);
	
	addr = (long)__pa(tlb_info_region) + (gpa - GPA_PV_INFO_PAGES);
	*epte = epte_addr(addr) | flags;
	write_unlock(&vcpu->vmx_instance->ept_lock);

	return 0;
}

static int hva_to_pfn_remapped(struct vm_area_struct *vma, unsigned long hva,
							   bool make_write, unsigned long *pfn)
{
	/*
	 * this code is adapted from KVM:
	 * get_user_pages fails for VM_IO and VM_PFNMAP vmas and does
	 * not call the fault handler, so do it here.
	 */
	pte_t *ptep;
	spinlock_t *ptl;
	int r;

	#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	r = follow_pte(vma->vm_mm, hva, &ptep, &ptl);
	#else
	r = follow_pte(vma, hva, &ptep, &ptl);
	#endif
	if (r) {
		bool unlocked = false;
		r = fixup_user_fault(current->mm, hva,
							 make_write ? FAULT_FLAG_WRITE : 0, &unlocked);
		if (unlocked) {
			return -EAGAIN;
		}
		if (r) {
			return r;
		}
		#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
		r = follow_pte(vma->vm_mm, hva, &ptep, &ptl);
		#else
		r = follow_pte(vma, hva, &ptep, &ptl);
		#endif

		
		if (r) {
			return r;
		}
	}

	pte_unmap_unlock(ptep, ptl);
	*pfn = pte_pfn(*ptep);
	return r;
}

static int ept_set_pfnmap_epte(struct vmx_vcpu *vcpu, int make_write,
							   unsigned long gpa, unsigned long hva)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	epte_t *epte, flags;
	unsigned long pfn;
	int ret;
	int cache_control;
	unsigned long vm_flags;
	unsigned long and_res = 0;
	unsigned long vm_io_or_pfnmap = 0;

	if ((gpa & PAGE_MASK) == GPA_APIC_PAGE) {
		//printk(KERN_INFO "vcpu %d mapped APIC_PAGE gpa %lx", vcpu->vpid, GPA_APIC_PAGE);
		return ept_map_apic_page(vcpu, make_write, gpa);
	}

	if ((gpa & PAGE_MASK) >= GPA_POSTED_INTR_DESCS &&
            (gpa & PAGE_MASK) <  GPA_POSTED_INTR_DESCS + (PAGE_SIZE * num_possible_cpus())) {
		//printk(KERN_INFO "vcpu %d mapped pi intr desc page gpa %lx", vcpu->vpid, GPA_APIC_PAGE);
		return ept_map_posted_intr_desc_page(vcpu, make_write, gpa);
	}

	if ((gpa & PAGE_MASK) >= GPA_PV_INFO_PAGES &&
            (gpa & PAGE_MASK) <  GPA_PV_INFO_PAGES + (PAGE_SIZE * num_possible_cpus())) {
		//printk(KERN_INFO "vcpu %d mapped pi intr desc page gpa %lx", vcpu->vpid, GPA_APIC_PAGE);
		return ept_map_tlb_info_page(vcpu, make_write, gpa);
	}

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, hva);
	if (!vma) {
		up_read(&mm->mmap_sem);
		printk(KERN_ERR "ept_set_pfnmap_epte: could not find vma for hva %lx\n", hva);
		return -EFAULT;
	}
	vm_flags = vma->vm_flags;
	vm_io_or_pfnmap = VM_IO | VM_PFNMAP;
	and_res = vm_flags & vm_io_or_pfnmap;
	if (!(vma->vm_flags & (VM_IO | VM_PFNMAP))) {
		up_read(&mm->mmap_sem);
		printk(KERN_ERR "ept_set_pfnmap_epte: %lx !(vma->vm_flags & (VM_IO | VM_PFNMAP)) %lx %lx \n", vm_flags, vm_io_or_pfnmap, and_res);
		return -EFAULT;
	}

	printk(KERN_ERR "ept_set_pfnmap_epte: %lx (vma->vm_flags & (VM_IO | VM_PFNMAP)), gpa %lx, hva %lx \n", vm_flags, gpa, hva);
	while ((ret = hva_to_pfn_remapped(vma, hva, make_write, &pfn)) == -EAGAIN)
		;
	//ret = follow_pfn(vma, hva, &pfn);
	if (ret) {
		up_read(&mm->mmap_sem);
		return ret;
	}

	if (pgprot2cachemode(vma->vm_page_prot) == _PAGE_CACHE_MODE_WB)
		cache_control = EPTE_TYPE_WB;
	else if (pgprot2cachemode(vma->vm_page_prot) == _PAGE_CACHE_MODE_WC)
		cache_control = EPTE_TYPE_WC;
	else
		cache_control = EPTE_TYPE_UC;

	up_read(&mm->mmap_sem);

	/* NOTE: pfn's can not be huge pages, which is quite a relief here */
	write_lock(&vcpu->vmx_instance->ept_lock);
	ret = ept_lookup_gpa(vcpu, (void *)gpa, 0, 1, &epte);
	if (ret) {
		write_unlock(&vcpu->vmx_instance->ept_lock);
		printk(KERN_ERR "ept: failed to lookup EPT entry\n");
		return ret;
	}

	flags =
		__EPTE_READ | __EPTE_TYPE(cache_control) | __EPTE_IPAT | __EPTE_PFNMAP;
	if (make_write)
		flags |= __EPTE_WRITE;
	if (vcpu->vmx_instance->ept_ad_enabled) {
		/* premark A/D to avoid extra memory references */
		flags |= __EPTE_A;
		if (make_write)
			flags |= __EPTE_D;
	}

	if (epte_present(*epte))
		ept_clear_epte(epte);

	*epte = epte_addr(pfn << PAGE_SHIFT) | flags;
	write_unlock(&vcpu->vmx_instance->ept_lock);

	return 0;
}

static int ept_set_epte(struct vmx_vcpu *vcpu, int make_write, unsigned long gva,
						unsigned long gpa, unsigned long hva, int fault_flags)
{
	int ret;
	epte_t *epte, flags;
	struct page *page;
	unsigned huge_shift;
	int level;
	unsigned long grip = 0;
	int was_present = 0;
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;

	if (!((gpa & PAGE_MASK) == GPA_APIC_PAGE || (gpa & PAGE_MASK) >= GPA_POSTED_INTR_DESCS &&
            (gpa & PAGE_MASK) <  GPA_POSTED_INTR_DESCS + (PAGE_SIZE * num_possible_cpus()) || (gpa & PAGE_MASK) >= GPA_PV_INFO_PAGES &&
            (gpa & PAGE_MASK) <  GPA_PV_INFO_PAGES + (PAGE_SIZE * num_possible_cpus()))) {
		down_read(&mm->mmap_sem);
		vma = find_vma(mm, hva);
		if (!vma) {
			up_read(&mm->mmap_sem);
			return -EFAULT;
		}
		if (vma_is_anonymous(vma) && !(vma->vm_flags & (VM_IO | VM_PFNMAP))) {
			make_write = 1;
		}
		up_read(&mm->mmap_sem);
	}

	// Pages will be faulted in here if not backed by any physical memory.
	ret = get_user_pages_fast(hva, 1, make_write, &page);
	if (ret != 1) {
		ret = ept_set_pfnmap_epte(vcpu, make_write, gpa, hva);
		if (ret)
			printk(KERN_ERR "ept: failed to get user page hva %lx gpa %lx, error %d\n", hva, gpa, -ret);
		return ret;
	}



	//write_lock(&vcpu->vmx_instance->ept_lock);

	huge_shift = compound_order(compound_head(page)) + PAGE_SHIFT;
	level = 0;
	if (huge_shift == 30)
		level = 2;
	else if (huge_shift == 21)
		level = 1;

	bool read_locked = true;
	read_lock(&vcpu->vmx_instance->ept_lock);
	ret = ept_lookup_gpa(vcpu, (void *)gpa, level, 0, &epte);
	if (ret == -ENOENT) {
		read_unlock(&vcpu->vmx_instance->ept_lock);
		read_locked = false;
		write_lock(&vcpu->vmx_instance->ept_lock);
		ret = ept_lookup_gpa(vcpu, (void *)gpa, level, 1, &epte);
	}

	if (ret) {
		if (read_locked)
			read_unlock(&vcpu->vmx_instance->ept_lock);
		else
			write_unlock(&vcpu->vmx_instance->ept_lock);
		put_page(page);
		printk(KERN_ERR "ept: failed to lookup EPT entry\n");
		return ret;
	}

	ept_pte_lock(vcpu->vmx_instance, (void *)gpa);
	if (epte_present(*epte)) {
		was_present = true;
		if (!epte_big(*epte) && level == 2)
			ept_clear_l2_epte(epte);
		else if (!epte_big(*epte) && level == 1)
			ept_clear_l1_epte(epte);
		else
			ept_clear_epte(epte);
	}

	flags = __EPTE_READ | __EPTE_EXEC | __EPTE_TYPE(EPTE_TYPE_WB) | __EPTE_IPAT;
	if (make_write)
		flags |= __EPTE_WRITE;
	if (vcpu->vmx_instance->ept_ad_enabled) {
		/* premark A/D to avoid extra memory references */
		flags |= __EPTE_A;
		if (make_write)
			flags |= __EPTE_D;
	}

	if (level) {
		struct page *tmp = page;
		page = compound_head(page);
		get_page(page);
		put_page(tmp);

		flags |= __EPTE_SZ;
		//vmx_get_cpu(vcpu);
		grip = vmcs_readl(GUEST_RIP);
		//vmx_put_cpu(vcpu);
		// printk(KERN_INFO "ept: vcpu %d, %lluth ept fault, make_write %d, GVA: 0x%lx, GPA: 0x%lx, HVA: 0x%lx, HPA: 0x%lx, GUEST_RIP %lx, was_present %d, fault_flags %lx, large page level %d\n", vcpu->vpid,  vcpu->pgflt_count,  make_write, gva, gpa,
		//  	 hva, page_to_phys(page), grip, was_present, fault_flags, level);
		vcpu->host_huge_pages_connected += 1;
	} else {
		vcpu->host_4k_pages_connected += 1;
	}

	*epte = epte_addr(page_to_phys(page)) | flags;
	
	ept_pte_unlock(vcpu->vmx_instance, (void *)gpa);
	if (read_locked)
		read_unlock(&vcpu->vmx_instance->ept_lock);
	else
		write_unlock(&vcpu->vmx_instance->ept_lock);

	// if (++vcpu->pgflt_count < 1000) {
	// 	printk(KERN_INFO "ept: %lluth ept fault, GVA: 0x%lx, GPA: 0x%lx, HVA: 0x%lx, HPA: 0x%llx\n", vcpu->pgflt_count, gva, gpa,
	// 		 hva, page_to_phys(page));
	// }

	return 0;
}

int vmx_do_ept_fault(struct vmx_vcpu *vcpu, unsigned long gpa,
					 unsigned long gva, int fault_flags)
{
	int ret;
	unsigned long hva = gpa_to_hva(vcpu, current->mm, gpa);
	int make_write = (fault_flags & VMX_EPT_FAULT_WRITE) ? 1 : 0;

	if (unlikely(hva == ADDR_INVAL)) {
		printk(KERN_ERR "ept: gpa 0x%lx is out of range\n", gpa);
		return -EINVAL;
	}

	pr_debug("ept: GPA: 0x%lx, GVA: 0x%lx, HVA: 0x%lx, flags: %x\n", gpa, gva,
			 hva, fault_flags);

	// TODO: do we need to check if the user has write permissions to this page
	// before mapping it into the EPT? Investigate the security of this.
	ret = ept_set_epte(vcpu, make_write, gva, gpa, hva, fault_flags);

	return ret;
}

/**
 * ept_invalidate_page - removes a page from the EPT
 * @vcpu: the vcpu
 * @mm: the process's mm_struct
 * @addr: the address of the page
 *
 * Returns 1 if the page was removed, 0 otherwise
 */
static int ept_invalidate_page(struct vmx_vcpu *vcpu, struct mm_struct *mm,
							   unsigned long addr)
{
	int ret;
	epte_t *epte;
	void *gpa = (void *)hva_to_gpa(vcpu, mm, (unsigned long)addr);

	if (gpa == (void *)ADDR_INVAL) {
		printk(KERN_ERR "ept: hva %lx is out of range\n", addr);
		return 0;
	}

	read_lock(&vcpu->vmx_instance->ept_lock);
	ept_pte_lock(vcpu->vmx_instance, gpa);
	ret = ept_lookup_gpa(vcpu, (void *)gpa, 0, 0, &epte);
	if (ret) {
		ept_pte_unlock(vcpu->vmx_instance, gpa);
		read_unlock(&vcpu->vmx_instance->ept_lock);
		return 0;
	}

	ret = ept_clear_epte(epte);
	ept_pte_unlock(vcpu->vmx_instance, gpa);
	read_unlock(&vcpu->vmx_instance->ept_lock);

	if (ret) {
		cpumask_t mask;
		memcpy(&mask, &vcpu->vmx_instance->invalidate_mask, sizeof(mask));
		vmx_ept_batch_sync_individual_addr(vcpu->vmx_instance->eptp, &mask, (gpa_t)gpa);
		//vmx_ept_sync_individual_addr(vcpu, (gpa_t)gpa);
	}
		//vmx_ept_sync_individual_addr(vcpu, (gpa_t)gpa);

	return ret;
}

/**
 * ept_check_page_mapped - determines if a page is mapped in the ept
 * @vcpu: the vcpu
 * @mm: the process's mm_struct
 * @addr: the address of the page
 *
 * Returns 1 if the page is mapped, 0 otherwise
 */
static int ept_check_page_mapped(struct vmx_vcpu *vcpu, struct mm_struct *mm,
								 unsigned long addr)
{
	int ret;
	epte_t *epte;
	void *gpa = (void *)hva_to_gpa(vcpu, mm, (unsigned long)addr);

	if (gpa == (void *)ADDR_INVAL) {
		printk(KERN_ERR "ept: hva %lx is out of range\n", addr);
		return 0;
	}

	read_lock(&vcpu->vmx_instance->ept_lock);
	ret = ept_lookup_gpa(vcpu, (void *)gpa, 0, 0, &epte);
	read_unlock(&vcpu->vmx_instance->ept_lock);

	return !ret;
}

/**
 * ept_check_page_accessed - determines if a page was accessed using AD bits
 * @vcpu: the vcpu
 * @mm: the process's mm_struct
 * @addr: the address of the page
 * @flush: if true, clear the A bit
 *
 * Returns 1 if the page was accessed, 0 otherwise
 */
static int ept_check_page_accessed(struct vmx_vcpu *vcpu, struct mm_struct *mm,
								   unsigned long addr, bool flush)
{
	int ret, accessed;
	epte_t *epte;
	void *gpa = (void *)hva_to_gpa(vcpu, mm, (unsigned long)addr);

	if (gpa == (void *)ADDR_INVAL) {
		printk(KERN_ERR "ept: hva %lx is out of range\n", addr);
		return 0;
	}

	read_lock(&vcpu->vmx_instance->ept_lock);
	ret = ept_lookup_gpa(vcpu, (void *)gpa, 0, 0, &epte);
	ept_pte_lock(vcpu->vmx_instance, gpa);
	if (ret) {
		ept_pte_unlock(vcpu->vmx_instance, gpa);
		read_unlock(&vcpu->vmx_instance->ept_lock);
		return 0;
	}

	accessed = (*epte & __EPTE_A);
	if (flush & accessed)
		*epte = (*epte & ~__EPTE_A);
	ept_pte_unlock(vcpu->vmx_instance, gpa);
	read_unlock(&vcpu->vmx_instance->ept_lock);

	if (flush & accessed) {
		cpumask_t mask;
		memcpy(&mask, &vcpu->vmx_instance->invalidate_mask, sizeof(mask));
		vmx_ept_batch_sync_individual_addr(vcpu->vmx_instance->eptp, &mask, (gpa_t)gpa);
		// vmx_ept_sync_individual_addr(vcpu, (gpa_t)gpa);
	}


	return accessed;
}

static inline struct vmx_vcpu *mmu_notifier_to_vmx(struct mmu_notifier *mn)
{
	return container_of(mn, struct vmx_vcpu, mmu_notifier);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
static void ept_mmu_notifier_invalidate_page(struct mmu_notifier *mn,
											 struct mm_struct *mm,
											 unsigned long address)
{
	struct vmx_vcpu *vcpu = mmu_notifier_to_vmx(mn);

	pr_debug("ept: invalidate_page addr %lx\n", address);

	ept_invalidate_page(vcpu, mm, address);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static int
ept_mmu_notifier_invalidate_range_start(struct mmu_notifier *mn,
										const struct mmu_notifier_range *range)
{
	struct mm_struct *mm = range->mm;
	unsigned long start = range->start;
	unsigned long end = range->end;
#else
static void ept_mmu_notifier_invalidate_range_start(struct mmu_notifier *mn,
													struct mm_struct *mm,
													unsigned long start,
													unsigned long end)
{
#endif
	struct vmx_vcpu *vcpu = mmu_notifier_to_vmx(mn);
	int ret;
	epte_t *epte;
	unsigned long pos = start;
	bool sync_needed = false;

	pr_debug("ept: invalidate_range_start start %lx end %lx\n", start, end);

	unsigned long gpa = 0;
	read_lock(&vcpu->vmx_instance->ept_lock);
	int count = 0;
	while (pos < end) {
		ret = ept_lookup(vcpu, mm, (void *)pos, 0, 0, &epte);
		if (!ret) {
			gpa = (void *)hva_to_gpa(vcpu, mm, pos);
			ept_pte_lock(vcpu->vmx_instance, gpa);
			pos += epte_big(*epte) ? HUGE_PAGE_SIZE : PAGE_SIZE;
			sync_needed = *epte == __EPTE_NONE ? false : true;
			ept_clear_epte(epte);
			ept_pte_unlock(vcpu->vmx_instance, gpa);
		} else
			pos += PAGE_SIZE;
		count++;
	}
	read_unlock(&vcpu->vmx_instance->ept_lock);

	if (sync_needed) {
		cpumask_t mask;
		memcpy(&mask, &vcpu->vmx_instance->invalidate_mask, sizeof(mask));
		if (count == 1) {
			if (gpa != 0) {
				vmx_ept_batch_sync_individual_addr(vcpu->vmx_instance->eptp, &mask, (gpa_t)gpa);
			}
		} else {
			vmx_ept_batch_sync(vcpu->vmx_instance->eptp, &mask);
			// vmx_ept_sync_vcpu(vcpu);
		}
		// vmx_ept_sync_vcpu(vcpu);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	return 0;
#endif
}

static void
ept_mmu_notifier_invalidate_range_end(struct mmu_notifier *mn,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
									  const struct mmu_notifier_range *range)
#else
									  struct mm_struct *mm, unsigned long start,
									  unsigned long end)
#endif
{
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static void ept_mmu_notifier_change_pte(struct mmu_notifier *mn,
										struct mm_struct *mm,
										unsigned long address, pte_t pte)
{
	struct vmx_vcpu *vcpu = mmu_notifier_to_vmx(mn);

	pr_debug("ept: change_pte addr %lx flags %lx\n", address, pte_flags(pte));

	/*
	 * NOTE: Recent linux kernels (seen on 3.7 at least) hold a lock
	 * while calling this notifier, making it impossible to call
	 * get_user_pages_fast(). As a result, we just invalidate the
	 * page so that the mapping can be recreated later during a fault.
	 */
	ept_invalidate_page(vcpu, mm, address);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)
static int ept_mmu_notifier_clear_flush_young(struct mmu_notifier *mn,
											  struct mm_struct *mm,
											  unsigned long start,
											  unsigned long end)
{
	int ret = 0;
	struct vmx_vcpu *vcpu = mmu_notifier_to_vmx(mn);

	pr_debug("ept: clear_flush_young start %lx end %lx\n", start, end);

	if (!vcpu->vmx_instance->ept_ad_enabled) {
		for (; start < end; start += PAGE_SIZE)
			ret |= ept_invalidate_page(vcpu, mm, start);
	} else {
		for (; start < end; start += PAGE_SIZE)
			ret |= ept_check_page_accessed(vcpu, mm, start, true);
	}

	return ret;
}
#else
static int ept_mmu_notifier_clear_flush_young(struct mmu_notifier *mn,
											  struct mm_struct *mm,
											  unsigned long address)
{
	struct vmx_vcpu *vcpu = mmu_notifier_to_vmx(mn);

	pr_debug("ept: clear_flush_young addr %lx\n", address);

	if (!vcpu->vmx_instance->ept_ad_enabled)
		return ept_invalidate_page(vcpu, mm, address);
	else
		return ept_check_page_accessed(vcpu, mm, address, true);
}
#endif

static int ept_mmu_notifier_test_young(struct mmu_notifier *mn,
									   struct mm_struct *mm,
									   unsigned long address)
{
	struct vmx_vcpu *vcpu = mmu_notifier_to_vmx(mn);

	pr_debug("ept: test_young addr %lx\n", address);

	if (!vcpu->vmx_instance->ept_ad_enabled)
		return ept_check_page_mapped(vcpu, mm, address);
	else
		return ept_check_page_accessed(vcpu, mm, address, false);
}

static void ept_mmu_notifier_release(struct mmu_notifier *mn,
									 struct mm_struct *mm)
{
}

static const struct mmu_notifier_ops ept_mmu_notifier_ops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
	.invalidate_page = ept_mmu_notifier_invalidate_page,
#endif
	.invalidate_range_start = ept_mmu_notifier_invalidate_range_start,
	.invalidate_range_end = ept_mmu_notifier_invalidate_range_end,
	.clear_flush_young = ept_mmu_notifier_clear_flush_young,
	.test_young = ept_mmu_notifier_test_young,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	.change_pte = ept_mmu_notifier_change_pte,
#endif
	.release = ept_mmu_notifier_release,
};

int vmx_init_ept(struct vmx_common *vmx_instance)
{
	void *page = (void *)__get_free_page(GFP_KERNEL);

	if (!page)
		return -ENOMEM;

	memset(page, 0, PAGE_SIZE);
	vmx_instance->ept_root = __pa(page);

	return 0;
}

int vmx_create_ept(struct vmx_vcpu *vcpu)
{
	int ret;

	vcpu->mmu_notifier.ops = &ept_mmu_notifier_ops;
	ret = mmu_notifier_register(&vcpu->mmu_notifier, current->mm);
	if (ret)
		goto fail;

	return 0;

fail:
	// vmx_free_ept(vmx_instance->ept_root);

	return ret;
}

void vmx_destroy_ept(struct vmx_common *vmx_instance)
{
	vmx_free_ept(vmx_instance->ept_root);
}
