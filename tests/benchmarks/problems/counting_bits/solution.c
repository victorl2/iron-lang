#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t counting_bits(int n) {
    int bits[1001] = {0};
    int64_t total = 0;
    for (int i = 1; i <= n; i++) {
        bits[i] = bits[i >> 1] + (i & 1);
        total += bits[i];
    }
    return total;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %lld (expected 2)\n", counting_bits(2));
    printf("Test 2: %lld (expected 7)\n", counting_bits(5));
    printf("Test 3: %lld (expected 17)\n", counting_bits(10));
    printf("Test 4: %lld (expected 4938)\n", counting_bits(1000));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = counting_bits(1000);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Counting Bits ===\n");
    printf("n=1000\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
