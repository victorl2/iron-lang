#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

typedef struct { int val; int list_idx; int elem_idx; } HeapNode;

void heap_down(HeapNode* h, int size, int i) {
    while (1) {
        int smallest = i;
        int l = 2*i + 1, r = 2*i + 2;
        if (l < size && h[l].val < h[smallest].val) smallest = l;
        if (r < size && h[r].val < h[smallest].val) smallest = r;
        if (smallest == i) break;
        HeapNode t = h[i]; h[i] = h[smallest]; h[smallest] = t;
        i = smallest;
    }
}

int mergeKSorted(int lists[10][10], int list_size, int k, int* output) {
    HeapNode heap[10];
    int heapSize = 0;

    for (int i = 0; i < k; i++) {
        heap[heapSize++] = (HeapNode){lists[i][0], i, 0};
    }
    for (int i = heapSize/2 - 1; i >= 0; i--) {
        heap_down(heap, heapSize, i);
    }

    int outIdx = 0;
    while (heapSize > 0) {
        HeapNode top = heap[0];
        output[outIdx++] = top.val;
        int li = top.list_idx;
        int ei = top.elem_idx + 1;
        if (ei < list_size) {
            heap[0] = (HeapNode){lists[li][ei], li, ei};
        } else {
            heap[0] = heap[--heapSize];
        }
        if (heapSize > 0) heap_down(heap, heapSize, 0);
    }
    return outIdx;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int k = 10, list_size = 10;
    int lists[10][10];
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < list_size; j++) {
            lists[i][j] = j * k + i;
        }
    }

    int output[100];
    int n = mergeKSorted(lists, list_size, k, output);

    int sorted = 1;
    for (int i = 1; i < n; i++) {
        if (output[i] < output[i-1]) { sorted = 0; break; }
    }
    printf("Merged %d elements, sorted: %s\n", n, sorted ? "yes" : "no");

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = mergeKSorted(lists, list_size, k, output);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Merge k Sorted Lists ===\n");
    printf("k=%d lists, %d elements each\n", k, list_size);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
