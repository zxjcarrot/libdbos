/*
 * vm.c - Virtual memory management routines
 */

#include <malloc.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "dune.h"

#define PDADDR(n, i)  (((unsigned long)(i)) << PDSHIFT(n))
#define PTE_DEF_FLAGS (PTE_P | PTE_W | PTE_U)
#define LGPGSIZE	  (1 << (PGSHIFT + NPTBITS))

static inline int pte_present(ptent_t pte)
{
	return (PTE_FLAGS(pte) & PTE_P);
}

static inline int pte_big(ptent_t pte)
{
	return (PTE_FLAGS(pte) & PTE_PS);
}

static inline void *alloc_page(void)
{
	struct page *pg = dune_page_alloc();
	if (!pg)
		return NULL;

	return (void *)dune_page2pa(pg);
}

void *dune_alloc_page_internal(void) {
	return alloc_page();
}

static inline void put_page(void *page)
{
	// XXX: Using PA == VA
	struct page *pg = dune_pa2page((physaddr_t)page);

	dune_page_put(pg);
}

int __dune_vm_page_walk(ptent_t *dir, void *start_va, void *end_va,
						page_walk_cb cb, const void *arg, int level, int create)
{
	// XXX: Using PA == VA
	int i, ret;
	int start_idx = PDX(level, start_va);
	int end_idx = PDX(level, end_va);
	void *base_va =
		(void *)((unsigned long)start_va & ~(PDADDR(level + 1, 1) - 1));

	assert(level >= 0 && level <= NPTLVLS);
	assert(end_idx < NPTENTRIES);

	for (i = start_idx; i <= end_idx; i++) {
		void *n_start_va, *n_end_va;
		void *cur_va = base_va + PDADDR(level, i);
		ptent_t *pte = &dir[i];
		

		if (level == 0) {
			if (create == CREATE_NORMAL || *pte) {
				ret = cb(arg, pte, cur_va, level);
				if (ret)
					return ret;
			}
			continue;
		}

		if (level == 1) {
			if (create == CREATE_BIG || pte_big(*pte)) {
				ret = cb(arg, pte, cur_va, level);
				if (ret)
					return ret;
				continue;
			}
		}

		if (level == 2) {
			if (create == CREATE_BIG_1GB || pte_big(*pte)) {
				ret = cb(arg, pte, cur_va, level);
				if (ret)
					return ret;
				continue;
			}
		}

		if (!pte_present(*pte)) {
			ptent_t *new_pte;

			if (!create)
				continue;

			new_pte = alloc_page();
			if (!new_pte)
				return -ENOMEM;
			memset(new_pte, 0, PGSIZE);
			*pte = PTE_ADDR(new_pte) | PTE_DEF_FLAGS;
		}

		n_start_va = (i == start_idx) ? start_va : cur_va;
		n_end_va = (i == end_idx) ? end_va : cur_va + PDADDR(level, 1) - 1;

		ret = __dune_vm_page_walk((ptent_t *)PTE_ADDR(dir[i]), n_start_va,
								  n_end_va, cb, arg, level - 1, create);
		if (ret)
			return ret;
	}

	return 0;
}


int __dune_vm_page_walk_batch(ptent_t *dir, void *start_va, void *end_va,
						page_walk_cb cb, page_walk_last_level_page_cb cb2, const void *arg, int level, int create)
{
	// XXX: Using PA == VA
	int i, ret;
	int start_idx = PDX(level, start_va);
	int end_idx = PDX(level, end_va);
	void *base_va =
		(void *)((unsigned long)start_va & ~(PDADDR(level + 1, 1) - 1));

	assert(level >= 0 && level <= NPTLVLS);
	assert(end_idx < NPTENTRIES);
	if (level == 0 && start_idx == 0 && end_idx - start_idx + 1 == 512) {
		//printf("base_va %p\n", base_va);
		return cb2(arg, dir, base_va + PDADDR(level, 0), 0);
	}
	for (i = start_idx; i <= end_idx; i++) {
		void *n_start_va, *n_end_va;
		void *cur_va = base_va + PDADDR(level, i);
		ptent_t *pte = &dir[i];

		if (level == 0) {
			if (create == CREATE_NORMAL || *pte) {
				ret = cb(arg, pte, cur_va, level);
				if (ret)
					return ret;
			}
			continue;
		}

		if (level == 1) {
			if (create == CREATE_BIG || pte_big(*pte)) {
				ret = cb(arg, pte, cur_va, level);
				if (ret)
					return ret;
				continue;
			}
		}

		if (level == 2) {
			if (create == CREATE_BIG_1GB || pte_big(*pte)) {
				ret = cb(arg, pte, cur_va, level);
				if (ret)
					return ret;
				continue;
			}
		}

		if (!pte_present(*pte)) {
			ptent_t *new_pte;

			if (!create)
				continue;

			new_pte = alloc_page();
			if (!new_pte)
				return -ENOMEM;
			memset(new_pte, 0, PGSIZE);
			*pte = PTE_ADDR(new_pte) | PTE_DEF_FLAGS;
		}

		n_start_va = (i == start_idx) ? start_va : cur_va;
		n_end_va = (i == end_idx) ? end_va : cur_va + PDADDR(level, 1) - 1;

		ret = __dune_vm_page_walk_batch((ptent_t *)PTE_ADDR(dir[i]), n_start_va,
								  n_end_va, cb, cb2, arg, level - 1, create);
		if (ret)
			return ret;
	}

	return 0;
}

