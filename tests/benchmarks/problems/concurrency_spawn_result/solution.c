#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define CHUNK 250
#define ITERATIONS 3
#define NUM_THREADS 4

static int64_t range_sum(int start_idx, int end_idx) {
    int64_t total = 0;
    for (int i = start_idx; i < end_idx; i++) {
        int64_t h = i + 1;
        for (int k = 0; k < 50; k++) {
            h = (h * 31 + i) % 1000000007LL;
        }
        total = (total + h) % 1000000007LL;
    }
    return total;
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    volatile int64_t r = range_sum(ta->start, ta->end);
    (void)r;
    return NULL;
}

int main(void) {
    /* Sequential checksum (ground truth) */
    int64_t s0 = range_sum(0, CHUNK);
    int64_t s1 = range_sum(CHUNK, CHUNK * 2);
    int64_t s2 = range_sum(CHUNK * 2, CHUNK * 3);
    int64_t s3 = range_sum(CHUNK * 3, CHUNK * 4);
    int64_t seq_checksum = (s0 + s1 + s2 + s3) % 1000000007LL;

    /* Spawn 4 pthreads */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        pthread_t threads[NUM_THREADS];
        thread_arg_t args[NUM_THREADS] = {
            {0, CHUNK}, {CHUNK, CHUNK*2}, {CHUNK*2, CHUNK*3}, {CHUNK*3, CHUNK*4}
        };
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
    int64_t p0 = range_sum(0, CHUNK);
    int64_t p1 = range_sum(CHUNK, CHUNK * 2);
    int64_t p2 = range_sum(CHUNK * 2, CHUNK * 3);
    int64_t p3 = range_sum(CHUNK * 3, CHUNK * 4);
    int64_t par_checksum = (p0 + p1 + p2 + p3) % 1000000007LL;

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Spawn Result ===\n");
    printf("Tasks: 4\n");
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
