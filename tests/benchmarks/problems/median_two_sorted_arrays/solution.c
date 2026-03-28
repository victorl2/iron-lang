#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

double findMedianSortedArrays(int* nums1, int n1, int* nums2, int n2) {
    if (n1 > n2) {
        return findMedianSortedArrays(nums2, n2, nums1, n1);
    }

    int lo = 0, hi = n1;
    int half = (n1 + n2 + 1) / 2;

    while (lo <= hi) {
        int i = (lo + hi) / 2;
        int j = half - i;

        int left1  = (i > 0)  ? nums1[i - 1] : -1000000;
        int right1 = (i < n1) ? nums1[i]     :  1000000;
        int left2  = (j > 0)  ? nums2[j - 1] : -1000000;
        int right2 = (j < n2) ? nums2[j]     :  1000000;

        if (left1 <= right2 && left2 <= right1) {
            int maxLeft = left1 > left2 ? left1 : left2;
            int minRight = right1 < right2 ? right1 : right2;
            if ((n1 + n2) % 2 == 1) {
                return (double)maxLeft;
            }
            return (maxLeft + minRight) / 2.0;
        } else if (left1 > right2) {
            hi = i - 1;
        } else {
            lo = i + 1;
        }
    }
    return 0.0;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int t1a[] = {1, 3};
    int t1b[] = {2};
    int t2a[] = {1, 2};
    int t2b[] = {3, 4};

    double r1 = findMedianSortedArrays(t1a, 2, t1b, 1);
    double r2 = findMedianSortedArrays(t2a, 2, t2b, 2);
    printf("Test 1: %.1f (expected 2.0)\n", r1);
    printf("Test 2: %.1f (expected 2.5)\n", r2);

    int a1[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98};
    int a2[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73, 75, 77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 99};

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile double result = 0;
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
    printf("Result: %.1f\n", result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