int dune_vm_page_walk(ptent_t *root, void *start_va, void *end_va,
					  page_walk_cb cb, const void *arg)
{
	return __dune_vm_page_walk(root, start_va, end_va, cb, arg, 3, CREATE_NONE);
}

extern int dune_vm_page_walk_batch(ptent_t *dir, void *start_va, void *end_va,
						page_walk_cb cb, page_walk_last_level_page_cb cb2, const void *arg, int level) {
	return __dune_vm_page_walk_batch(dir, start_va, end_va, cb, cb2, arg, 3, CREATE_NONE);
}

int dune_vm_page_walk_fill(ptent_t *root, void *start_va, void *end_va,
							 page_walk_cb cb, const void * arg, int create) {
	return __dune_vm_page_walk(root, start_va, end_va, cb, arg, 3, create);
}
static int dune_vm_lookup_internal(ptent_t *root, void *va, int create, bool * created,
						  ptent_t **pte_out) {
	// XXX: Using PA == VA
	int i, j, k, l;
	ptent_t *pml4 = root, *pdpte, *pde, *pte;

	i = PDX(3, va);
	j = PDX(2, va);
	k = PDX(1, va);
	l = PDX(0, va);

	if (!pte_present(pml4[i])) {
		if (!create)
			return -ENOENT;
		//dune_printf("alloc_page\n");
		pdpte = alloc_page();
		memset(pdpte, 0, PGSIZE);
		*created = true;
		pml4[i] = PTE_ADDR(pdpte) | PTE_DEF_FLAGS;
	} else
		pdpte = (ptent_t *)PTE_ADDR(pml4[i]);

	if (!pte_present(pdpte[j])) {
		if (!create)
			return -ENOENT;
		//dune_printf("alloc_page\n");
		pde = alloc_page();
		memset(pde, 0, PGSIZE);
		*created = true;
		pdpte[j] = PTE_ADDR(pde) | PTE_DEF_FLAGS;
	} else if (pte_big(pdpte[j])) {
		*pte_out = &pdpte[j];
		return 0;
	} else
		pde = (ptent_t *)PTE_ADDR(pdpte[j]);

	if (!pte_present(pde[k])) {
		if (!create)
			return -ENOENT;
		//dune_printf("alloc_page\n");
		pte = alloc_page();
		memset(pte, 0, PGSIZE);
		*created = true;
		pde[k] = PTE_ADDR(pte) | PTE_DEF_FLAGS;
	} else if (pte_big(pde[k])) {
		*pte_out = &pde[k];
		return 0;
	} else
		pte = (ptent_t *)PTE_ADDR(pde[k]);

	*pte_out = &pte[l];
	return 0;
}

int dune_vm_lookup(ptent_t *root, void *va, int create, ptent_t **pte_out)
{
	bool created = false;
	return dune_vm_lookup_internal(root, va, create, &created, pte_out);
}
int dune_vm_lookup2(ptent_t *root, void *va, int create, bool * created, ptent_t **pte_out)
{
	return dune_vm_lookup_internal(root, va, create, created, pte_out);
}

