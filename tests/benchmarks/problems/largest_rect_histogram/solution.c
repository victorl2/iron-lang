#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t largest_rect(int64_t* heights, int n) {
    int stack[200];
    int top = 0;
    int64_t max_area = 0;

    for (int i = 0; i <= n; i++) {
        int64_t h = (i < n) ? heights[i] : 0;

        while (top > 0 && heights[stack[top - 1]] > h) {
            top--;
            int64_t height = heights[stack[top]];
            int64_t width = (top == 0) ? i : i - stack[top - 1] - 1;
            int64_t area = height * width;
            if (area > max_area) max_area = area;
        }
        stack[top++] = i;
    }
    return max_area;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {2, 1, 5, 6, 2, 3};
    printf("Test 1: %lld (expected 10)\n", largest_rect(t1, 6));

    int64_t t2[] = {2, 4};
    printf("Test 2: %lld (expected 4)\n", largest_rect(t2, 2));

    int64_t t3[] = {6, 2, 5, 4, 5, 1, 6};
    printf("Test 3: %lld (expected 12)\n", largest_rect(t3, 7));

    int64_t bench[50];
    for (int i = 0; i < 50; i++) bench[i] = (i * 17 + 23) % 50 + 1;
    printf("Test 4: %lld (expected 252)\n", largest_rect(bench, 50));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = largest_rect(bench, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Largest Rectangle in Histogram ===\n");
    printf("Array size: 50\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
