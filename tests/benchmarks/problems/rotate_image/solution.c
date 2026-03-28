#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

void rotate_image(int64_t* mat, int64_t n) {
    // Transpose
    for (int64_t i = 0; i < n; i++) {
        for (int64_t j = i + 1; j < n; j++) {
            int64_t temp = mat[i * n + j];
            mat[i * n + j] = mat[j * n + i];
            mat[j * n + i] = temp;
        }
    }
    // Reverse each row
    for (int64_t i = 0; i < n; i++) {
        int64_t left = 0, right = n - 1;
        while (left < right) {
            int64_t temp = mat[i * n + left];
            mat[i * n + left] = mat[i * n + right];
            mat[i * n + right] = temp;
            left++;
            right--;
        }
    }
}

int64_t checksum(int64_t* mat, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n * n; i++) {
        sum += mat[i] * (i + 1);
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
    rotate_image(m1, 3);
    printf("Test 1: %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
           m1[0], m1[1], m1[2], m1[3], m1[4], m1[5], m1[6], m1[7], m1[8]);

    int64_t m2[] = {1,2,3,4};
    rotate_image(m2, 2);
    printf("Test 2: %lld %lld %lld %lld\n", m2[0], m2[1], m2[2], m2[3]);

    int64_t n = 20;
    int64_t total = n * n;
    int64_t orig[400], mat[400];
    for (int64_t i = 0; i < total; i++) {
        orig[i] = i + 1;
    }
    memcpy(mat, orig, sizeof(mat));
    rotate_image(mat, n);
    printf("Bench check: %lld\n", checksum(mat, n));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        memcpy(mat, orig, sizeof(mat));
        rotate_image(mat, n);
        result = mat[0];
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Rotate Image ===\n");
    printf("Matrix: %lldx%lld\n", n, n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
