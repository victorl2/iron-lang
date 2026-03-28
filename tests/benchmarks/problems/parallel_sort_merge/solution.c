#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define NUM_CHUNKS 2000
#define CHUNK_SIZE 500

static int64_t count_divisors(int64_t n) {
    int64_t count = 0;
    int64_t d = 1;
    while (d * d <= n) {
        if (n % d == 0) {
            count++;
            if (d != n / d) count++;
        }
        d++;
    }
    return count;
}

static int64_t is_prime(int64_t n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0) return 0;
    int64_t d = 3;
    while (d * d <= n) {
        if (n % d == 0) return 0;
        d += 2;
    }
    return 1;
}

static int64_t verify_chunk(int64_t chunk_id) {
    int64_t base = chunk_id * CHUNK_SIZE + 100000;
    int64_t div_sum = 0;
    int64_t prime_count = 0;
    for (int64_t i = 0; i < CHUNK_SIZE; i++) {
        int64_t num = base + i;
        div_sum += count_divisors(num);
        prime_count += is_prime(num);
    }
    int64_t result = div_sum * 1000 + prime_count;
    if (result == -1) printf("x\n");  /* prevent DCE */
    return result;
}

typedef struct { int start; int end; } thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t h = verify_chunk(i);
        (void)h;
    }
    return NULL;
}

int main(void) {
    int64_t checksum = 0;
    for (int c = 0; c < NUM_CHUNKS; c++) {
        checksum += verify_chunk(c) % 100000;
    }
    printf("Verification checksum: %lld\n", checksum);
    printf("chunk(0): %lld\n", verify_chunk(0));
    printf("chunk(1): %lld\n", verify_chunk(1));
    printf("chunk(999): %lld\n", verify_chunk(999));

    int iterations = 3;
    int num_threads = 8;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int iter = 0; iter < iterations; iter++) {
        pthread_t threads[8];
        thread_arg_t args[8];
        int chunk = (NUM_CHUNKS + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; t++) {
            args[t].start = t * chunk;
            args[t].end = (t + 1) * chunk;
            if (args[t].end > NUM_CHUNKS) args[t].end = NUM_CHUNKS;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < num_threads; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Sort Merge ===\n");
    printf("Chunks: %d\n", NUM_CHUNKS);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
