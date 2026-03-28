#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t spiral_sum(int64_t* mat, int64_t rows, int64_t cols) {
    int64_t top = 0, bottom = rows - 1, left = 0, right = cols - 1;
    int64_t sum = 0, pos = 1;

    while (top <= bottom && left <= right) {
        for (int64_t j = left; j <= right; j++) {
            sum += mat[top * cols + j] * pos++;
        }
        top++;

        for (int64_t i = top; i <= bottom; i++) {
            sum += mat[i * cols + right] * pos++;
        }
        right--;

        if (top <= bottom) {
            for (int64_t j = right; j >= left; j--) {
                sum += mat[bottom * cols + j] * pos++;
            }
            bottom--;
        }

        if (left <= right) {
            for (int64_t i = bottom; i >= top; i--) {
                sum += mat[i * cols + left] * pos++;
            }
            left++;
        }
    }
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t m1[] = {1,2,3,4,5,6,7,8,9};
    printf("Test 1: %lld (expected 257)\n", spiral_sum(m1, 3, 3));

    int64_t m2[] = {1,2,3,4,5,6};
    printf("Test 2: %lld (expected 87)\n", spiral_sum(m2, 2, 3));

    int64_t m3[] = {42};
    printf("Test 3: %lld (expected 42)\n", spiral_sum(m3, 1, 1));

    int64_t rows = 10, cols = 10;
    int64_t mat[100];
    for (int64_t i = 0; i < rows; i++)
        for (int64_t j = 0; j < cols; j++)
            mat[i * cols + j] = i * cols + j + 1;

    printf("Bench check: %lld\n", spiral_sum(mat, rows, cols));

    long mem_before = get_memory_kb();
    int iterations = 5000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = spiral_sum(mat, rows, cols);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Spiral Matrix ===\n");
    printf("Matrix: %lldx%lld\n", rows, cols);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
