/*
 * page.c - page management
 */

#define _GNU_SOURCE

#include <errno.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include "dune.h"

#define GROW_SIZE (512)

#define NUM_GLOBAL_LISTS 104
typedef struct freelist {
	pthread_mutex_t page_mutex;
	//int num_pages;
	struct page_head pages_free;
	char _padding[64 - sizeof(pthread_mutex_t) - sizeof(struct page_head)];
} freelist_t;

typedef struct physical_page_allocator_t {
	freelist_t local_lists[NUM_GLOBAL_LISTS];
	//pthread_mutex_t page_mutex;
	uint64_t num_pages;
	//struct page_head pages_free;
	struct page pages[];
} physical_page_allocator_t;

static physical_page_allocator_t *p_allocator;
static uintptr_t p_allocator_begin;
static uintptr_t p_allocator_end;

uintptr_t dune_pmem_alloc_begin()
{
	return p_allocator_begin;
}
uintptr_t dune_pmem_alloc_end()
{
	return p_allocator_end;
}
static void *do_mapping(void *base, unsigned long len)
{
	void *mem;

	mem = mmap((void *)base, len, PROT_READ | PROT_WRITE,
			   MAP_FIXED | MAP_HUGETLB | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (mem != (void *)base) {
		printf("mmap for huge page failed: base %p, len %lu, %s\n", base, len, strerror(errno));
		// try again without huge pages
		mem = mmap((void *)base, len, PROT_READ | PROT_WRITE,
				   MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mem != (void *)base) {
			printf("mmap for huge page failed 2: %s\n", strerror(errno));
			return NULL;
		}
	} else {
		//printf("mmap for huge page succeeded: base %p, len %lu\n", base, len);
	}

	return mem;
}

static int grow_size(int list_id)
{
	uint64_t old_num_pages = p_allocator->num_pages;
	if (old_num_pages >= MAX_PAGES) {
		//printf("grow_size failed, old_num_pages %lu\n", old_num_pages);
		return -ENOMEM;
	}
	int i;
	uint64_t new_num_pages = __atomic_add_fetch(
		&p_allocator->num_pages, GROW_SIZE,
		__ATOMIC_RELEASE); // p_allocator->num_pages + GROW_SIZE;
	if (new_num_pages > MAX_PAGES) {
		__atomic_sub_fetch(
		&p_allocator->num_pages, GROW_SIZE,
		__ATOMIC_RELEASE);
		//printf("grow_size failed, new_num_pages %lu\n", new_num_pages);
		return -ENOMEM;
	}
	void *ptr;
	old_num_pages = new_num_pages - GROW_SIZE;
	ptr = do_mapping((void *)PAGEBASE + old_num_pages * PGSIZE,
					 GROW_SIZE * PGSIZE);
	if (!ptr){
		//printf("grow_size do_mapping failed, old_num_pages %lu\n", old_num_pages);
		return -ENOMEM;
	}
		

	for (i = old_num_pages; i < new_num_pages; i++) {
		p_allocator->pages[i].ref = 0;
		SLIST_INSERT_HEAD(&p_allocator->local_lists[list_id].pages_free,
						  &p_allocator->pages[i], link);
	}

	return 0;
}

static int grow_size_and_populate(int list_id)
{
	int i;
	uint64_t new_num_pages = __atomic_add_fetch(
		&p_allocator->num_pages, GROW_SIZE,
		__ATOMIC_RELEASE); // p_allocator->num_pages + GROW_SIZE;
	void *ptr;
	uint64_t old_num_pages = new_num_pages - GROW_SIZE;
	ptr = do_mapping((void *)PAGEBASE + old_num_pages * PGSIZE,
					 GROW_SIZE * PGSIZE);
	if (!ptr)
		return -ENOMEM;

	for (i = old_num_pages; i < new_num_pages; i++) {
		p_allocator->pages[i].ref = 0;
		SLIST_INSERT_HEAD(&p_allocator->local_lists[list_id].pages_free,
						  &p_allocator->pages[i], link);
	}
	// Pre-populate the page
	//memset(ptr, 1, GROW_SIZE * PGSIZE);
	//printf("old_num_pages %d, mapping %p got %p, memseted\n", old_num_pages, PAGEBASE + old_num_pages * PGSIZE, ptr);
	return 0;
}

void dune_page_stats(void)
{
	int i;
	int num_alloc = 0;

	for (i = 0; i < p_allocator->num_pages; i++) {
		if (p_allocator->pages[i].ref != 0)
			num_alloc++;
	}

	dune_printf("DUNE Page Allocator: Alloc %d, Free %d, Total %d\n", num_alloc,
				p_allocator->num_pages - num_alloc, p_allocator->num_pages);
}

// static int fill_local_list(int cpu_id) {
// 	struct page *pg;
// 	int ret = 0;
// 	int max_fill = GROW_SIZE * 2;
// 	pthread_mutex_lock(&p_allocator->page_mutex);
// 	if (SLIST_EMPTY(&p_allocator->pages_free)) {
// 		ret = grow_size();
// 		if (ret) {
// 			pthread_mutex_unlock(&p_allocator->page_mutex);
// 			return ret;
// 		}
// 	}
// 	while (!SLIST_EMPTY(&p_allocator->pages_free) && max_fill--) {
// 		pg = SLIST_FIRST(&p_allocator->pages_free);
// 		SLIST_REMOVE_HEAD(&p_allocator->pages_free, link);
// 		SLIST_INSERT_HEAD(&p_allocator->local_lists[cpu_id].pages_free, pg, link);
// 		p_allocator->local_lists[cpu_id].num_pages++;
// 	}

// 	pthread_mutex_unlock(&p_allocator->page_mutex);

// 	return ret;
// }

static void prezero_freelist(freelist_t* list) {
	struct page * pg;
	SLIST_FOREACH (pg, (&list->pages_free), link) {
		void * addr = dune_page2pa(pg);
		memset(addr, 1, PGSIZE);
	}
}

void dune_prefault_pages() {
	int i;
	for (i = 0; i < NUM_GLOBAL_LISTS; ++i) {
		pthread_mutex_lock(&p_allocator->local_lists[i].page_mutex);
		prezero_freelist(&p_allocator->local_lists[i]);
		pthread_mutex_unlock(&p_allocator->local_lists[i].page_mutex);
	}
}

static uint32_t rand_number() {
	static __thread uint32_t seed = 5323;
    // our initial starting seed is 5323
    if (seed == 5323) {
		seed = dune_get_cpu_id();
	}

    // Take the current seed and generate a new value from it
    // Due to our use of large constants and overflow, it would be
    // very hard for someone to predict what the next number is
    // going to be from the previous one.
    seed = (8253729 * seed + 2396403); 

    return seed;
}

// Assume p_allocator->local_lists[to].page_mutex is taken and steal_from > to
static bool dune_try_steal_pages(int steal_from, int to, int num_pages) {
	bool stolen = false;
	struct page * pg;
	assert(steal_from > to);
	if (pthread_mutex_trylock(&p_allocator->local_lists[steal_from].page_mutex)) {
		return false;
	}
	while (!SLIST_EMPTY(&p_allocator->local_lists[steal_from].pages_free) && num_pages--) {
		pg = SLIST_FIRST(&p_allocator->local_lists[steal_from].pages_free);
		SLIST_REMOVE_HEAD(&p_allocator->local_lists[steal_from].pages_free, link);
		SLIST_INSERT_HEAD(&p_allocator->local_lists[to].pages_free, pg, link);
		stolen = true;
	}
	pthread_mutex_unlock(&p_allocator->local_lists[steal_from].page_mutex);
	return stolen;
}

struct page *dune_page_alloc(void)
{
	int steal_attempts = 0;
	static int alloc_cnt = 0;
	retry:
	if (steal_attempts > 100000) {
		printf(
			"dune_page_alloc failed to allocate a page after %d failed stealing attempts\n",
			steal_attempts);
		return NULL;
	}
	static __thread int last_stealed = -1;
	struct page *pg;
	int cpu_id = dune_get_cpu_id();
	if (cpu_id == -1) {
		cpu_id = 0;
	}
	// if (++alloc_cnt > 1000000) {
	// 	printf("dune_page_alloc %d\n", alloc_cnt);
	// }
	pthread_mutex_lock(&p_allocator->local_lists[cpu_id].page_mutex);
	if (SLIST_EMPTY(&p_allocator->local_lists[cpu_id].pages_free)) {
		if (grow_size(cpu_id)) {
			// now try stealing from other local lists
			if (last_stealed == -1) {
				last_stealed = rand_number() % (NUM_GLOBAL_LISTS - dune_get_max_cores()) + dune_get_max_cores();
				assert(last_stealed < NUM_GLOBAL_LISTS);
			}
			++steal_attempts;
			if (!dune_try_steal_pages(last_stealed, cpu_id, GROW_SIZE)) {
				// stealing failed, retry again
				last_stealed = -1;
			} else {
				//printf("cpu %d stolen %lu pages from cpu %d\n", cpu_id, GROW_SIZE, last_stealed);
			}
			pthread_mutex_unlock(&p_allocator->local_lists[cpu_id].page_mutex);
			//return NULL;
			goto retry;
		}
	}

	pg = SLIST_FIRST(&p_allocator->local_lists[cpu_id].pages_free);
	SLIST_REMOVE_HEAD(&p_allocator->local_lists[cpu_id].pages_free, link);
	pthread_mutex_unlock(&p_allocator->local_lists[cpu_id].page_mutex);

	dune_page_get(pg);

	return pg;
}

void dune_page_free(struct page *pg)
{
	assert(!pg->ref);
	int cpu_id = dune_get_cpu_id();
	if (cpu_id == -1) {
		cpu_id = 0;
	}
	pthread_mutex_lock(&p_allocator->local_lists[cpu_id].page_mutex);
	SLIST_INSERT_HEAD(&p_allocator->local_lists[cpu_id].pages_free, pg, link);
	pthread_mutex_unlock(&p_allocator->local_lists[cpu_id].page_mutex);
}

bool dune_page_isfrompool(physaddr_t pa)
{
	// XXX: Insufficent?
	return (pa >= PAGEBASE) &&
		   (pa < PAGEBASE + p_allocator->num_pages * PGSIZE);
}

static int dune_page_local_freelist_init(freelist_t *list)
{
	int ret = pthread_mutex_init(&list->page_mutex, NULL);
	if (ret) {
		return ret;
	}
	//list->num_pages = 0;
	SLIST_INIT(&list->pages_free);
	assert(sizeof(freelist_t) == 64);
	return 0;
};

int dune_page_init(void)
{
	int i;
	int ret;
	// void *mem;
	// int num_pages = GROW_SIZE;

	// mem = do_mapping((void *)PAGEBASE, num_pages * PGSIZE);
	// if (!mem)
	// 	return -ENOMEM;

	size_t size = ROUND_UP(sizeof(struct page) * MAX_PAGES +
							   sizeof(physical_page_allocator_t),
						   PGSIZE);
	p_allocator = mmap(NULL, size, PROT_READ | PROT_WRITE,
					   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (p_allocator == MAP_FAILED) {
		printf("dune_page_init mmap failed for %lu bytes\n", size);
		goto err;
	}
	p_allocator_begin = (uintptr_t)p_allocator;
	p_allocator_end = (uintptr_t)(((char *)p_allocator) + size);
	printf(
		"dune_page_init p_allocator size %lu [%p , %p], physical memory [%p, %p]\n",
		size, p_allocator, p_allocator_end, PAGEBASE, PAGEBASE_END);

	//ret = pthread_mutex_init(&p_allocator->page_mutex, NULL);
	//assert(ret == 0);
	p_allocator->num_pages = 0;
	// SLIST_INIT(&p_allocator->pages_free);
	// for (i = 0; i < num_pages; i++) {
	// 	p_allocator->pages[i].ref = 0;
	// 	SLIST_INSERT_HEAD(&p_allocator->pages_free, &p_allocator->pages[i], link);
	// }

	for (i = 0; i < NUM_GLOBAL_LISTS; ++i) {
		// printf("dune allocator list %d mutex addr %p\n", i, &p_allocator->local_lists[i].page_mutex);
		ret = dune_page_local_freelist_init(&p_allocator->local_lists[i]);
		if (ret != 0) {
			return ret;
		}
	}
	int j = 0;
	for (i = 0; i < MAX_PAGES; i += GROW_SIZE) {
		ret = grow_size(j);
		if (ret != 0) {
			return ret;
		}
		j = (j + 1) % NUM_GLOBAL_LISTS;
	}
	//dune_procmap_dump();
	printf("dune_page_init succeeded, free pages per list %lu\n", MAX_PAGES /NUM_GLOBAL_LISTS );
	// dune_prefault_pages();
	// printf("dune_page_init finished pre-faulting %lu pages\n", MAX_PAGES );
	return 0;

err:
	//munmap((void *)PAGEBASE, num_pages * PGSIZE);
	return -ENOMEM;
}

inline struct page *dune_pa2page(physaddr_t pa)
{
	return &p_allocator->pages[PPN(pa - PAGEBASE)];
}

inline physaddr_t dune_page2pa(struct page *pg)
{
	return PAGEBASE + ((pg - p_allocator->pages) << PGSHIFT);
}

inline struct page *dune_page_get(struct page *pg)
{
	assert(pg >= p_allocator->pages);
	assert(pg < (p_allocator->pages + p_allocator->num_pages));

	pg->ref++;

	return pg;
}

inline void dune_page_put(struct page *pg)
{
	assert(pg >= p_allocator->pages);
	assert(pg < (p_allocator->pages + p_allocator->num_pages));

	pg->ref--;

	if (!pg->ref)
		dune_page_free(pg);
}