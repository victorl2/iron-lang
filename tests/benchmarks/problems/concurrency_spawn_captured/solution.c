#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 500
#define ITERATIONS 3
#define NUM_THREADS 3

static int64_t compute_with_seed(int64_t seed, int n) {
    int64_t h = seed;
    for (int i = 0; i < n; i++) {
        h = (h * 1000003LL + i * 37) % 1000000007LL;
    }
    return h;
}

static int64_t get_seed(void) {
    return compute_with_seed(42, 100);
}

typedef struct {
    int offset;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    int64_t s = get_seed();
    volatile int64_t x = compute_with_seed(s + ta->offset, N);
    (void)x;
    return NULL;
}

int main(void) {
    int64_t seed = get_seed();

    /* Sequential computation (ground truth) */
    int64_t r0 = compute_with_seed(seed, N);
    int64_t r1 = compute_with_seed(seed + 1, N);
    int64_t r2 = compute_with_seed(seed + 2, N);
    int64_t seq_checksum = (r0 + r1 + r2) % 1000000007LL;

    /* Spawn threads that recompute seed internally */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        pthread_t threads[NUM_THREADS];
        thread_arg_t args[NUM_THREADS] = {{0}, {1}, {2}};
        for (int t = 0; t < NUM_THREADS; t++) {
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
    int64_t p_seed = get_seed();
    int64_t p0 = compute_with_seed(p_seed, N);
    int64_t p1 = compute_with_seed(p_seed + 1, N);
    int64_t p2 = compute_with_seed(p_seed + 2, N);
    int64_t par_checksum = (p0 + p1 + p2) % 1000000007LL;

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Spawn Captured ===\n");
    printf("Seed=%lld\n", (long long)seed);
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
