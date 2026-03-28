#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define N 500000
#define NUM_THREADS 8
#define ITERATIONS 20

static int64_t complex_hash(int64_t input) {
    int64_t h = input;
    for (int round = 0; round < 16; round++) {
        h = h * 2654435761LL;
        int64_t shifted = h / 16;
        if (shifted < 0) shifted += 576460752303423488LL;
        h = h + shifted;
        h = h * 2246822519LL;
        shifted = h / 8192;
        if (shifted < 0) shifted += 1125899906842624LL;
        h = h + shifted;
    }
    if (h < 0) h = -h;
    return h;
}

typedef struct {
    int64_t lo;
    int64_t hi;
} chunk_t;

static void* worker(void* arg) {
    chunk_t* c = (chunk_t*)arg;
    for (int64_t i = c->lo; i < c->hi; i++) {
        volatile int64_t h = complex_hash(i);
        (void)h;
    }
    return NULL;
}

int main(void) {
    /* Sequential: compute checksum */
    int64_t checksum = 0;
    for (int64_t i = 0; i < N; i++) {
        int64_t h = complex_hash(i);
        int64_t bits = h % 1000000;
        checksum += bits;
    }
    printf("N: %d\n", N);
    printf("Hash checksum: %lld\n", checksum);

    /* Benchmark: parallel hash computation */
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

    printf("\n=== Benchmark: Parallel Hash Computation ===\n");
    printf("Elements: %d\n", N);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
