#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t pascal_row_sum(int64_t n) {
    int64_t row[31];
    row[0] = 1;
    for (int64_t k = 1; k <= n; k++) {
        row[k] = row[k - 1] * (n - k + 1) / k;
    }
    int64_t sum = 0;
    for (int64_t k = 0; k <= n; k++) {
        sum += row[k];
    }
    return sum;
}

int64_t pascal_row_checksum(int64_t n) {
    int64_t row[31];
    row[0] = 1;
    for (int64_t k = 1; k <= n; k++) {
        row[k] = row[k - 1] * (n - k + 1) / k;
    }
    int64_t sum = 0;
    for (int64_t k = 0; k <= n; k++) {
        sum += row[k] * (k + 1);
    }
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1 sum row 5: %lld (expected 32)\n", pascal_row_sum(5));
    printf("Test 2 sum row 10: %lld (expected 1024)\n", pascal_row_sum(10));
    printf("Test 3 sum row 20: %lld (expected 1048576)\n", pascal_row_sum(20));

    int64_t n = 30;
    printf("Bench check: %lld\n", pascal_row_checksum(n));

    long mem_before = get_memory_kb();
    int iterations = 500000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = pascal_row_checksum(n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Pascal Triangle ===\n");
    printf("Row: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
