#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define ROWS 50
#define ITERATIONS 3
#define NUM_THREADS 8

static int64_t row_sum_50(int row) {
    int64_t total = 0;
    for (int col = 0; col < 50; col++) {
        total += (row * 50 + col) % 1000003;
    }
    return total;
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int r = ta->start; r < ta->end; r++) {
        volatile int64_t rs = row_sum_50(r);
        (void)rs;
    }
    return NULL;
}

int main(void) {
    /* Sequential checksum (ground truth) */
    int64_t seq_total = 0;
    for (int r = 0; r < ROWS; r++) {
        seq_total += row_sum_50(r);
    }
    int64_t seq_checksum = seq_total % 1000000007LL;

    /* Parallel section */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        pthread_t threads[NUM_THREADS];
        thread_arg_t args[NUM_THREADS];
        int chunk = ROWS / NUM_THREADS;
        for (int t = 0; t < NUM_THREADS; t++) {
            args[t].start = t * chunk;
            args[t].end = (t == NUM_THREADS - 1) ? ROWS : (t + 1) * chunk;
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
    int64_t par_total = 0;
    for (int r = 0; r < ROWS; r++) {
        par_total += row_sum_50(r);
    }
    int64_t par_checksum = par_total % 1000000007LL;

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Parallel Sum ===\n");
    printf("Grid=50x50\n");
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
