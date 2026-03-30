#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t gcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b > 0) {
        int64_t temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

int64_t lcm(int64_t a, int64_t b) {
    int64_t g = gcd(a, b);
    if (g == 0) return 0;
    return (a / g) * b;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1 gcd: %lld (expected 4)\n", gcd(12, 8));
    printf("Test 1 lcm: %lld (expected 24)\n", lcm(12, 8));
    printf("Test 2 gcd: %lld (expected 25)\n", gcd(100, 75));
    printf("Test 2 lcm: %lld (expected 300)\n", lcm(100, 75));
    printf("Test 3 gcd: %lld (expected 1)\n", gcd(17, 13));
    printf("Test 3 lcm: %lld (expected 221)\n", lcm(17, 13));

    long mem_before = get_memory_kb();
    int iterations = 6000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int64_t sum_gcd = 0, sum_lcm = 0;
    for (int i = 0; i < iterations; i++) {
        int64_t a = (i * 13 + 7) % 10000 + 1;
        int64_t b = (i * 17 + 11) % 10000 + 1;
        sum_gcd += gcd(a, b);
        sum_lcm += lcm(a, b) % 100000;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("Bench gcd sum: %lld\n", sum_gcd);
    printf("Bench lcm sum mod: %lld\n", sum_lcm);

    printf("\n=== Benchmark: GCD/LCM ===\n");
    printf("Pairs: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
