#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 5000000
#define DEGREE 20
#define NUM_THREADS 8

/* Fixed coefficients for the polynomial */
static const int64_t COEFFS[DEGREE + 1] = {
    3, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41,
    43, 47, 53, 59, 61, 67, 71, 73, 79, 83
};

/* Evaluate polynomial at x using Horner's method, all mod large prime */
static int64_t eval_poly(int64_t x) {
    int64_t result = COEFFS[DEGREE];
    for (int d = DEGREE - 1; d >= 0; d--) {
        result = (result * x + COEFFS[d]) % 1000000007;
    }
    /* Ensure non-negative */
    if (result < 0) result += 1000000007;
    return result;
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
        int64_t x = (i * 31 + 7) % 100003;
        sum = (sum + eval_poly(x)) % 1000000007;
    }
    c->partial_sum = sum;
    return NULL;
}

int main(void) {
    /* Correctness tests */
    printf("poly(0): %lld\n", (long long)eval_poly(0));
    printf("poly(1): %lld\n", (long long)eval_poly(1));
    printf("poly(2): %lld\n", (long long)eval_poly(2));
    printf("poly(100): %lld\n", (long long)eval_poly(100));

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
        total_sum = (total_sum + chunks[t].partial_sum) % 1000000007;
    }

    printf("N: %d\n", N);
    printf("degree: %d\n", DEGREE);
    printf("total eval sum: %lld\n", (long long)total_sum);

    /* Sequential verification on small range */
    int64_t verify_sum = 0;
    for (int64_t i = 0; i < 1000; i++) {
        int64_t x = (i * 31 + 7) % 100003;
        verify_sum = (verify_sum + eval_poly(x)) % 1000000007;
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

    printf("\n=== Benchmark: Parallel Polynomial Eval ===\n");
    printf("N: %d\n", N);
    printf("Degree: %d\n", DEGREE);
    printf("Threads: %d\n", NUM_THREADS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
