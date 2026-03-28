#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define NUM_BLOCKS 10000
#define BLOCK_SIZE 300

static int64_t is_prime(int64_t n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0) return 0;
    if (n % 3 == 0) return 0;
    int64_t d = 5;
    while (d * d <= n) {
        if (n % d == 0) return 0;
        if (n % (d + 2) == 0) return 0;
        d += 6;
    }
    return 1;
}

static int64_t search_block(int64_t block_id) {
    int64_t base = block_id * BLOCK_SIZE + 1000000;
    int64_t prime_count = 0;
    int64_t hash = 0;
    for (int64_t i = 0; i < BLOCK_SIZE; i++) {
        int64_t num = base + i;
        int64_t p = is_prime(num);
        prime_count += p;
        if (p == 1) hash += num;
    }
    int64_t result = prime_count * 100000 + hash % 100000;
    if (result == -1) printf("x\n");  /* prevent DCE */
    return result;
}

typedef struct { int start; int end; } thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t h = search_block(i);
        (void)h;
    }
    return NULL;
}

int main(void) {
    int64_t checksum = 0;
    for (int b = 0; b < NUM_BLOCKS; b++) {
        checksum += search_block(b) % 100000;
    }
    printf("Search checksum: %lld\n", checksum);
    printf("block(0): %lld\n", search_block(0));
    printf("block(1): %lld\n", search_block(1));
    printf("block(999): %lld\n", search_block(999));

    int iterations = 3;
    int num_threads = 8;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int iter = 0; iter < iterations; iter++) {
        pthread_t threads[8];
        thread_arg_t args[8];
        int chunk = (NUM_BLOCKS + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; t++) {
            args[t].start = t * chunk;
            args[t].end = (t + 1) * chunk;
            if (args[t].end > NUM_BLOCKS) args[t].end = NUM_BLOCKS;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < num_threads; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel String Search ===\n");
    printf("Blocks: %d\n", NUM_BLOCKS);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