static inline ptent_t get_pte_perm(int perm)
{
	ptent_t pte_perm = 0;

	if (perm & PERM_R)
		pte_perm = PTE_P;
	if (perm & PERM_W)
		pte_perm |= PTE_W;
	if (!(perm & PERM_X))
		pte_perm |= PTE_NX;
	if (perm & PERM_U)
		pte_perm |= PTE_U;
	if (perm & PERM_UC)
		pte_perm |= PTE_PCD;
	if (perm & PERM_COW)
		pte_perm |= PTE_COW;
	if (perm & PERM_USR1)
		pte_perm |= PTE_USR1;
	if (perm & PERM_USR2)
		pte_perm |= PTE_USR2;
	if (perm & PERM_USR3)
		pte_perm |= PTE_USR3;
	if (perm & PERM_USR4)
		pte_perm |= PTE_STACK;
	if (perm & PERM_BIG || perm & PERM_BIG_1GB)
		pte_perm |= PTE_PS;

	return pte_perm;
}

static int __dune_vm_mprotect_helper(const void *arg, ptent_t *pte, void *va, int level)
{
	ptent_t perm = (ptent_t)arg;

	//	if (!(PTE_FLAGS(*pte) & PTE_P))
	//		return -ENOMEM;

	*pte = PTE_ADDR(*pte) | (PTE_FLAGS(*pte) & PTE_PS) | perm;
	return 0;
}

int dune_vm_mprotect(ptent_t *root, void *va, size_t len, int perm)
{
	int ret;
	ptent_t pte_perm;

	if (!(perm & PERM_R)) {
		if (perm & PERM_W)
			return -EINVAL;
		perm = PERM_NONE;
	}

	pte_perm = get_pte_perm(perm);

	ret =
		__dune_vm_page_walk(root, va, va + len - 1, &__dune_vm_mprotect_helper,
							(void *)pte_perm, 3, CREATE_NONE);
	if (ret)
		return ret;

	dune_flush_tlb();

	return 0;
}

struct map_phys_data {
	ptent_t perm;
	unsigned long va_base;
	unsigned long pa_base;
	uint64_t pte_count;
};

static int __dune_vm_map_phys_helper(const void *arg, ptent_t *pte, void *va, int level)
{
	struct map_phys_data *data = (struct map_phys_data *)arg;

	*pte = PTE_ADDR(va - data->va_base + data->pa_base) | data->perm;
	data->pte_count++;
	return 0;
}

int dune_vm_map_phys(ptent_t *root, void *va, size_t len, void *pa, int perm)
{
	int ret;
	struct map_phys_data data;
	int create;

	//	if (!(perm & PERM_R) && (perm & ~(PERM_R)))
	//		return -EINVAL;

	data.perm = get_pte_perm(perm);
	data.va_base = (unsigned long)va;
	data.pa_base = (unsigned long)pa;
	data.pte_count = 0;
	if (perm & PERM_BIG)
		create = CREATE_BIG;
	else if (perm & PERM_BIG_1GB)
		create = CREATE_BIG_1GB;
	else
		create = CREATE_NORMAL;

	ret =
		__dune_vm_page_walk(root, va, va + len - 1, &__dune_vm_map_phys_helper,
							(void *)&data, 3, create);
	if (ret)
		return ret;

	//printf("dune_vm_map_phys: virtual [%lx-%lx] to physical [%lx-%lx] %lu KiB, ptes created %lu\n", (uintptr_t)va, (uintptr_t)((char*)va + len), (uintptr_t)pa, (uintptr_t)((char*)pa + len), len / 1024, data.pte_count);
	return 0;
}

int dune_vm_map_phys_with_pte_flags(ptent_t *root, void *va, size_t len, void *pa, int pte_flags, int perm)
{
	int ret;
	struct map_phys_data data;
	int create;

	data.perm = pte_flags;
	data.va_base = (unsigned long)va;
	data.pa_base = (unsigned long)pa;
	data.pte_count = 0;
	if (perm & PERM_BIG)
		create = CREATE_BIG;
	else if (perm & PERM_BIG_1GB)
		create = CREATE_BIG_1GB;
	else
		create = CREATE_NORMAL;

	ret =
		__dune_vm_page_walk(root, va, va + len - 1, &__dune_vm_map_phys_helper,
							(void *)&data, 3, create);
	if (ret)
		return ret;

	return 0;
}

static int __dune_vm_map_pages_helper(const void *arg, ptent_t *pte, void *va, int level)
{
	ptent_t perm = (ptent_t)arg;
	struct page *pg = dune_page_alloc();

	if (!pg)
		return -ENOMEM;

	*pte = PTE_ADDR(dune_page2pa(pg)) | perm;

	return 0;
}

