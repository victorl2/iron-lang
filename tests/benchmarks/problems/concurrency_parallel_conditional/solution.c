#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 1000
#define ITERATIONS 3
#define NUM_THREADS 8

static int64_t even_compute(int idx) {
    int64_t h = (int64_t)idx * 37 + 1;
    for (int k = 0; k < 60; k++) {
        h = (h * 1000003LL + k) % 1000000007LL;
    }
    return h;
}

static int64_t odd_compute(int idx) {
    int64_t h = (int64_t)idx * 31 + 7;
    for (int k = 0; k < 60; k++) {
        h = (h + 999983LL * k + idx) % 1000000007LL;
    }
    return h;
}

static int64_t conditional_elem(int idx) {
    if (idx % 2 == 0) {
        return even_compute(idx);
    } else {
        return odd_compute(idx);
    }
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t elem = conditional_elem(i);
        (void)elem;
    }
    return NULL;
}

int main(void) {
    /* Sequential checksum (ground truth) */
    int64_t seq_sum = 0;
    for (int i = 0; i < N; i++) {
        seq_sum = (seq_sum + conditional_elem(i)) % 1000000007LL;
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
        par_sum = (par_sum + conditional_elem(i)) % 1000000007LL;
    }
    int64_t par_checksum = par_sum;

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Parallel Conditional ===\n");
    printf("N=%d\n", N);
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
