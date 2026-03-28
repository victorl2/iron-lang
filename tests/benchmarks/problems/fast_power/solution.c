#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t fast_power(int64_t base, int64_t exp, int64_t mod) {
    int64_t result = 1;
    int64_t b = base % mod;
    int64_t e = exp;
    while (e > 0) {
        if (e % 2 == 1) {
            result = (result * b) % mod;
        }
        e /= 2;
        b = (b * b) % mod;
    }
    return result;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %lld (expected 24)\n", fast_power(2, 10, 1000));
    printf("Test 2: %lld (expected 1594323)\n", fast_power(3, 13, 1000000007));
    printf("Test 3: %lld (expected 699853951)\n", fast_power(7, 256, 1000000007));

    int64_t base = 3, exp = 1000000, mod = 1000000007;
    printf("Bench check: %lld\n", fast_power(base, exp, mod));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = fast_power(base, exp, mod);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Fast Power ===\n");
    printf("base=%lld, exp=%lld, mod=%lld\n", base, exp, mod);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
