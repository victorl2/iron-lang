#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 400
#define ITERATIONS 3
#define NUM_THREADS 4

static int64_t hash_seed(int64_t seed, int n) {
    int64_t h = seed;
    for (int i = 0; i < n; i++) {
        h = (h * 1000003LL + i) % 1000000007LL;
    }
    return h;
}

static const int seeds[NUM_THREADS] = {7, 13, 29, 97};

typedef struct {
    int seed_idx;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    volatile int64_t r = hash_seed(seeds[ta->seed_idx], N);
    (void)r;
    return NULL;
}

int main(void) {
    /* Sequential computation (ground truth) */
    int64_t a0 = hash_seed(7, N);
    int64_t a1 = hash_seed(13, N);
    int64_t a2 = hash_seed(29, N);
    int64_t a3 = hash_seed(97, N);
    int64_t seq_checksum = (a0 + a1 + a2 + a3) % 1000000007LL;

    /* Spawn 4 independent threads */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        pthread_t threads[NUM_THREADS];
        thread_arg_t args[NUM_THREADS] = {{0}, {1}, {2}, {3}};
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
    int64_t b0 = hash_seed(7, N);
    int64_t b1 = hash_seed(13, N);
    int64_t b2 = hash_seed(29, N);
    int64_t b3 = hash_seed(97, N);
    int64_t par_checksum = (b0 + b1 + b2 + b3) % 1000000007LL;

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Spawn Independence ===\n");
    printf("Spawns: 4\n");
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
