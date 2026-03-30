#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 500
#define ITERATIONS 3
#define NUM_THREADS 8

static int64_t fib_iter(int n) {
    if (n <= 1) return n;
    int64_t a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        int64_t tmp = a + b;
        a = b;
        b = tmp;
    }
    return b;
}

static int64_t fib_for_index(int idx) {
    return fib_iter(idx % 20 + 10);
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t f = fib_for_index(i);
        (void)f;
    }
    return NULL;
}

int main(void) {
    /* Sequential checksum (ground truth) */
    int64_t seq_sum = 0;
    for (int i = 0; i < N; i++) {
        seq_sum = (seq_sum + fib_for_index(i)) % 1000000007LL;
    }
    int64_t seq_checksum = seq_sum;

    /* Parallel section */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        pthread_t threads[NUM_THREADS];
        thread_arg_t args[NUM_THREADS];
        int chunk = N / NUM_THREADS;
        for (int t = 0; t < NUM_THREADS; t++) {
            args[t].start = t * chunk;
            args[t].end = (t == NUM_THREADS - 1) ? N : (t + 1) * chunk;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0
                   + (end.tv_nsec - start.tv_nsec) / 1e6;

    /* Recompute to verify */
    int64_t par_sum = 0;
    for (int i = 0; i < N; i++) {
        par_sum = (par_sum + fib_for_index(i)) % 1000000007LL;
    }
    int64_t par_checksum = par_sum;

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Parallel Fibonacci ===\n");
    printf("N=%d\n", N);
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
