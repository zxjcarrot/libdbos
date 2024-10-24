#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <x86intrin.h>
#include <string.h>
#include <errno.h>
#include <liburing.h>
#include <limits.h>
#include "bench.h"

static char *mem;
unsigned long tsc, trap_tsc, overhead;
unsigned long time_c = 0;

#define PGSIZE 4096

static void prime_memory(void)
{
	int i;

	for (i = 0; i < NRPGS * 2; i++) {
		mem[i * PGSIZE] = i;
	}
}

static void benchmark1_handler(int sn, siginfo_t *si, void *ctx)
{
	//	fprintf (stderr, "afault_handler: %x\n", si->si_addr);
	unsigned long addr = (((unsigned long)si->si_addr) & ~(PGSIZE - 1));
	time_c += rdtscllp() - trap_tsc;

	mprotect((void *)addr, PGSIZE, PROT_READ | PROT_WRITE);
	mprotect((void *)addr + PGSIZE * NRPGS, PGSIZE, PROT_READ);
}

static void benchmark1(void)
{
	int i;

	for (i = 0; i < NRPGS; i++) {
		trap_tsc = rdtscll();
		mem[i * PGSIZE] = i;
	}
}

static void benchmark2_handler(int sn, siginfo_t *si, void *ctx)
{
	//	fprintf (stderr, "bfault_handler: %x\n", si->si_addr);
	unsigned long addr = (((unsigned long)si->si_addr) & ~(PGSIZE - 1));
	mprotect((void *)addr, PGSIZE, PROT_READ | PROT_WRITE);
}

static void benchmark2(void)
{
	int i;

	mprotect(mem, NRPGS * PGSIZE, PROT_READ);

	for (i = 0; i < NRPGS; i++) {
		mem[i * PGSIZE] = i;
	}
}

static void benchmark_syscall(void)
{
	int i;
	unsigned long t0;

	synch_tsc();
	t0 = rdtscll();
	for (i = 0; i < 50000; i++) {
		syscall(SYS_gettid);
	}

	printf("System call took %ld cycles\n", (rdtscllp() - t0 - overhead) / 50000);
}

