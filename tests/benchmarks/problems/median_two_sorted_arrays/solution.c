#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

__attribute__((noinline))
int32_t findMedianSortedArrays(int32_t* nums1, int32_t n1, int32_t* nums2, int32_t n2) {
    /* Returns median * 2 to avoid float issues */
    int32_t lo = 0, hi = n1;
    int32_t half = (n1 + n2 + 1) / 2;

    while (lo <= hi) {
        int32_t i = (lo + hi) / 2;
        int32_t j = half - i;

        int32_t left1  = (i > 0)  ? nums1[i - 1] : -1000000;
        int32_t right1 = (i < n1) ? nums1[i]     :  1000000;
        int32_t left2  = (j > 0)  ? nums2[j - 1] : -1000000;
        int32_t right2 = (j < n2) ? nums2[j]     :  1000000;

        if (left1 <= right2 && left2 <= right1) {
            int32_t maxLeft = left1 > left2 ? left1 : left2;
            int32_t minRight = right1 < right2 ? right1 : right2;
            if ((n1 + n2) % 2 == 1) {
                return maxLeft * 2;
            }
            return maxLeft + minRight;
        } else if (left1 > right2) {
            hi = i - 1;
        } else {
            lo = i + 1;
        }
    }
    return 0;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int32_t t1a[] = {1, 3};
    int32_t t1b[] = {2};
    int32_t t2a[] = {1, 2};
    int32_t t2b[] = {3, 4};

    int32_t r1 = findMedianSortedArrays(t1b, 1, t1a, 2);
    int32_t r2 = findMedianSortedArrays(t2a, 2, t2b, 2);
    printf("Test 1: %d.%d (expected 2.0)\n", r1 / 2, (r1 % 2) * 5);
    printf("Test 2: %d.%d (expected 2.5)\n", r2 / 2, (r2 % 2) * 5);

    int32_t a1[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98};
    int32_t a2[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73, 75, 77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 99};

    long mem_before = get_memory_kb();
    int iterations = 500000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int32_t result = 0;
    for (int i = 0; i < iterations; i++) {
        result = findMedianSortedArrays(a1, 50, a2, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Median of Two Sorted Arrays ===\n");
    printf("Array sizes: 50 + 50 = 100 elements\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %d.%d\n", result / 2, (result % 2) * 5);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
