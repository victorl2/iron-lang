#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define NUM_BODIES 5000

static int64_t body_x(int64_t idx) { return ((idx * 7919 + 104729) % 10000) - 5000; }
static int64_t body_y(int64_t idx) { return ((idx * 6271 + 73856) % 10000) - 5000; }
static int64_t body_z(int64_t idx) { return ((idx * 3571 + 51427) % 10000) - 5000; }
static int64_t body_mass(int64_t idx) { return (idx * 31 + 17) % 100 + 1; }

static int64_t isqrt(int64_t n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    int64_t x = n;
    int64_t y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static int64_t compute_force_pair(int64_t i, int64_t j) {
    int64_t dx = body_x(j) - body_x(i);
    int64_t dy = body_y(j) - body_y(i);
    int64_t dz = body_z(j) - body_z(i);
    int64_t dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq < 1) return 0;
    int64_t dist = isqrt(dist_sq);
    if (dist < 1) return 0;
    int64_t mi = body_mass(i);
    int64_t mj = body_mass(j);
    int64_t force = mi * mj * 10000 / (dist_sq + 1);
    int64_t potential = mi * mj * 1000 / (dist + 1);
    return force + potential % 1000;
}

static int64_t force_for_body(int64_t i) {
    int64_t total = 0;
    for (int64_t j = 0; j < NUM_BODIES; j++) {
        if (j != i) total += compute_force_pair(i, j);
    }
    if (total == -1) printf("x\n");  /* prevent DCE */
    return total;
}

typedef struct { int start; int end; } thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t f = force_for_body(i);
        (void)f;
    }
    return NULL;
}

int main(void) {
    int64_t checksum = 0;
    for (int i = 0; i < NUM_BODIES; i++) {
        checksum += force_for_body(i) % 10000;
    }
    printf("Force checksum: %lld\n", checksum);
    printf("force(0): %lld\n", force_for_body(0));
    printf("force(1): %lld\n", force_for_body(1));
    printf("force(99): %lld\n", force_for_body(99));

    int iterations = 3;
    int num_threads = 8;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int iter = 0; iter < iterations; iter++) {
        pthread_t threads[8];
        thread_arg_t args[8];
        int chunk = (NUM_BODIES + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; t++) {
            args[t].start = t * chunk;
            args[t].end = (t + 1) * chunk;
            if (args[t].end > NUM_BODIES) args[t].end = NUM_BODIES;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < num_threads; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel N-Body Simulation ===\n");
    printf("Bodies: %d\n", NUM_BODIES);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
