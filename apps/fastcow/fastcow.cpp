#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <memory.h>

#include <vector>
#include <atomic>
#include <stdexcept>
#include "RandomGenerator.hpp"


#include "bench.h"

#ifdef __cplusplus
extern "C"{
#endif 

#include "dbos.h"
#include "fast_spawn.h"

void *asm_memcpy(void *dest, const void *src, size_t n);
void *asm_memset(void *s, int c, size_t n);

#ifdef __cplusplus
}
#endif

#define MB (1024 * 1024ULL)

unsigned long rdtsc_overhead;
static uint64_t debug_va_addr = 0;
volatile bool writer_ready[128];
uint64_t cow_region_begin;
uint64_t cow_region_end;
volatile uint64_t cycles_start;
volatile double program_start_timestamp;
volatile bool main_exited = false;

typedef struct {
    char *memory;
    size_t size;
    size_t count;
	int thread_id;
	int num_threads;
	double avg_cycles_per_access;
	volatile bool *ready_to_go;
} ThreadArg;

void *t_snapshot(void *arg) {
	struct timeval start, end;
	uint64_t s = 0;
	int scanned = 0;
	ThreadArg * targ = (ThreadArg*) arg;
	double elapsed = 0;
	uint64_t ssss = 0;
	//asm volatile("cli");
	//FILE *file;
	dune_printf("t_snapshot\n");
	//return NULL;
    // Open the file for writing
    // file = fopen("example.txt", "w");
    // if (file == NULL) {
    //     perror("Error opening file");
    //     return NULL;
    // }
	gettimeofday(&start, NULL);
	while(true) {
		
		gettimeofday(&end, NULL);
		elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
		if (elapsed > 5) {
			break;
		}
		struct timeval bench_start, bench_end;
		gettimeofday(&bench_start, NULL);
		ssss = 0;
		for (int i = 0; i < targ->size; i += 8) {
			ssss += *(uint64_t*)(&targ->memory[i]);
		}
		gettimeofday(&bench_end, NULL);
		elapsed = (bench_end.tv_sec - bench_start.tv_sec) + (bench_end.tv_usec - bench_start.tv_usec) / 1000000.0;
		printf("Time taken for fork: %.6f seconds, s %lu, throughput %fGB/s\n", elapsed, ssss, targ->size / (elapsed * 1024 * 1024 * 1024UL));
	}
	//fclose(file);
	//asm volatile("sti");
	//sleep(20);
	//dune_printf("t_snapshot scan took %f seconds, ssss %lu, targ->size*2 %lu, throughput %fGB/s\n", elapsed, ssss, targ->size * 2, targ->size /1024/1024.0/1024 / elapsed);
	assert(s == ssss);
	//dune_printf("tlb shootdowns %d, page_faults %d\n", state->dbos_tlb_shootdowns, state->page_faults);

	return NULL;
}

constexpr size_t kHistogramBinCount = 5000000;

class LatencyHistogram {
private:
    std::vector<int> bins;
    double binWidth;
    double minLatency;

public:
    // Constructor: specify the number of bins, min latency, and the width of each bin
    LatencyHistogram(int numBins, double minLatency, double binWidth)
        : bins(numBins, 0), binWidth(binWidth), minLatency(minLatency) {
		memset(bins.data(), 0, bins.size() * sizeof(int));
    }

    // Add a new latency measurement
    void addLatency(double latency) {
        int index = static_cast<int>((latency - minLatency) / binWidth);
        if (index < 0 || index >= bins.size()) {
            //throw std::out_of_range("Latency" + std::to_string(latency) + " is out of the histogram range.");
			return;
        }
        bins[index] += 1;
    }

    // Get the count of latencies in a specific bin
    int getBinCount(int binIndex) const {
        if (binIndex < 0 || binIndex >= bins.size()) {
            throw std::out_of_range("Bin index is out of range.");
        }
        return bins[binIndex];
    }

    // Calculate and return the percentile value
    double getPercentile(double percentile) {
        int totalLatencies = 0;
        for (const auto& bin : bins) {
            totalLatencies += bin;
        }

        if (totalLatencies == 0) {
            //throw std::runtime_error("No latencies recorded.");
			return 0;
        }

        int targetCount = static_cast<int>(percentile / 100.0 * totalLatencies);
        size_t runningCount = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            runningCount += bins[i];
            if (runningCount >= targetCount) {
                return minLatency + i * binWidth;
            }
        }

