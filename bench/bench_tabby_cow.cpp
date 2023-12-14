#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <vector>
#include <atomic>
#include <stdexcept>
#include "RandomGenerator.hpp"
#ifdef __cplusplus
extern "C"{
#endif 

#include "libdune/dune.h"
#ifdef __cplusplus
}
#endif

#define MB (1024 * 1024ULL)

typedef struct {
    char *memory;
    size_t size;
    size_t count;
} ThreadArg;

constexpr size_t kHistogramBinCount = 50000000;
static inline unsigned long rdtscllp(void)
{
	unsigned int a, d;
	asm volatile("rdtscp" : "=a"(a), "=d"(d) : : "%rbx", "%rcx");
	return ((unsigned long)a) | (((unsigned long)d) << 32);
}

class LatencyHistogram {
private:
    std::vector<int> bins;
    double binWidth;
    double minLatency;

public:
    // Constructor: specify the number of bins, min latency, and the width of each bin
    LatencyHistogram(int numBins, double minLatency, double binWidth)
        : bins(numBins, 0), binWidth(binWidth), minLatency(minLatency) {
    }

    // Add a new latency measurement
    void addLatency(double latency) {
        int index = static_cast<int>((latency - minLatency) / binWidth);
        if (index < 0 || index >= bins.size()) {
            throw std::out_of_range("Latency" + std::to_string(latency) + " is out of the histogram range.");
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
            throw std::runtime_error("No latencies recorded.");
        }

        int targetCount = static_cast<int>(percentile / 100.0 * totalLatencies);
        int runningCount = 0;
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

static uint32_t seedStart = 1;
inline uint32_t random_u32(uint32_t prev) {
    return prev*1664525U + 1013904223U; // assuming complement-2 integers and non-signaling overflow
}


void* perform_random_access(char *memory, size_t size, int cnt) {
    LatencyHistogram *hist = new LatencyHistogram(kHistogramBinCount, 0, 1);
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    for (size_t i = 0; i < size && cnt--; i += 4096) { // Accessing in page size increments
        uint64_t x = RandomGenerator::getRandU64();
        size_t index = x % size;
        unsigned long ts = rdtscllp();
        memory[index] += 1; // Simple write operation to trigger CoW
        unsigned long te = rdtscllp();
        hist->addLatency(te - ts);
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    //printf("Time taken for random memory access: %.6f seconds\n", elapsed);
    return hist;
}


void *random_access_thread(void *arg) {
    //srand(time(NULL)); // Seed for random number generation
    ThreadArg *thread_arg = (ThreadArg *)arg;
    
    return perform_random_access(thread_arg->memory, thread_arg->size, thread_arg->count);
}


void run_benchmark(int num_threads, ThreadArg * arg) {
    void*ret;
    LatencyHistogram hist(kHistogramBinCount, 0, 1);
    pthread_t threads[num_threads];
    for (int i = 0; i < num_threads; ++i) { // Random access after fork
        pthread_create(&threads[i], NULL, random_access_thread, arg);
    }
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], &ret);
        LatencyHistogram * lHist = (LatencyHistogram *)ret;
        hist.Merge(*lHist);
        delete lHist;
    }
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
}

void change_pgroot() {
    ptent_t* new_root = dune_vm_clone(pgroot);
    load_cr3((unsigned long) new_root);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <memory_size_in_MB> <ratio_of_memory_to_access> <threads> <fork_or_not> \n", argv[0]);
        return 1;
    }
    int num_threads = 0;
    size_t memory_size = atoi(argv[1]) * MB;
    float perc = atof(argv[2]);
    char *memory = (char*)malloc(memory_size);
    size_t count = memory_size / 4096 * perc;
    num_threads = atoi(argv[3]);
    bool do_fork = atoi(argv[4]);
    printf("memory size %lu, perc %f, count %lu\n", memory_size, perc, count);
    

    volatile int ret;

	ret = dune_init_and_enter();
	if (ret) {
		printf("failed to initialize dune\n");
		return ret;
	} else {
        printf("entered dune\n");
    }

    ThreadArg thread_arg = {memory, memory_size, count};
    if (!memory) {
        perror("Failed to allocate memory");
        return 1;
    }
    struct timeval start, end;
    gettimeofday(&start, NULL);
    memset(memory, 0, memory_size);
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for memset(fault): %.6f seconds\n", elapsed);

    gettimeofday(&start, NULL);
    change_pgroot();
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for dune_vm_clone: %.6f seconds\n", elapsed);

    run_benchmark(num_threads, &thread_arg);
    return 0;
    free(memory);
    return 0;
}
