#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define WIDTH 2000
#define HEIGHT 2000
#define MAX_ITER 100
#define ITERATIONS 3
#define NUM_THREADS 8

static int64_t mandelbrot_row_checksum(int64_t row) {
    /* Map row to y in [-15000, 15000] (fixed-point * 10000) */
    int64_t y0 = -15000 + row * 30000 / HEIGHT;
    int64_t row_sum = 0;
    for (int64_t col = 0; col < WIDTH; col++) {
        /* Map col to x in [-25000, 10000] */
        int64_t x0 = -25000 + col * 35000 / WIDTH;
        int64_t x = 0, y = 0;
        int64_t iter = 0;
        while (iter < MAX_ITER) {
            int64_t x2 = x * x / 10000;
            int64_t y2 = y * y / 10000;
            if (x2 + y2 > 40000) {
                iter = MAX_ITER + iter + 1;
            } else {
                int64_t xtemp = x2 - y2 + x0;
                y = 2 * x * y / 10000 + y0;
                x = xtemp;
                iter++;
            }
        }
        if (iter > MAX_ITER) {
            row_sum += (iter - MAX_ITER - 1);
        } else {
            row_sum += MAX_ITER;
        }
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
        volatile int64_t x = mandelbrot_row_checksum(i);
        (void)x;
    }
    return NULL;
}

int main(void) {
    /* Correctness: compute checksum */
    int64_t checksum = 0;
    for (int i = 0; i < HEIGHT; i++) {
        checksum += mandelbrot_row_checksum(i);
    }
    printf("checksum: %lld\n", checksum);

    /* Benchmark: sequential */
    volatile int64_t seq_total = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (int i = 0; i < HEIGHT; i++) {
            seq_total += mandelbrot_row_checksum(i);
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
        int chunk = HEIGHT / NUM_THREADS;
        for (int t = 0; t < NUM_THREADS; t++) {
            args[t].start = t * chunk;
            args[t].end = (t == NUM_THREADS - 1) ? HEIGHT : (t + 1) * chunk;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_par = (end.tv_sec - start.tv_sec) * 1000.0
                       + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Mandelbrot ===\n");
    printf("Grid: 2000x%d\n", HEIGHT);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Sequential time: %.0f ms\n", elapsed_seq);
    printf("Parallel time: %.0f ms\n", elapsed_par);
    printf("Seq verify: %lld\n", (long long)seq_total);
    printf("Total time: %.3f ms\n", elapsed_par);

    return 0;
}
