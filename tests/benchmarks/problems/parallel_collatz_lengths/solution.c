#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

#define N 500000
#define NUM_THREADS 8

static int64_t collatz_length(int64_t n) {
    int64_t steps = 0;
    while (n != 1) {
        if (n % 2 == 0) {
            n = n / 2;
        } else {
            n = 3 * n + 1;
        }
        steps++;
    }
    return steps;
}

typedef struct {
    int64_t lo;
    int64_t hi;
    int64_t partial_sum;
} chunk_t;

static void *worker(void *arg) {
    chunk_t *c = (chunk_t *)arg;
    int64_t sum = 0;
    for (int64_t i = c->lo; i < c->hi; i++) {
        sum += collatz_length(i + 1);
    }
    c->partial_sum = sum;
    return NULL;
}

int main(void) {
    /* Correctness tests */
    printf("collatz(1): %lld\n", (long long)collatz_length(1));
    printf("collatz(2): %lld\n", (long long)collatz_length(2));
    printf("collatz(3): %lld\n", (long long)collatz_length(3));
    printf("collatz(6): %lld\n", (long long)collatz_length(6));
    printf("collatz(27): %lld\n", (long long)collatz_length(27));

    /* Parallel computation */
    chunk_t chunks[NUM_THREADS];
    pthread_t threads[NUM_THREADS];
    int64_t chunk_size = N / NUM_THREADS;

    for (int t = 0; t < NUM_THREADS; t++) {
        chunks[t].lo = t * chunk_size;
        chunks[t].hi = (t == NUM_THREADS - 1) ? N : (t + 1) * chunk_size;
        pthread_create(&threads[t], NULL, worker, &chunks[t]);
    }

    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    int64_t total_sum = 0;
    for (int t = 0; t < NUM_THREADS; t++) {
        total_sum += chunks[t].partial_sum;
    }

    printf("N: %d\n", N);
    printf("total collatz sum: %lld\n", (long long)total_sum);

    /* Verification: compute max collatz length in [1, 1000] */
    int64_t max_len = 0;
    int64_t max_n = 0;
    for (int64_t i = 1; i <= 1000; i++) {
        int64_t len = collatz_length(i);
        if (len > max_len) {
            max_len = len;
            max_n = i;
        }
    }
    printf("max collatz in [1,1000]: n=%lld len=%lld\n", (long long)max_n, (long long)max_len);

    /* Benchmark timing */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int t = 0; t < NUM_THREADS; t++) {
        chunks[t].lo = t * chunk_size;
        chunks[t].hi = (t == NUM_THREADS - 1) ? N : (t + 1) * chunk_size;
        pthread_create(&threads[t], NULL, worker, &chunks[t]);
    }
    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Collatz Lengths ===\n");
    printf("N: %d\n", N);
    printf("Threads: %d\n", NUM_THREADS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
