#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

long level_order_checksum(int* vals, int size) {
    if (size == 0) return 0;
    int queue[64], q_front = 0, q_back = 0;
    queue[q_back++] = 0;
    long checksum = 0;
    int pos = 1;

    while (q_front < q_back) {
        int node = queue[q_front++];
        checksum += (long)vals[node] * pos;
        pos++;
        int lc = 2 * node + 1, rc = 2 * node + 2;
        if (lc < size) queue[q_back++] = lc;
        if (rc < size) queue[q_back++] = rc;
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

    printf("Test 1: %ld (expected 27349)\n", level_order_checksum(tree_vals, tree_size));

    int small[] = {2, 1, 3};
    printf("Test 2: %ld (expected 13)\n", level_order_checksum(small, 3));

    int single[] = {42};
    printf("Test 3: %ld (expected 42)\n", level_order_checksum(single, 1));

    int tree7[] = {10, 5, 15, 3, 7, 12, 20};
    printf("Test 4: %ld (expected 324)\n", level_order_checksum(tree7, 7));

    long mem_before = get_memory_kb();
    int iterations = 6000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile long result = 0;
    for (int it = 0; it < iterations; it++) {
        result = level_order_checksum(tree_vals, tree_size);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Level Order Traversal ===\n");
    printf("Tree size: %d\n", tree_size);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