        return minLatency + bins.size() * binWidth; // Maximum latency
    }

    void Merge(const LatencyHistogram & other) {
        for (int i = 0; i < bins.size(); ++i) {
            bins[i] += other.bins[i];
        }
    }


    double getAverageLatency() const {
        double totalLatency = 0;
        int totalCount = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            int count = bins[i];
            totalLatency += (minLatency + i * binWidth) * count;
            totalCount += count;
        }
        return totalCount > 0 ? totalLatency / totalCount : 0;
    }

    double getMedianLatency() const {
        int totalLatencies = 0;
        for (const auto& bin : bins) {
            totalLatencies += bin;
        }

        if (totalLatencies == 0) {
            return 0; // No latencies recorded
        }

        int halfCount = totalLatencies / 2;
        int runningCount = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            runningCount += bins[i];
            if (runningCount >= halfCount) {
                return minLatency + i * binWidth;
            }
        }
        return 0; // Should not reach here
    }

    double getMaxLatency() const {
        for (int i = bins.size() - 1; i >= 0; --i) {
            if (bins[i] > 0) {
                return minLatency + (i + 1) * binWidth;
            }
        }
        return 0; // No latencies recorded
    }

    double getMinLatency() const {
        for (size_t i = 0; i < bins.size(); ++i) {
            if (bins[i] > 0) {
                return minLatency + i * binWidth;
            }
        }
        return 0; // No latencies recorded
    }
};

char data[1024*1024 * 512];

void *random_access_thread(void *arg) {
	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	printf("random_access_thread worker before started on cpu %d\n", cpu_id);
	volatile int ret = dune_enter();
	if (ret) {
		printf("random_access_thread: failed to enter dune\n");
		return NULL;
	}
	assert(sched_getcpu() == dune_ipi_get_cpu_id());
	
	volatile ThreadArg *thread_arg = (ThreadArg *)arg;
	char* memory = thread_arg->memory;
	size_t size = thread_arg->size;
	int cnt = thread_arg->count; 
	int num_threads = thread_arg->num_threads;
	int thread_id = thread_arg->thread_id;
	size_t num_pages = size / PGSIZE;
	size_t pages_per_worker = num_pages / num_threads;
	struct timeval start, end;
	volatile bool *ready_to_go = thread_arg->ready_to_go;
	gettimeofday(&start, NULL);
    //dune_prefault_pages();
	//memset(memory, 0, size);
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

	auto state = dune_get_fast_spawn_state();
	dune_set_thread_state(state, cpu_id, DUNE_THREAD_MAIN);
	writer_ready[cpu_id] = true;
	asm volatile("mfence" ::: "memory");
    
	gettimeofday(&start, NULL);
	double seconds_since_program_start = (start.tv_sec) + (start.tv_usec) / 1000000.0 - program_start_timestamp;

	dune_printf("random_access_thread worker started on cpu %d\n", cpu_id);
	while(!(*ready_to_go));
    //sleep(1);
	LatencyHistogram *hist = new LatencyHistogram(kHistogramBinCount, 0, 1);
	dune_printf("Time taken for dune_prefault_pages on thread %d: %.6f seconds, seconds_since_program_start %.6f seconds\n", thread_id, elapsed, seconds_since_program_start);

	//usleep(100000);
    gettimeofday(&start, NULL);
	//dune_printf("3 random_access_thread started on worker %d, t %lu\n", cpu_id, t);
    uint64_t s = 0;
	uint64_t accesses = 0;
	unsigned long ts = rdtscllp();
	unsigned long max_latency = 0;
	int start_p = thread_id * (pages_per_worker);
	int end_p = (thread_id + 1) * (pages_per_worker);
	if (end_p > num_pages) {
		end_p = num_pages;
	}
	state->cycles_spent_in_flt[cpu_id] = 0;
	state->cycles_access_start[cpu_id] = 0;
	state->cycles_spent_all_access[cpu_id] = 0;
	state->cycles_spent_for_interrupt[cpu_id] = 0;
	state->interrupt_ts[cpu_id] = 0;
	//return hist;
	for (size_t i = start_p; i < end_p; i++) {
		unsigned long ts1 = rdtscll();
		// uint64_t x = RandomGenerator::getRandU64();
		// size_t index = (x % (end_p - start_p) + start_p) * PGSIZE;
		//state->cycles_access_start[cpu_id] = rdtscll();
		size_t index = i * PGSIZE;
		//memset(memory + index, 1, PGSIZE);
		memory[index] = 1;
		// for (int j = 0; j < PGSIZE; ++j) {
		// 	s += memory[index + j];
		// }
		//memory[index] = 1; // Simple write operation to trigger CoW
		//s += memory[index];
		//accesses += PGSIZE;
		accesses += 1;
		// if (++accesses % 10000 == 0) {
		// 	printf("%lu accesses on thread %d, start_p %d, end_p %d\n", accesses, cpu_id, start_p, end_p);
		// }
		unsigned long ts2 = rdtscll();
		hist->addLatency(ts2 - ts1);
		max_latency = max_latency <  (ts2 - ts1) ? ts2 - ts1 : max_latency;
	}
	//assert(accesses == s);
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
	double minLatency = hist->getMinLatency();
    double avgLatency = hist->getAverageLatency();
    double maxLatency = hist->getMaxLatency();
    double p50Latency = hist->getPercentile(50);
    double p75Latency = hist->getPercentile(75);
    double p90Latency = hist->getPercentile(90);
    double p95Latency = hist->getPercentile(95);
    double p99Latency = hist->getPercentile(99);
    double p999Latency = hist->getPercentile(99.9);
    printf("thread %d min %.2f  avg %.2f max %.2f p50 %.2f p75 %.2f p90 %.2f p95 %.2f p99 %.2f p99.9 %.2f\n", thread_id, minLatency, avgLatency, maxLatency, p50Latency, p75Latency, p90Latency, p95Latency, p99Latency, p999Latency);
    printf("Thread %d time taken for random memory access: %.6f seconds s %lu, %d page_faults, max_latency %lu, shootdowns %lu, cycles per shootdown %lu, cycles per fault %lu, avg_cycles_spent_per_fault_handler %lu, avg_cycles_per_access %lu, avg_cycles_to_interrupt %lu, num_pages %lu, pages per worker %lu, page range[%d, %d], ipi_calls[%d] %lu\n", thread_id, elapsed, s, state->thread_pgfaults[cpu_id], max_latency, state->shootdowns[cpu_id], state->cycles_spent_for_shootdown[cpu_id] / (state->shootdowns[cpu_id] + 1), (rdtscllp() - ts)/(state->thread_pgfaults[cpu_id]), state->cycles_spent_in_flt[cpu_id] /(accesses) , state->cycles_spent_all_access[cpu_id] / accesses, state->cycles_spent_for_interrupt[cpu_id] / accesses, num_pages, pages_per_worker, start_p, end_p, SNAPSHOT_CORE, state->ipi_calls[SNAPSHOT_CORE]);
	//dune_printf("random_access_thread tlb shootdowns %d, page_faults %d\n", state->dbos_tlb_shootdowns, state->page_faults);
	//dune_ipi_print_stats();
	thread_arg->avg_cycles_per_access = (rdtscllp() - ts)/(accesses);
	sleep(1);
	dune_set_thread_state(state, cpu_id, DUNE_THREAD_NONE);
	//sleep(30);
    return hist;
}

