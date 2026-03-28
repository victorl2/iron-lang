#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t daily_temps(int64_t* temps, int n) {
    int result[200] = {0};
    int stack[200];
    int top = 0;

    for (int i = 0; i < n; i++) {
        while (top > 0 && temps[stack[top - 1]] < temps[i]) {
            top--;
            int idx = stack[top];
            result[idx] = i - idx;
        }
        stack[top++] = i;
    }

    int64_t checksum = 0;
    for (int i = 0; i < n; i++) checksum += result[i];
    return checksum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {73, 74, 75, 71, 69, 72, 76, 73};
    printf("Test 1: %lld (expected 10)\n", daily_temps(t1, 8));

    int64_t t2[] = {30, 40, 50, 60};
    printf("Test 2: %lld (expected 3)\n", daily_temps(t2, 4));

    int64_t t3[] = {30, 60, 90};
    printf("Test 3: %lld (expected 2)\n", daily_temps(t3, 3));

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = (i * 17 + 50) % 100 + 1;
    printf("Test 4: %lld (expected 225)\n", daily_temps(bench, 100));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = daily_temps(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Daily Temperatures ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