static int __dune_vm_map_pages_add_flags_helper(const void *arg, ptent_t *pte, void *va, int level)
{
	ptent_t flags = (ptent_t)arg;
	struct page *pg = dune_page_alloc();

	if (!pg)
		return -ENOMEM;

	*pte |= flags;

	return 0;
}


int dune_vm_map_pages_add_flags(ptent_t *root, void *va, size_t len, int perm)
{
	int ret;
	ptent_t pte_perm;

	pte_perm = get_pte_perm(perm);

	ret =
		__dune_vm_page_walk(root, va, va + len - 1, &__dune_vm_map_pages_add_flags_helper,
							(void *)pte_perm, 3, CREATE_NONE);

	return ret;
}

int dune_vm_map_pages(ptent_t *root, void *va, size_t len, int perm)
{
	int ret;
	ptent_t pte_perm;

	if (!(perm & PERM_R) && (perm & ~(PERM_R)))
		return -EINVAL;

	pte_perm = get_pte_perm(perm);

	ret =
		__dune_vm_page_walk(root, va, va + len - 1, &__dune_vm_map_pages_helper,
							(void *)pte_perm, 3, CREATE_NORMAL);

	return ret;
}


uint64_t pgsize(int level) {
	if (level == 0) return PGSIZE;
	else if (level == 1) return PGSIZE * 512UL;
	else if (level == 2) return PGSIZE * 512UL * 512UL;
	assert(false);

	return -1;
}
int pteflags_to_perm(uint64_t pte_flags, int level)
{
	int perm = 0;

	if (pte_flags & PTE_P)
		perm |= PERM_R;
	if (pte_flags & PTE_W)
		perm |= PERM_W;
	if (!(pte_flags & PTE_NX))
		perm |= PERM_X;
	if (pte_flags & PTE_U)
		perm |= PERM_U;
	if (pte_flags & PTE_PCD)
		perm |= PERM_UC;
	if (pte_flags & PTE_COW)
		perm |= PERM_COW;
	if (pte_flags & PTE_USR1)
		perm |= PERM_USR1;
	if (pte_flags & PTE_USR2)
		perm |= PERM_USR2;
	if (pte_flags & PTE_USR3)
		perm |= PERM_USR3;
	if (pte_flags & PTE_STACK)
		perm |= PERM_USR4;
	if ((pte_flags & PTE_PS) && level == 1)
		perm |= PERM_BIG;
	if ((pte_flags & PTE_PS) && level == 2)
		perm |= PERM_BIG_1GB;

	return perm;
}



static int __dune_vm_clone_helper(const void *arg, ptent_t *pte, void *va, int level)
{
	int ret;
	struct page *pg = dune_pa2page(PTE_ADDR(*pte));
	ptent_t *newRoot = (ptent_t *)arg;
	ptent_t *newPte;

	int create_perm = 0;
	if (level == 0) {
		assert(!pte_big(*pte));
		create_perm = PERM_NONE;
	} else if (level == 1 ) {
		create_perm = PERM_BIG;
		assert(pte_big(*pte));
	} else {
		assert(level == 2);
		assert(pte_big(*pte));
		create_perm = PERM_BIG_1GB;
	}
	ret = dune_vm_map_phys_with_pte_flags(newRoot, va, pgsize(level), PTE_ADDR(*pte), PTE_FLAGS(*pte), create_perm);
	
	if (ret != 0) {
		return ret;
	}

	if (dune_page_isfrompool(PTE_ADDR(*pte)))
		dune_page_get(pg);

	return ret;
}

/**
 * Clone a page root.
 */
ptent_t *dune_vm_clone(ptent_t *root)
{
	int ret;
	ptent_t *newRoot;

	newRoot = alloc_page();
	memset(newRoot, 0, PGSIZE);

	ret = __dune_vm_page_walk(root, VA_START, VA_END, &__dune_vm_clone_helper,
							  newRoot, 3, CREATE_NONE);
	if (ret < 0) {
		dune_vm_free(newRoot);
		return NULL;
	}

	return newRoot;
}

struct fast_clone_args{
	ptent_t *newRoot;
	int pages_copied;
};

