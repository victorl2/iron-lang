#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t gas_station(int64_t* gas, int64_t* cost, int64_t n) {
    int64_t total_tank = 0, curr_tank = 0, start = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t diff = gas[i] - cost[i];
        total_tank += diff;
        curr_tank += diff;
        if (curr_tank < 0) {
            start = i + 1;
            curr_tank = 0;
        }
    }
    return total_tank >= 0 ? start : -1;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t g1[] = {1, 2, 3, 4, 5};
    int64_t c1[] = {3, 4, 5, 1, 2};
    printf("Test 1: %lld (expected 3)\n", gas_station(g1, c1, 5));

    int64_t g2[] = {2, 3, 4};
    int64_t c2[] = {3, 4, 3};
    printf("Test 2: %lld (expected -1)\n", gas_station(g2, c2, 3));

    int64_t g3[] = {5, 1, 2, 3, 4};
    int64_t c3[] = {4, 4, 1, 5, 1};
    printf("Test 3: %lld (expected 4)\n", gas_station(g3, c3, 5));

    int64_t n = 50;
    int64_t gas[50], cost[50];
    for (int64_t i = 0; i < n; i++) {
        gas[i] = (i * 13 + 7) % 20 + 1;
        cost[i] = (i * 11 + 3) % 20 + 1;
    }
    printf("Bench check: %lld\n", gas_station(gas, cost, n));

    long mem_before = get_memory_kb();
    int iterations = 5000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = gas_station(gas, cost, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Gas Station ===\n");
    printf("Stations: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
