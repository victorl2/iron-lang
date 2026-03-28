#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define NUM_CHUNKS 64
#define POINTS_PER_CHUNK 10000
#define NUM_THREADS 8
#define ITERATIONS 200

static int64_t monte_carlo_chunk(int64_t chunk_id, int64_t num_points) {
    /* Use modular PRNG to avoid signed overflow UB across platforms */
    int64_t seed = (chunk_id * 6364136223LL + 1) % 1000000007;
    if (seed < 0) seed += 1000000007;
    int64_t hits = 0;
    for (int64_t j = 0; j < num_points; j++) {
        seed = (seed * 48271LL) % 2147483647LL; /* Park-Miller LCG */
        int64_t x = seed % 1000000;

        seed = (seed * 48271LL) % 2147483647LL;
        int64_t y = seed % 1000000;

        if (x * x + y * y <= 1000000LL * 1000000LL) {
            hits++;
        }
    }
    return hits;
}

typedef struct {
    int64_t lo;
    int64_t hi;
    int64_t points_per_chunk;
} work_t;

static void* worker(void* arg) {
    work_t* w = (work_t*)arg;
    for (int64_t i = w->lo; i < w->hi; i++) {
        volatile int64_t r = monte_carlo_chunk(i, w->points_per_chunk);
        (void)r;
    }
    return NULL;
}

int main(void) {
    /* Sequential: compute total hits */
    int64_t total_hits = 0;
    for (int64_t i = 0; i < NUM_CHUNKS; i++) {
        total_hits += monte_carlo_chunk(i, POINTS_PER_CHUNK);
    }
    int64_t total_points = (int64_t)NUM_CHUNKS * POINTS_PER_CHUNK;
    int64_t pi_est = 4 * total_hits * 10000 / total_points;
    printf("Total hits: %lld\n", total_hits);
    printf("Total points: %lld\n", total_points);
    printf("Pi estimate x10000: %lld\n", pi_est);

    /* Benchmark: parallel monte carlo */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int it = 0; it < ITERATIONS; it++) {
        pthread_t threads[NUM_THREADS];
        work_t works[NUM_THREADS];
        int64_t chunk_size = NUM_CHUNKS / NUM_THREADS;

        for (int t = 0; t < NUM_THREADS; t++) {
            works[t].lo = t * chunk_size;
            works[t].hi = (t == NUM_THREADS - 1) ? NUM_CHUNKS : (t + 1) * chunk_size;
            works[t].points_per_chunk = POINTS_PER_CHUNK;
            pthread_create(&threads[t], NULL, worker, &works[t]);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Pi Estimation ===\n");
    printf("Chunks: %d\n", NUM_CHUNKS);
    printf("Points per chunk: %d\n", POINTS_PER_CHUNK);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