static int __dune_vm_clone_last_level_page_helper(const void *arg, ptent_t *dir, void *start_va, int level)
{
	assert(level == 0);
	int ret;
	ptent_t *newRoot = ((struct fast_clone_args *)arg)->newRoot;
	ptent_t *newDir = NULL;
	bool created = false;
	ret = dune_vm_lookup_internal(newRoot, start_va, CREATE_NORMAL, &created, &newDir);

	// memcpy(newDir, dir, PGSIZE);
	for (int i = 0; i < 512; ++i) {
		newDir[i] = dir[i];
		newDir[i] |= (PTE_COW);
		newDir[i] &= ~PTE_W;
	}
	assert(ret == 0);
	((struct fast_clone_args *)arg)->pages_copied++;

	return ret;
}

/**
 * Clone a page root.
 */
ptent_t *dune_vm_clone_fast(ptent_t *root)
{
	int ret;
	struct fast_clone_args args;

	ptent_t *newRoot;

	newRoot = alloc_page();
	memset(newRoot, 0, PGSIZE);
	args.newRoot = newRoot;
	args.pages_copied = 0;

	ret = __dune_vm_page_walk_batch(root, VA_START, VA_END, &__dune_vm_clone_helper, &__dune_vm_clone_last_level_page_helper,
							  &args, 3, CREATE_NONE);
	if (ret < 0) {
		dune_vm_free(newRoot);
		return NULL;
	}
	printf("%d pages copied\n", args.pages_copied);
	return newRoot;
}

/**
 * Clone a page root.
 */
ptent_t *dune_vm_clone_custom(ptent_t *root, page_walk_cb cb)
{
	int ret;
	ptent_t *newRoot;

	newRoot = alloc_page();
	memset(newRoot, 0, PGSIZE);

	ret = __dune_vm_page_walk(root, VA_START, VA_END, cb,
							  newRoot, 3, CREATE_NONE);
	if (ret < 0) {
		dune_vm_free(newRoot);
		return NULL;
	}

	return newRoot;
}

static int __dune_vm_free_helper(const void *arg, ptent_t *pte, void *va, int level)
{
	struct page *pg = dune_pa2page(PTE_ADDR(*pte));

	if (dune_page_isfrompool(PTE_ADDR(*pte)))
		dune_page_put(pg);

	// Invalidate mapping
	*pte = 0;

	return 0;
}

/**
 * Free the page table and decrement the reference count on any pages.
 */
void dune_vm_free(ptent_t *root)
{
	// XXX: Should only need one page walk
	// XXX: Hacky - Until I fix ref counting
	__dune_vm_page_walk(root, VA_START, VA_END,
			&__dune_vm_free_helper, NULL,
			3, CREATE_NONE);

	__dune_vm_page_walk(root, VA_START, VA_END, &__dune_vm_free_helper, NULL, 2,
						CREATE_NONE);

	__dune_vm_page_walk(root, VA_START, VA_END, &__dune_vm_free_helper, NULL, 1,
						CREATE_NONE);

	put_page(root);

	return;
}

void dune_vm_unmap(ptent_t *root, void *va, size_t len)
{
	/* FIXME: Doesn't free as much memory as it could */
	__dune_vm_page_walk(root, va, va + len - 1, &__dune_vm_free_helper, NULL, 3,
						CREATE_NONE);

	dune_flush_tlb();
}

void dune_vm_default_pgflt_handler(uintptr_t addr, uint64_t fec)
{
	ptent_t *pte = NULL;
	int rc;

	/*
	 * Assert on present and reserved bits.
	 */
	assert(!(fec & (FEC_P | FEC_RSV)));

	rc = dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	assert(rc == 0);

	if ((fec & FEC_W) && (*pte & PTE_COW)) {
		void *newPage;
		struct page *pg = dune_pa2page(PTE_ADDR(*pte));
		ptent_t perm = PTE_FLAGS(*pte);

		// Compute new permissions
		perm &= ~PTE_COW;
		perm |= PTE_W;

		if (dune_page_isfrompool(PTE_ADDR(*pte)) && pg->ref == 1) {
			*pte = PTE_ADDR(*pte) | perm;
			return;
		}

		// Duplicate page
		newPage = alloc_page();
		memcpy(newPage, (void *)PGADDR(addr), PGSIZE);

		// Map page
		if (dune_page_isfrompool(PTE_ADDR(*pte))) {
			dune_page_put(pg);
		}
		*pte = PTE_ADDR(newPage) | perm;

		// Invalidate
		dune_flush_tlb_one(addr);
	}
}
