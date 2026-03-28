#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t isqrt(int64_t n) {
    if (n < 2) return n;
    int64_t lo = 1, hi = n;
    if (hi > 1500000000LL) hi = 1500000000LL;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (mid <= n / mid) lo = mid + 1;
        else hi = mid - 1;
    }
    return hi;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %lld (expected 44721)\n", isqrt(2000000000LL));
    printf("Test 2: %lld (expected 10)\n", isqrt(100));
    printf("Test 3: %lld (expected 1)\n", isqrt(1));
    printf("Test 4: %lld (expected 0)\n", isqrt(0));
    printf("Test 5: %lld (expected 5)\n", isqrt(26));

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = isqrt(2000000000LL);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Integer Square Root ===\n");
    printf("n=2000000000\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
