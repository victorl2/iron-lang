#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t max_profit(int64_t* prices, int64_t n) {
    int64_t min_price = prices[0];
    int64_t best = 0;
    for (int64_t i = 1; i < n; i++) {
        if (prices[i] < min_price) {
            min_price = prices[i];
        }
        int64_t profit = prices[i] - min_price;
        if (profit > best) {
            best = profit;
        }
    }
    return best;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {7,1,5,3,6,4};
    printf("Test 1: %lld (expected 5)\n", max_profit(a1, 6));

    int64_t a2[] = {7,6,4,3,1};
    printf("Test 2: %lld (expected 0)\n", max_profit(a2, 5));

    int64_t a3[] = {2,4,1,7,5,3,6,8};
    printf("Test 3: %lld (expected 7)\n", max_profit(a3, 8));

    /* Benchmark: 200 elements, prices[i] = (i*31 + 17) % 500 */
    int64_t bench[200];
    for (int i = 0; i < 200; i++) bench[i] = (i * 31 + 17) % 500;
    int64_t bench_result = max_profit(bench, 200);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 10000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = max_profit(bench, 200);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Buy Sell Stock ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
