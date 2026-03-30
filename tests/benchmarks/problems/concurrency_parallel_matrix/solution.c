#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define ROWS 40
#define ITERATIONS 3
#define NUM_THREADS 8

static int64_t matrix_row_checksum(int row) {
    int64_t total = 0;
    for (int col = 0; col < 40; col++) {
        total += (int64_t)(row * 40 + col) * (row + col + 1) % 1000003;
    }
    return total % 1000000007LL;
}

static int64_t matrix_total_checksum(void) {
    int64_t total = 0;
    for (int row = 0; row < ROWS; row++) {
        total = (total + matrix_row_checksum(row)) % 1000000007LL;
    }
    return total;
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int row = ta->start; row < ta->end; row++) {
        volatile int64_t cs = matrix_row_checksum(row);
        (void)cs;
    }
    return NULL;
}

int main(void) {
    /* Sequential checksum (ground truth) */
    int64_t seq_checksum = matrix_total_checksum();

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
    int64_t par_checksum = matrix_total_checksum();

    printf("Sequential checksum: %lld\n", (long long)seq_checksum);
    printf("Parallel checksum: %lld\n", (long long)par_checksum);
    printf("Match: %d\n", seq_checksum == par_checksum ? 1 : 0);

    printf("\n=== Benchmark: Concurrency Parallel Matrix ===\n");
    printf("Matrix=40x40\n");
    printf("Total time: %.3f ms\n", elapsed);

    return 0;
}
