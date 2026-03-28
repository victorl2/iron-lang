#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t classify(int64_t n) {
    if (n < 0) return 0;
    if (n == 0) return 1;
    if (n % 7 == 0) return 7;
    if (n % 5 == 0) return 5;
    if (n % 3 == 0) return 3;
    if (n % 2 == 0) return 2;
    return n % 100;
}

int64_t process(int64_t idx, int64_t depth) {
    if (depth <= 0) return idx % 997;
    int64_t v = (idx * 31 + 17) % 10007;
    if (v % 3 == 0) return v;
    if (v % 5 == 0) return v + 1;
    return v + process(v, depth - 1) % 1000;
}

int64_t bench_multi_exit(int64_t n) {
    int64_t checksum = 0;
    for (int64_t i = 0; i < n; i++) {
        checksum += classify(i);
        checksum += process(i * 73 + 29, 4) % 10000;
    }
    return checksum;
}

int main(void) {
    printf("Test 1 classify(0): %lld (expected 1)\n", classify(0));
    printf("Test 2 classify(14): %lld (expected 7)\n", classify(14));
    printf("Test 3 classify(10): %lld (expected 5)\n", classify(10));
    printf("Test 4 classify(9): %lld (expected 3)\n", classify(9));
    printf("Test 5 process(100,0): %lld (expected 100)\n", process(100, 0));
    printf("Test 6 bench(200): %lld\n", bench_multi_exit(200));

    int iterations = 200000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int i = 0; i < iterations; i++) {
        result = bench_multi_exit(200);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec)*1000.0 + (end.tv_nsec - start.tv_nsec)/1e6;
    printf("\n=== Benchmark: Multi-Exit Functions ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
