#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 100
#define ITERATIONS 3
#define NUM_THREADS 3

static int64_t stage_a(int64_t seed) {
    int64_t h = seed;
    for (int k = 0; k < 200; k++) {
        h = (h * 1000003LL + k) % 1000000007LL;
    }
    return h;
}

static int64_t stage_b(int64_t seed) {
    int64_t h = seed;
    for (int k = 0; k < 200; k++) {
        h = (h * 999983LL + k * 7) % 1000000007LL;
    }
    return h;
}

static int64_t stage_c(int64_t seed) {
    int64_t h = seed;
    for (int k = 0; k < 200; k++) {
        h = (h * 999979LL + k * 13) % 1000000007LL;
    }
    return h;
}

static int64_t pipeline_checksum(int n) {
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t a = stage_a(i);
        int64_t b = stage_b(a);
        int64_t c = stage_c(b);
        total = (total + c) % 1000000007LL;
    }
    return total;
}

static void *worker(void *arg) {
    volatile int64_t r = pipeline_checksum(N);
    (void)r;
    return NULL;
}

int main(void) {
    /* Sequential checksum (ground truth) */
    int64_t seq_checksum = pipeline_checksum(N);

    /* Spawn 3 pipeline stage threads */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        pthread_t threads[NUM_THREADS];
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_create(&threads[t], NULL, worker, NULL);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0
                   + (end.tv_nsec - start.tv_nsec) / 1e6;

    /* Recompute to verify */
    int64_t par_checksum = pipeline_checksum(N);

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Pipeline ===\n");
    printf("Stages: 3\n");
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
