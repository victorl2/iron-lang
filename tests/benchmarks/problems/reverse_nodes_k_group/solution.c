#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

/* Simulate linked list as array: next[i] = index of next node, val[i] = value.
   -1 means null. */

void reverseKGroup(int* vals, int* nxt, int head, int k, int* out_vals, int* out_count) {
    /* Count total nodes */
    int count = 0;
    int cur = head;
    while (cur != -1) { count++; cur = nxt[cur]; }

    int groups = count / k;
    /* Build output by collecting in order, reversing each k-group */
    int idx = 0;
    cur = head;
    for (int g = 0; g < groups; g++) {
        /* Collect k values, then reverse */
        int buf[100];
        for (int i = 0; i < k; i++) {
            buf[i] = vals[cur];
            cur = nxt[cur];
        }
        for (int i = k - 1; i >= 0; i--) {
            out_vals[idx++] = buf[i];
        }
    }
    /* Remaining nodes unchanged */
    while (cur != -1) {
        out_vals[idx++] = vals[cur];
        cur = nxt[cur];
    }
    *out_count = idx;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    /* List: 1->2->3->4->5->6->7->8, k=3 => 3->2->1->6->5->4->7->8 */
    int n = 50;
    int vals[50], nxt[50];
    for (int i = 0; i < n; i++) {
        vals[i] = i + 1;
        nxt[i] = (i + 1 < n) ? i + 1 : -1;
    }

    int out[50];
    int out_count;
    reverseKGroup(vals, nxt, 0, 3, out, &out_count);
    printf("First 12: ");
    for (int i = 0; i < 12 && i < out_count; i++) printf("%d ", out[i]);
    printf("\n");

    long mem_before = get_memory_kb();
    int iterations = 2500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        reverseKGroup(vals, nxt, 0, 3, out, &out_count);
        result = out_count;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Reverse Nodes in k-Group ===\n");
    printf("List size: %d, k=3\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
