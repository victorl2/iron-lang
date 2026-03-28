#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define NUM_ITEMS 150
#define ITERATIONS 1
#define NUM_THREADS 8

static int64_t fib(int64_t n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

static int64_t compute_fib_for_index(int64_t idx) {
    return fib(28 + idx % 6);
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t x = compute_fib_for_index(i);
        (void)x;
    }
    return NULL;
}

int main(void) {
    /* Correctness: sequential checksum */
    int64_t checksum = 0;
    for (int i = 0; i < NUM_ITEMS; i++) {
        checksum += compute_fib_for_index(i);
    }
    printf("checksum: %lld\n", checksum);

    /* Benchmark: sequential */
    volatile int64_t seq_total = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (int i = 0; i < NUM_ITEMS; i++) {
            seq_total += compute_fib_for_index(i);
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

    printf("\n=== Benchmark: Parallel Fibonacci ===\n");
    printf("Work items: %d\n", NUM_ITEMS);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Sequential time: %.0f ms\n", elapsed_seq);
    printf("Parallel time: %.0f ms\n", elapsed_par);
    printf("Seq verify: %lld\n", (long long)seq_total);
    printf("Total time: %.3f ms\n", elapsed_par);

    return 0;
}
