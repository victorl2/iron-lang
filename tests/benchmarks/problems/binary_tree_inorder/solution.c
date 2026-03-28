#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

long inorder_checksum(int* vals, int size) {
    int stack[64], top = 0;
    int curr = 0, has_curr = 1;
    long checksum = 0;
    int order = 1;

    while (1) {
        while (has_curr) {
            stack[top++] = curr;
            int lc = 2 * curr + 1;
            if (lc < size) curr = lc;
            else has_curr = 0;
        }
        if (top == 0) break;
        curr = stack[--top];
        checksum += (long)vals[curr] * order;
        order++;
        int rc = 2 * curr + 2;
        if (rc < size) { curr = rc; has_curr = 1; }
        else has_curr = 0;
    }
    return checksum;
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

    printf("Test 1: %ld (expected 32312)\n", inorder_checksum(tree_vals, tree_size));

    int small[] = {2, 1, 3};
    printf("Test 2: %ld (expected 14)\n", inorder_checksum(small, 3));

    int single[] = {42};
    printf("Test 3: %ld (expected 42)\n", inorder_checksum(single, 1));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile long result = 0;
    for (int it = 0; it < iterations; it++) {
        result = inorder_checksum(tree_vals, tree_size);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Binary Tree Inorder ===\n");
    printf("Tree size: %d\n", tree_size);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
