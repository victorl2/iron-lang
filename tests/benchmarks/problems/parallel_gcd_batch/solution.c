#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 10000000
#define NUM_THREADS 8

static int64_t gcd(int64_t a, int64_t b) {
    while (b > 0) {
        int64_t temp = b;
        b = a % b;
        a = temp;
    }
    return a;
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
        int64_t a = (i * 1000003 + 500009) % 999983 + 1;
        int64_t b = (i * 999979 + 100003) % 999979 + 1;
        sum += gcd(a, b);
    }
    c->partial_sum = sum;
    return NULL;
}

int main(void) {
    /* Correctness tests */
    printf("gcd(12, 8): %lld\n", (long long)gcd(12, 8));
    printf("gcd(100, 75): %lld\n", (long long)gcd(100, 75));
    printf("gcd(17, 13): %lld\n", (long long)gcd(17, 13));
    printf("gcd(1000000, 999999): %lld\n", (long long)gcd(1000000, 999999));

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
    printf("total gcd sum: %lld\n", (long long)total_sum);

    /* Sequential verification on small range */
    int64_t verify_sum = 0;
    for (int64_t i = 0; i < 1000; i++) {
        int64_t a = (i * 1000003 + 500009) % 999983 + 1;
        int64_t b = (i * 999979 + 100003) % 999979 + 1;
        verify_sum += gcd(a, b);
    }
    printf("verify sum [0,1000): %lld\n", (long long)verify_sum);

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

    printf("\n=== Benchmark: Parallel GCD Batch ===\n");
    printf("N: %d\n", N);
    printf("Threads: %d\n", NUM_THREADS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
