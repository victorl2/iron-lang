#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

int diameter(int* vals, int size) {
    if (size == 0) return 0;
    int height[64];
    int max_diam = 0;

    for (int i = size - 1; i >= 0; i--) {
        int lc = 2 * i + 1, rc = 2 * i + 2;
        int lh = (lc < size) ? height[lc] : 0;
        int rh = (rc < size) ? height[rc] : 0;
        int d = lh + rh;
        if (d > max_diam) max_diam = d;
        height[i] = 1 + (lh > rh ? lh : rh);
    }
    return max_diam;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int tree_vals[] = {50, 25, 75, 12, 37, 62, 87, 6, 18, 31, 43, 56, 68, 81, 93,
                       3, 9, 15, 21, 28, 34, 40, 46, 53, 59, 65, 71, 78, 84, 90, 96};
    int tree_size = 31;

    printf("Test 1: %d (expected 8)\n", diameter(tree_vals, tree_size));

    int small[] = {2, 1, 3};
    printf("Test 2: %d (expected 2)\n", diameter(small, 3));

    int single[] = {42};
    printf("Test 3: %d (expected 0)\n", diameter(single, 1));

    int tree7[] = {10, 5, 15, 3, 7, 12, 20};
    printf("Test 4: %d (expected 4)\n", diameter(tree7, 7));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = diameter(tree_vals, tree_size);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Binary Tree Diameter ===\n");
    printf("Tree size: %d\n", tree_size);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
