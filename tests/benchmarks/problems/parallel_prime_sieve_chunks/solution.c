#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define N 100000
#define NUM_THREADS 8
#define ITERATIONS 500

static int is_prime(int64_t n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0) return 0;
    if (n % 3 == 0) return 0;
    for (int64_t d = 5; d * d <= n; d += 6) {
        if (n % d == 0) return 0;
        if (n % (d + 2) == 0) return 0;
    }
    return 1;
}

typedef struct {
    int64_t lo;
    int64_t hi;
} chunk_t;

static void* worker(void* arg) {
    chunk_t* c = (chunk_t*)arg;
    for (int64_t i = c->lo; i < c->hi; i++) {
        volatile int r = is_prime(i + 2);
        (void)r;
    }
    return NULL;
}

int main(void) {
    /* Sequential: compute prime count and checksum */
    int64_t prime_count = 0;
    int64_t checksum = 0;
    for (int64_t i = 0; i < N; i++) {
        int64_t num = i + 2;
        int p = is_prime(num);
        prime_count += p;
        if (p) checksum += num;
    }
    printf("Prime count [2, %lld): %lld\n", (long long)(N + 2), prime_count);
    printf("Checksum: %lld\n", checksum);

    /* Benchmark: parallel primality test */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int it = 0; it < ITERATIONS; it++) {
        pthread_t threads[NUM_THREADS];
        chunk_t chunks[NUM_THREADS];
        int64_t chunk_size = N / NUM_THREADS;

        for (int t = 0; t < NUM_THREADS; t++) {
            chunks[t].lo = t * chunk_size;
            chunks[t].hi = (t == NUM_THREADS - 1) ? N : (t + 1) * chunk_size;
            pthread_create(&threads[t], NULL, worker, &chunks[t]);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Prime Sieve ===\n");
    printf("Range: [2, %d)\n", N + 2);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
