#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

int max_depth(int* vals, int size) {
    if (size == 0) return 0;
    int stack_node[64], stack_depth[64];
    int top = 0;
    stack_node[top] = 0;
    stack_depth[top] = 1;
    top++;
    int max_d = 0;

    while (top > 0) {
        top--;
        int node = stack_node[top];
        int depth = stack_depth[top];
        if (depth > max_d) max_d = depth;

        int lc = 2 * node + 1, rc = 2 * node + 2;
        if (lc < size) {
            stack_node[top] = lc;
            stack_depth[top] = depth + 1;
            top++;
        }
        if (rc < size) {
            stack_node[top] = rc;
            stack_depth[top] = depth + 1;
            top++;
        }
    }
    return max_d;
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

    printf("Test 1: %d (expected 5)\n", max_depth(tree_vals, tree_size));

    int small[] = {2, 1, 3};
    printf("Test 2: %d (expected 2)\n", max_depth(small, 3));

    int single[] = {42};
    printf("Test 3: %d (expected 1)\n", max_depth(single, 1));

    int tree7[] = {10, 5, 15, 3, 7, 12, 20};
    printf("Test 4: %d (expected 3)\n", max_depth(tree7, 7));

    long mem_before = get_memory_kb();
    int iterations = 3000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = max_depth(tree_vals, tree_size);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Max Depth Binary Tree ===\n");
    printf("Tree size: %d\n", tree_size);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
