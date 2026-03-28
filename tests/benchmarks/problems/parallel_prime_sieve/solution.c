#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N_SEGMENTS 800
#define SEG_SIZE 5000
#define ITERATIONS 3
#define NUM_THREADS 8

static int64_t is_prime(int64_t n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0) return 0;
    if (n % 3 == 0) return 0;
    int64_t d = 5;
    while (d * d <= n) {
        if (n % d == 0) return 0;
        if (n % (d + 2) == 0) return 0;
        d += 6;
    }
    return 1;
}

static int64_t count_primes_half(int64_t seg_idx, int64_t start, int64_t count) {
    if (count <= 0) return 0;
    if (count == 1) {
        int64_t base = seg_idx * SEG_SIZE;
        return is_prime(base + start);
    }
    int64_t mid = count / 2;
    int64_t left = count_primes_half(seg_idx, start, mid);
    int64_t right = count_primes_half(seg_idx, start + mid, count - mid);
    return left + right;
}

static int64_t count_primes_in_segment(int64_t seg_idx) {
    return count_primes_half(seg_idx, 0, SEG_SIZE);
}

typedef struct {
    int start;
    int end;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t x = count_primes_in_segment(i);
        (void)x;
    }
    return NULL;
}

int main(void) {
    /* Correctness: sequential checksum */
    int64_t checksum = 0;
    for (int i = 0; i < N_SEGMENTS; i++) {
        checksum += count_primes_in_segment(i);
    }
    printf("checksum: %lld\n", checksum);

    /* Benchmark: sequential */
    volatile int64_t seq_total = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (int i = 0; i < N_SEGMENTS; i++) {
            seq_total += count_primes_in_segment(i);
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
        int chunk = N_SEGMENTS / NUM_THREADS;
        for (int t = 0; t < NUM_THREADS; t++) {
            args[t].start = t * chunk;
            args[t].end = (t == NUM_THREADS - 1) ? N_SEGMENTS : (t + 1) * chunk;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_par = (end.tv_sec - start.tv_sec) * 1000.0
                       + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Prime Sieve ===\n");
    printf("Segments: %d\n", N_SEGMENTS);
    printf("Range: [0, %d)\n", N_SEGMENTS * SEG_SIZE);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Sequential time: %.0f ms\n", elapsed_seq);
    printf("Parallel time: %.0f ms\n", elapsed_par);
    printf("Seq verify: %lld\n", (long long)seq_total);
    printf("Total time: %.3f ms\n", elapsed_par);

    return 0;
}