LatencyHistogram hist(kHistogramBinCount, 0, 1);

int main(int argc, char *argv[])
{
	struct timeval start, end;
	gettimeofday(&start, NULL);
	program_start_timestamp = (start.tv_sec) + (start.tv_usec) / 1000000.0;
	volatile int ret;
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);

   	pthread_t main_thread = pthread_self();    
   	pthread_setaffinity_np(main_thread, sizeof(cpu_set_t), &cpuset);

	printf("fastcow: not running dune yet\n");

	int cpu_id = sched_getcpu();
	dune_ipi_set_cpu_id(cpu_id);
	dune_procmap_dump();
	ret = dbos_init(true);
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	}
	ret = dbos_enter();
	if (ret) {
		printf("failed to enter dune\n");
		return ret;
	}

	printf("fastcow: now printing from dune mode on core %d, pgroot %p\n", cpu_id, pgroot);

	if (argc != 5) {
        fprintf(stderr, "Usage: %s <memory_size_in_MB> <ratio_of_memory_to_access> <threads> <fork_or_not> \n", argv[0]);
        return 1;
    }
    int num_threads = 0;
    size_t memory_size = atoi(argv[1]) * MB;
    float perc = atof(argv[2]);
	//size_t memory_size = sizeof(data);
    size_t count = memory_size / 4096 * perc;
    num_threads = atoi(argv[3]);

	//volatile char *memory = (char*)malloc(memory_size);
	volatile char *memory = (volatile char *)mmap(NULL, memory_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	madvise((void*)memory, memory_size, MADV_HUGEPAGE);
	//volatile char * memory = data;
	
	debug_va_addr = ((uint64_t)memory);
    printf("memory %p memory size %lu, perc %f, count %lu, debug_va_addr %p\n",memory, memory_size, perc, count, debug_va_addr);

	dune_set_max_cores(num_threads + 3);
	dune_prefault_pages();
	gettimeofday(&start, NULL);
    memset((char*)memory, 0, memory_size);
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

	printf("Time taken for memset(fault): %.6f seconds, cow region[%p, %p]\n", elapsed, cow_region_begin, cow_region_end);

	assert(sched_getcpu() == dune_ipi_get_cpu_id());
	volatile ThreadArg thread_arg = {(char*)memory, memory_size, count, false};
	if (!memory) {
        perror("Failed to allocate memory");
        return 1;
    }

	rdtsc_overhead = measure_tsc_overhead();
	synch_tsc();
	dune_fast_spawn_configure();

	cow_region_begin = (uint64_t)memory;
	cow_region_end = ((uint64_t)memory) + memory_size;

	//cow_state_init();

	if (dune_fast_spawn_init(1) != 0) {
		printf("dune fast spawn init failed\n");
	} else {
		printf("dune fast spawn init succeeded\n");
	}
	volatile auto state = dune_get_fast_spawn_state();

	dune_set_thread_state(state, cpu_id, DUNE_THREAD_MAIN);

    pthread_t threads[num_threads];
	pthread_attr_t attrs[num_threads];
	cpu_set_t cpus[num_threads];
	ThreadArg targs[num_threads + 1];
	volatile bool ready_to_go = false;
    for (int i = 0; i < num_threads; ++i) { // Random access after fork
		pthread_attr_init(&attrs[i]);
		CPU_ZERO(&cpus[i]);
		CPU_SET((i + 1), &cpus[i]);
		pthread_attr_setaffinity_np(&attrs[i], sizeof(cpu_set_t), &cpus[i]);
		targs[i].memory = (char*)memory;
		targs[i].ready_to_go = &ready_to_go;
		targs[i].thread_id = i;
		targs[i].size = memory_size;
		targs[i].count = count;
		targs[i].num_threads = num_threads;
		targs[i].avg_cycles_per_access = 0;
        pthread_create(&threads[i], &attrs[i], random_access_thread, (void*)&targs[i]);
    }
	
	for (int i = 1; i <= num_threads; ++i) {
		while(!writer_ready[i]);
	}

	dune_fast_spawn_signal_copy_worker();

	sleep(2);

	printf("All ready\n");

	asm volatile("mfence" ::: "memory");
	
	targs[num_threads].memory = (char*)memory;
	targs[num_threads].ready_to_go = &ready_to_go;
	targs[num_threads].thread_id = num_threads;
	targs[num_threads].size = memory_size;
	targs[num_threads].count = count;
	targs[num_threads].num_threads = num_threads;
	gettimeofday(&start, NULL);
    dune_fast_spawn_on_snapshot(t_snapshot, (void*)&targs[num_threads]);
	//dune_vm_page_walk(state->pgroot, VA_START, VA_END, pgtable_set_cow, NULL);
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for fast copy-on-write: %.6f seconds\n", elapsed);
	//sleep(2);
	ready_to_go = true;
	asm volatile("mfence" ::: "memory");
	void *ret_val;
	double avg_cycles_per_access = 0;
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], &ret_val);
		printf("thread %d joined \n", i);
        LatencyHistogram * lHist = (LatencyHistogram *)ret_val;
        hist.Merge(*lHist);
        delete lHist;
		avg_cycles_per_access += targs[i].avg_cycles_per_access;
    }
	avg_cycles_per_access /= num_threads;
    double minLatency = hist.getMinLatency();
    double avgLatency = hist.getAverageLatency();
    double maxLatency = hist.getMaxLatency();
    double p50Latency = hist.getPercentile(50);
    double p75Latency = hist.getPercentile(75);
    double p90Latency = hist.getPercentile(90);
    double p95Latency = hist.getPercentile(95);
    double p99Latency = hist.getPercentile(99);
    double p999Latency = hist.getPercentile(99.9);
    printf("min %.2f  avg %.2f max %.2f p50 %.2f p75 %.2f p90 %.2f p95 %.2f p99 %.2f p99.9 %.2f\n", minLatency, avgLatency, maxLatency, p50Latency, p75Latency, p90Latency, p95Latency, p99Latency, p999Latency);
	//printf("main tlb shootdowns %d, page_faults %d, cycles/fault %lu\n", state->dbos_tlb_shootdowns, state->page_faults, state->page_fault_cycles / state->page_faults);
	printf("Average # cycles per access %lf\n", avg_cycles_per_access);
	main_exited = true;
	sleep(30);
	asm volatile("mfence" ::: "memory");
	return 0;
}
