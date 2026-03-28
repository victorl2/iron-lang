#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define NUM_ITEMS 2000
#define CHUNK_SIZE 500
#define ITERATIONS 3
#define NUM_THREADS 8

static int64_t count_divisors(int64_t n) {
    int64_t count = 0;
    int64_t d = 1;
    while (d * d <= n) {
        if (n % d == 0) {
            count++;
            if (d != n / d) {
                count++;
            }
        }
        d++;
    }
    return count;
}

static int64_t heavy_work(int64_t idx) {
    int64_t base = idx * CHUNK_SIZE + 1;
    int64_t total = 0;
    for (int64_t k = 0; k < CHUNK_SIZE; k++) {
        total += count_divisors(base + k);
    }
    return total;
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t x = heavy_work(i);
        (void)x;
    }
    return NULL;
}

int main(void) {
    /* Correctness: compute checksum sequentially */
    int64_t checksum = 0;
    for (int i = 0; i < NUM_ITEMS; i++) {
        checksum += heavy_work(i);
    }
    printf("checksum: %lld\n", checksum);

    /* Benchmark: sequential */
    volatile int64_t seq_accum = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (int i = 0; i < NUM_ITEMS; i++) {
            seq_accum += heavy_work(i);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_seq = (end.tv_sec - start.tv_sec) * 1000.0
                       + (end.tv_nsec - start.tv_nsec) / 1e6;

    /* Benchmark: parallel */
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        pthread_t threads[NUM_THREADS];
        thread_arg_t args[NUM_THREADS];
        int chunk = NUM_ITEMS / NUM_THREADS;
        for (int t = 0; t < NUM_THREADS; t++) {
            args[t].start = t * chunk;
            args[t].end = (t == NUM_THREADS - 1) ? NUM_ITEMS : (t + 1) * chunk;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_par = (end.tv_sec - start.tv_sec) * 1000.0
                       + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Compute Intensive ===\n");
    printf("Work items: %d\n", NUM_ITEMS);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Sequential time: %.0f ms\n", elapsed_seq);
    printf("Parallel time: %.0f ms\n", elapsed_par);
    printf("Seq verify: %lld\n", (long long)seq_accum);
    printf("Total time: %.3f ms\n", elapsed_par);

    return 0;
}
