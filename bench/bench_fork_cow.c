#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

#define MB (1024 * 1024ULL)

typedef struct {
    char *memory;
    size_t size;
    size_t count;
} ThreadArg;

static uint32_t seedStart = 1;
inline uint32_t random_u32(uint32_t prev) {
    return prev*1664525U + 1013904223U; // assuming complement-2 integers and non-signaling overflow
}


uint32_t perform_random_read(char *memory, size_t size, int cnt) {
    uint32_t x = seedStart++;
    struct timeval start, end;
    gettimeofday(&start, NULL);
    uint32_t s = 0;
    for (size_t i = 0; i < size && cnt--; i += 4096) { // Accessing in page size increments
        x = random_u32(x);
        size_t index = (x % (size / 4096)) * 4096;
        s += memory[index]; // Simple read
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for random memory read: %.6f seconds\n", elapsed);
    return s;
}
void *random_access_thread(void *arg) {
    //srand(time(NULL)); // Seed for random number generation
    ThreadArg *thread_arg = (ThreadArg *)arg;
    uint32_t s = 0;
    
    while (1) {
        s += perform_random_access(thread_arg->memory, thread_arg->size, thread_arg->count);
    }
    return (void*)s;
}
void perform_random_access(char *memory, size_t size, int cnt) {
    uint32_t x = seedStart++;
    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (size_t i = 0; i < size && cnt--; i += 4096) { // Accessing in page size increments
        x = random_u32(x);
        size_t index = (x % (size / 4096)) * 4096;
        memory[index] += 1; // Simple write operation to trigger CoW
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for random memory access: %.6f seconds\n", elapsed);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <memory_size_in_MB> <ratio_of_memory_to_access> <threads> \n", argv[0]);
        return 1;
    }
    int num_threads = 0;

    size_t memory_size = atoi(argv[1]) * MB;
    float perc = atof(argv[2]);
    char *memory = malloc(memory_size);
    size_t count = memory_size / 4096 * perc;
    num_threads = atoi(argv[3]);
    printf("memory size %lu, perc %f, count %lu\n", memory_size, perc, count);
    pthread_t threads[num_threads];
    ThreadArg thread_arg = {memory, memory_size, count};
    if (!memory) {
        perror("Failed to allocate memory");
        return 1;
    }
    struct timeval start, end;
    gettimeofday(&start, NULL);
    // memset(memory, 0, memory_size);
    for (size_t i = 0; i < memory_size; i += 4096) { // Accessing in page size increments
        size_t index = (rand() % (memory_size / 4096)) * 4096;
        memory[index] = 1; // Simple write operation
    }
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for memset(fault): %.6f seconds\n", elapsed);

    perform_random_access(memory, memory_size, count);

    gettimeofday(&start, NULL);
    pid_t pid = fork();
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Time taken for fork: %.6f seconds\n", elapsed);
    if (pid < 0) {
        perror("fork failed");
        free(memory);
        return 1;
    } else if (pid == 0) { // Child process
        sleep(30);
        _exit(0); // Child exits immediately
    } else { // Parent process
        
        for (int i = 0; i < num_threads; ++i) { // Random access after fork
            pthread_create(&threads[i], NULL, random_access_thread, &thread_arg);
        }
        // Random access before fork
        perform_random_access(memory, memory_size, count);

        printf("all completes\n");
        wait(NULL); // Wait for child to exit
    }

    free(memory);
    return 0;
}
