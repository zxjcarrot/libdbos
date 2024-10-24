#include <stdio.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#define _GNU_SOURCE
#define __USE_GNU 1
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <x86intrin.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "libdune/dune.h"
#include "bench.h"

#define SYSCALL_N 500000
#define MAP_ADDR 0x400000000000

static char *mem;
unsigned long trap_tsc, overhead;
unsigned long time = 0;
unsigned long s = 0;
static void prime_memory(void)
{
	int i;

	for (i = 0; i < NRPGS * 2; i++) {
		mem[i * PGSIZE] = i;
	}
}

static void benchmark1_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	ptent_t *pte;
	int accessed;

	time += rdtscllp() - trap_tsc;

	dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	*pte |= PTE_P | PTE_W | PTE_U | PTE_A | PTE_D;

	dune_vm_lookup(pgroot, (void *)addr + NRPGS * PGSIZE, 0, &pte);
	accessed = *pte & PTE_A;
	*pte = PTE_ADDR(*pte);
	if (accessed)
		dune_flush_tlb_one(addr + NRPGS * PGSIZE);
}


static void make_writable_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	ptent_t *pte;
	int accessed;

	dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	*pte |= PTE_P | PTE_W | PTE_U | PTE_A | PTE_D;
}

static void make_writable_and_allocate_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	ptent_t *pte;
	int accessed;


	dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	volatile char *p = dune_alloc_page_internal();
	*pte |= PTE_P | PTE_W | PTE_U | PTE_A | PTE_D;// | PTE_ADDR(p);
	s += (uint64_t)p;
}

static void make_writable_and_allocate_zeroed_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	ptent_t *pte;

	dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	*pte |= PTE_P | PTE_W | PTE_U | PTE_A | PTE_D;
	volatile char *p = dune_alloc_page_internal();
	//*pte = PTE_P | PTE_W | PTE_U | PTE_A | PTE_D | PTE_ADDR(p);
	memset(p, 0, PGSIZE);
	//memcpy(p, (void*)PGADDR(addr), PGSIZE);
	s += (uint64_t)p[8];
}


static void benchmark1(void)
{
	int i;

	for (i = 0; i < NRPGS; i++) {
		trap_tsc = dune_get_ticks();
		mem[i * PGSIZE] = i;
	}
}

static void benchmark3(void)
{
	int i;

	for (i = 0; i < NRPGS; i++) {
		mem[i * PGSIZE] = i;
	}
}

static void benchmark2_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	ptent_t *pte;

	dune_vm_lookup(pgroot, (void *)addr, 0, &pte);
	*pte |= PTE_P | PTE_W | PTE_U | PTE_A | PTE_D;
}

static void benchmark2(void)
{
	int i;

	dune_vm_mprotect(pgroot, (void *)MAP_ADDR, PGSIZE * NRPGS, PERM_R);

	for (i = 0; i < NRPGS; i++) {
		mem[i * PGSIZE] = i;
	}
}


static void syscall_handler(struct dune_tf *tf)
{
	// static int counter = 0;
	// int syscall_num = tf->rax;
	// ++counter;
	// if (syscall_num == 666) {
	// 	dune_ret_from_user(0);
	// 	return;
	// }

	dune_passthrough_syscall(tf);
//	dune_ret_from_user(0);
}

static void benchmark_syscall(void)
{
	int i;
	unsigned long ticks;

	dune_register_syscall_handler(syscall_handler);

	synch_tsc();
	ticks = dune_get_ticks();

	// for (i = 0; i < N; i++) {
	// 	int ret;

	// 	asm volatile("movq $39, %%rax \n\t" // get_pid
	// 				 "vmcall \n\t"
	// 				 "mov %%eax, %0 \n\t"
	// 				 : "=r"(ret)::"rax");
	// }
	for (i = 0; i < SYSCALL_N; i++) {
		syscall(SYS_gettid);
	}

	dune_printf("System call took %ld cycles, overhead %ld\n",
				(rdtscllp() - ticks - overhead) / (SYSCALL_N), overhead);
}

