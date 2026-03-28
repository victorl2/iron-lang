#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 200
#define ITERATIONS 20
#define NUM_THREADS 8

static inline int64_t mat_elem_a(int64_t row, int64_t col) {
    return ((row * N + col + 7) * 37 + 13) % 100;
}

static inline int64_t mat_elem_b(int64_t row, int64_t col) {
    return ((row * N + col + 42) * 37 + 13) % 100;
}

static int64_t compute_row_checksum(int64_t row) {
    int64_t row_sum = 0;
    for (int64_t j = 0; j < N; j++) {
        int64_t dot = 0;
        for (int64_t k = 0; k < N; k++) {
            dot += mat_elem_a(row, k) * mat_elem_b(k, j);
        }
        row_sum += dot;
    }
    return row_sum;
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t x = compute_row_checksum(i);
        (void)x;
    }
    return NULL;
}

int main(void) {
    /* Correctness: sequential checksum */
    int64_t checksum = 0;
    for (int i = 0; i < N; i++) {
        checksum += compute_row_checksum(i);
    }
    printf("checksum: %lld\n", checksum);

    /* Benchmark: sequential */
    volatile int64_t seq_total = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (int i = 0; i < N; i++) {
            seq_total += compute_row_checksum(i);
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
    double elapsed_par = (end.tv_sec - start.tv_sec) * 1000.0
                       + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Matrix Multiply ===\n");
    printf("Matrix size: %dx%d\n", N, N);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Sequential time: %.0f ms\n", elapsed_seq);
    printf("Parallel time: %.0f ms\n", elapsed_par);
    printf("Seq verify: %lld\n", (long long)seq_total);
    printf("Total time: %.3f ms\n", elapsed_par);

    return 0;
}