void benchmark_fork_fault(void)
{
	int i;
	unsigned long ticks;
	
	size_t npages = 10000;
	char *fm = mmap(NULL, npages * PGSIZE, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (fm == MAP_FAILED) {
		perror("mmap failed");
	}
	measure_tsc_overhead_n(npages);
	madvise(fm, npages * PGSIZE, MADV_NOHUGEPAGE);
	
	synch_tsc();
	ticks = rdtscll();
	for (i = 0; i < npages; i++) {
		fm[i * PGSIZE] = i;
	}
	printf("Kernel page fault (with allocation) took %ld cycles\n",
		   (rdtscllp() - ticks - overhead) / npages);

	int pid = fork(); // write-protect pages
	if (pid == 0) {
		return 0;
	}

	if (pid < 0) {
        perror("fork failed");
        return;
    } else if (pid == 0) { // Child process
        exit(0);
    } else { // Parent process
        wait(NULL); // Wait for child to exit
    }
	synch_tsc();
	ticks = rdtscll();

	for (i = 0; i < npages; i++) {
		fm[i * PGSIZE] = i;
	}

	printf("Kernel write-protection fault took %ld cycles\n",
		   (rdtscllp() - ticks - overhead) / npages);
}


void benchmark_fault(void)
{
	int i;
	unsigned long ticks;
	char *fm = mmap(NULL, N * PGSIZE, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	madvise(fm, N * PGSIZE, MADV_NOHUGEPAGE);
	synch_tsc();
	ticks = rdtscll();
	for (i = 0; i < N; i++) {
		fm[i * PGSIZE] = i;
	}

	printf("Kernel fault took %ld cycles\n",
		   (rdtscllp() - ticks - overhead) / N);
}

static void benchmark_appel1(void)
{
	struct sigaction act;
	int i;
	unsigned long avg_appel1 = 0, avg_user_fault = 0;

	memset(&act, sizeof(sigaction), 0);
	act.sa_sigaction = benchmark1_handler;
	act.sa_flags = SA_SIGINFO;
	sigemptyset(&act.sa_mask);
	sigaction(SIGSEGV, &act, NULL);

	for (i = 0; i < N; i++) {
		time_c = 0;
		mprotect(mem, NRPGS * PGSIZE, PROT_READ);
		synch_tsc();
		tsc = rdtscll();
		benchmark1();

		avg_appel1 += rdtscllp() - tsc - overhead;
		avg_user_fault += (time_c - overhead * NRPGS) / NRPGS;
	}

	printf("User fault took %ld cycles\n", avg_user_fault / N);
	printf("PROT1,TRAP,UNPROT took %ld cycles\n", avg_appel1 / N);
}

static void benchmark_appel2(void)
{
	struct sigaction act;
	int i;
	unsigned long avg = 0;

	memset(&act, sizeof(sigaction), 0);
	act.sa_sigaction = benchmark2_handler;
	act.sa_flags = SA_SIGINFO;
	sigemptyset(&act.sa_mask);
	sigaction(SIGSEGV, &act, NULL);

	for (i = 0; i < N; i++) {
		mprotect(mem, NRPGS * PGSIZE * 2, PROT_READ | PROT_WRITE);
		prime_memory();

		synch_tsc();
		tsc = rdtscll();
		benchmark2();
		avg += rdtscllp() - tsc - overhead;
	}

	printf("PROTN,TRAP,UNPROT took %ld cycles\n", avg / N);
}


unsigned long long rdtsc() {
    return __rdtsc();
}

#define QUEUE_DEPTH 1
#define NUM_OPERATIONS 50000
#define BLOCK_SIZE 4096

void perform_read_benchmark(int fd, off_t file_size, void *buffer) {
    unsigned long long total_cycles = 0;
    unsigned long long min_cycles = ULLONG_MAX;
    unsigned long long max_cycles = 0;

    for (int i = 0; i < NUM_OPERATIONS; i++) {
        off_t offset = (rand() % (file_size / BLOCK_SIZE)) * BLOCK_SIZE;

        unsigned long long start = rdtsc();
        ssize_t bytes_read = pread(fd, buffer, BLOCK_SIZE, offset);
        unsigned long long end = rdtsc();

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

        unsigned long long start = rdtsc();
        ssize_t bytes_written = pwrite(fd, buffer, BLOCK_SIZE, offset);
        unsigned long long end = rdtsc();

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


void benchmark_io(int direct_io) {
	const char *filename = "/Higgs1/test_file";
	int flags = direct_io ? (O_RDWR | O_DIRECT): O_RDWR;
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

struct io_data {
    unsigned long long start_time;
    off_t offset;
};

void submit_io(struct io_uring *ring, int fd, void *buf, off_t offset, int is_write) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    
    if (is_write)
        io_uring_prep_write(sqe, 0, buf, BLOCK_SIZE, offset);
    else
        io_uring_prep_read(sqe, 0, buf, BLOCK_SIZE, offset);
    sqe->flags |= IOSQE_FIXED_FILE;
}

int perform_uring_benchmark(int fd, off_t file_size, void *buf, int is_write) {
    struct io_uring ring;
    int ret;
    struct io_data *data;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;
    ret = io_uring_queue_init_params(8, &ring, &params);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret));
        close(fd);
        return 1;
    }

    ret = io_uring_register_files(&ring, &fd, 1);
    if(ret) {
        fprintf(stderr, "Error registering buffers: %s", strerror(-ret));
        return 1;
    }

    memset(buf, 'A', BLOCK_SIZE);


    unsigned long long total_cycles = 0;
    unsigned long long min_cycles = -1ULL;
    unsigned long long max_cycles = 0;

    for (int i = 0; i < NUM_OPERATIONS; i++) {
		off_t offset = (rand() % (file_size / BLOCK_SIZE)) * BLOCK_SIZE;
        unsigned long long start_time = rdtsc();
        submit_io(&ring, fd, buf, offset,  is_write);

            ret = io_uring_submit(&ring);
            if (ret < 0) {
                fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
                break;
            }

            struct io_uring_cqe *cqe;
            io_uring_wait_cqe(&ring, &cqe);
            unsigned long long end_time = rdtsc();
            unsigned long long cycles = end_time - start_time;
            if (cqe->res < 0) {
                fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-cqe->res));
                break;
            }
            total_cycles += cycles;
            if (cycles < min_cycles) min_cycles = cycles;
            if (cycles > max_cycles) max_cycles = cycles;

            io_uring_cqe_seen(&ring, cqe);
    }

    printf("%s Latency Benchmark Results (io_uring with polling):\n", is_write ? "Write" : "Read");
    printf("Average latency: %.2f CPU cycles\n", (double)total_cycles / NUM_OPERATIONS);
    printf("Minimum latency: %llu CPU cycles\n", min_cycles);
    printf("Maximum latency: %llu CPU cycles\n", max_cycles);
    printf("\n");

    free(data);
    io_uring_queue_exit(&ring);
    return 0;
}

void benchmark_uring_io(int direct_io) {
	const char *filename = "/Higgs1/test_file";
	int flags = direct_io ? (O_RDWR | O_DIRECT) : O_RDWR;
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
    perform_uring_benchmark(fd, file_size, buffer, 0);

    // Perform write benchmark
    perform_uring_benchmark(fd, file_size, buffer, 1);

    free(buffer);
    close(fd);
}

int main(int argc, char *argv[])
{
	overhead = measure_tsc_overhead();
	printf("TSC overhead is %ld\n", overhead);

	printf("Benchmarking Linux performance...\n");

    // benchmark_uring_io(0);
    // benchmark_uring_io(0);
    // benchmark_uring_io(1);

	// benchmark_io(0);

	// benchmark_io(0);
	// benchmark_io(1);


	benchmark_syscall();
	benchmark_fork_fault();
	
	benchmark_fault();

	mem = mmap(NULL, NRPGS * PGSIZE * 2, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (mem == (void *)-1)
		return -1;

	prime_memory();
	benchmark_appel1();
	benchmark_appel2();

	return 0;
}
