#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 2000

static int64_t count_divisors(int64_t n) {
    int64_t count = 0;
    int64_t d = 1;
    while (d * d <= n) {
        if (n % d == 0) {
            count++;
            if (d != n / d) count++;
        }
        d++;
    }
    return count;
}

static int64_t heavy_reduce_work(int64_t idx) {
    int64_t base = idx * 500 + 1;
    int64_t total = 0;
    for (int64_t k = 0; k < 500; k++) {
        total += count_divisors(base + k);
    }
    if (total == -1) printf("x\n");  /* prevent DCE */
    return total;
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t x = heavy_reduce_work(i);
        (void)x;
    }
    return NULL;
}

int main(void) {
    /* Correctness */
    int64_t checksum = 0;
    for (int i = 0; i < N; i++) {
        checksum += heavy_reduce_work(i);
    }
    printf("Checksum: %lld\n", checksum);

    printf("work(0): %lld\n", heavy_reduce_work(0));
    printf("work(1): %lld\n", heavy_reduce_work(1));
    printf("work(999): %lld\n", heavy_reduce_work(999));

    /* Benchmark: parallel computation */
    int iterations = 3;
    int num_threads = 8;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int iter = 0; iter < iterations; iter++) {
        pthread_t threads[8];
        thread_arg_t args[8];
        int chunk = (N + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; t++) {
            args[t].start = t * chunk;
            args[t].end = (t + 1) * chunk;
            if (args[t].end > N) args[t].end = N;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < num_threads; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Reduce Sum ===\n");
    printf("N=%d\n", N);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