static void benchmark_fault(void)
{
	int i;
	unsigned long ticks;
	char *fm = dune_mmap(NULL, N * PGSIZE, PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	synch_tsc();
	ticks = dune_get_ticks();
	for (i = 0; i < N; i++) {
		fm[i * PGSIZE] = i;
	}

	dune_printf("Kernel fault took %ld cycles\n",
				(rdtscllp() - ticks - overhead) / N);
}

static void benchmark_appel1(void)
{
	int i;
	unsigned long tsc, avg_appel1 = 0, avg_user_fault = 0;

	dune_register_pgflt_handler(benchmark1_handler);

	for (i = 0; i < N; i++) {
		dune_vm_mprotect(pgroot, (void *)MAP_ADDR, PGSIZE * NRPGS, PERM_R);

		synch_tsc();
		time = 0;
		tsc = dune_get_ticks();
		benchmark1();

		avg_appel1 += rdtscllp() - tsc - overhead;
		avg_user_fault += (time - overhead * NRPGS) / NRPGS;
	}

	dune_printf("User fault took %ld cycles\n", avg_user_fault / N);
	dune_printf("PROT1,TRAP,UNPROT took %ld cycles\n", avg_appel1 / N);
}

static void benchmark_cow_clear_wp(void)
{
	int i;
	unsigned long tsc, avg_user_fault = 0;

	dune_register_pgflt_handler(make_writable_handler);

	for (i = 0; i < N; i++) {
		dune_vm_mprotect(pgroot, (void *)MAP_ADDR, PGSIZE * NRPGS, PERM_R);

		synch_tsc();
		unsigned long start = dune_get_ticks();
		benchmark3();
		unsigned long end = dune_get_ticks();

		avg_user_fault += (end - start);
	}

	dune_printf("Clear-wp fault took %ld cycles\n", avg_user_fault / (N * NRPGS));
}

static void benchmark_cow_clear_alloc(void)
{
	int i;
	unsigned long tsc, avg_user_fault = 0;

	dune_register_pgflt_handler(make_writable_and_allocate_handler);

	for (i = 0; i < N; i++) {
		dune_vm_mprotect(pgroot, (void *)MAP_ADDR, PGSIZE * NRPGS, PERM_R);

		synch_tsc();
		unsigned long start = dune_get_ticks();
		benchmark3();
		unsigned long end = dune_get_ticks();

		avg_user_fault += (end - start);
	}

	dune_printf("Clear-wp and allocation fault took %ld cycles\n", avg_user_fault / (N * NRPGS));
}

static void benchmark_cow_clear_alloc_zero(void)
{
	int i;
	unsigned long tsc, avg_user_fault = 0;

	dune_register_pgflt_handler(make_writable_and_allocate_zeroed_handler);

	for (i = 0; i < N; i++) {
		dune_vm_mprotect(pgroot, (void *)MAP_ADDR, PGSIZE * NRPGS, PERM_R);

		synch_tsc();
		unsigned long start = dune_get_ticks();
		benchmark3();
		unsigned long end = dune_get_ticks();
		avg_user_fault += (end - start);
	}

	dune_printf("Clear-wp and allocation zero fault took %ld cycles\n", avg_user_fault / (N * NRPGS));
}


static void benchmark_appel2(void)
{
	int i;
	unsigned long tsc, avg = 0;

	dune_register_pgflt_handler(benchmark2_handler);

	for (i = 0; i < N; i++) {
		dune_vm_mprotect(pgroot, (void *)MAP_ADDR, PGSIZE * NRPGS * 2,
						 PERM_R | PERM_W);
		prime_memory();

		synch_tsc();
		tsc = dune_get_ticks();
		benchmark2();
		avg += rdtscllp() - tsc - overhead;
	}

	dune_printf("PROTN,TRAP,UNPROT took %ld cycles\n", avg / N);
}


#define NUM_OPERATIONS 50000
#define BLOCK_SIZE 4096

void perform_read_benchmark(int fd, off_t file_size, void *buffer) {
    unsigned long long total_cycles = 0;
    unsigned long long min_cycles = ULLONG_MAX;
    unsigned long long max_cycles = 0;

    for (int i = 0; i < NUM_OPERATIONS; i++) {
        off_t offset = (rand() % (file_size / BLOCK_SIZE)) * BLOCK_SIZE;

        unsigned long long start = rdtscllp();
        ssize_t bytes_read = pread(fd, buffer, BLOCK_SIZE, offset);
        unsigned long long end = rdtscllp();

        if (bytes_read != BLOCK_SIZE) {
            fprintf(stderr, "Error reading file: %s\n", strerror(errno));
            continue;
        }

        unsigned long long cycles = end - start;
        total_cycles += cycles;

        if (cycles < min_cycles) min_cycles = cycles;
        if (cycles > max_cycles) max_cycles = cycles;
    }

    double avg_cycles = (double)total_cycles / NUM_OPERATIONS;

    printf("Read Latency Benchmark Results:\n");
    printf("Average latency: %.2f CPU cycles\n", avg_cycles);
    printf("Minimum latency: %llu CPU cycles\n", min_cycles);
    printf("Maximum latency: %llu CPU cycles\n", max_cycles);
    printf("\n");
}

void perform_write_benchmark(int fd, off_t file_size, void *buffer) {
    unsigned long long total_cycles = 0;
    unsigned long long min_cycles = ULLONG_MAX;
    unsigned long long max_cycles = 0;

    for (int i = 0; i < NUM_OPERATIONS; i++) {
        off_t offset = (rand() % (file_size / BLOCK_SIZE)) * BLOCK_SIZE;

        unsigned long long start = rdtscllp();
        ssize_t bytes_written = pwrite(fd, buffer, BLOCK_SIZE, offset);
        unsigned long long end = rdtscllp();

        if (bytes_written != BLOCK_SIZE) {
            fprintf(stderr, "Error writing file: %s\n", strerror(errno));
            continue;
        }

        unsigned long long cycles = end - start;
        total_cycles += cycles;

        if (cycles < min_cycles) min_cycles = cycles;
        if (cycles > max_cycles) max_cycles = cycles;
    }

    double avg_cycles = (double)total_cycles / NUM_OPERATIONS;

    printf("Write Latency Benchmark Results:\n");
    printf("Average latency: %.2f CPU cycles\n", avg_cycles);
    printf("Minimum latency: %llu CPU cycles\n", min_cycles);
    printf("Maximum latency: %llu CPU cycles\n", max_cycles);
    printf("\n");
}

void benchmark_io(bool direct_io) {
	const char *filename = "/Higgs1/test_file";
	int flags = direct_io ? O_RDWR | O_DIRECT: O_RDWR;
    int fd = open(filename, flags);
    if (fd == -1) {
        perror("Error opening file");
        exit(1);
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("Error getting file size");
        close(fd);
        exit(1);
    }

    off_t file_size = st.st_size;
    if (file_size < BLOCK_SIZE) {
        fprintf(stderr, "File is too small. Minimum size: %d bytes\n", BLOCK_SIZE);
        close(fd);
        exit(1);
    }

	void *buffer;
    if (posix_memalign(&buffer, BLOCK_SIZE, BLOCK_SIZE) != 0) {
        perror("Error allocating aligned memory");
        close(fd);
        exit(1);
    }

    // Initialize buffer with random data for write operations
    for (int i = 0; i < BLOCK_SIZE; i++) {
        ((char *)buffer)[i] = rand() % 256;
    }

	srand(0);


	// Perform read benchmark
    perform_read_benchmark(fd, file_size, buffer);

    // Perform write benchmark
    perform_write_benchmark(fd, file_size, buffer);

    free(buffer);
    close(fd);
}

int main(int argc, char *argv[])
{
	int ret;

	overhead = measure_tsc_overhead();
	printf("TSC overhead is %ld\n", overhead);

	ret = dune_init_and_enter();
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}

	dune_printf("Benchmarking dune performance...\n");

	// benchmark_io(false);
	// benchmark_io(false);
	// benchmark_io(true);
	benchmark_syscall();
	benchmark_fault();

	ret = dune_vm_map_pages(pgroot, (void *)MAP_ADDR, 2 * NRPGS * PGSIZE,
							PERM_R | PERM_W);
	if (ret) {
		printf("failed to setup memory mapping\n");
		return ret;
	}

	mem = (void *)MAP_ADDR;
	prime_memory();

	benchmark_appel1();
	benchmark_appel2();

	benchmark_cow_clear_wp();
	benchmark_cow_clear_alloc_zero();
	benchmark_cow_clear_alloc();
	printf("s %lu\n", s);
	return 0;
}
