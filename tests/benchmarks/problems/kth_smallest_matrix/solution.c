#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

void heap_up(int64_t* heap, int64_t idx) {
    int64_t i = idx;
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (heap[i] < heap[parent]) {
            int64_t temp = heap[i];
            heap[i] = heap[parent];
            heap[parent] = temp;
            i = parent;
        } else {
            break;
        }
    }
}

void heap_down_min(int64_t* heap, int64_t size, int64_t idx) {
    int64_t i = idx;
    while (1) {
        int64_t smallest = i;
        int64_t l = 2 * i + 1;
        int64_t r = 2 * i + 2;
        if (l < size && heap[l] < heap[smallest]) smallest = l;
        if (r < size && heap[r] < heap[smallest]) smallest = r;
        if (smallest == i) break;
        int64_t temp = heap[i];
        heap[i] = heap[smallest];
        heap[smallest] = temp;
        i = smallest;
    }
}

int64_t kth_smallest(int64_t* mat, int64_t rows, int64_t cols, int64_t k) {
    int64_t heap[100];
    int64_t heap_size = 0;

    for (int64_t r = 0; r < rows; r++) {
        int64_t encoded = mat[r * cols + 0] * 10000 + r * 100 + 0;
        heap[heap_size] = encoded;
        heap_size++;
        heap_up(heap, heap_size - 1);
    }

    int64_t count = 0;
    int64_t result = 0;
    while (count < k) {
        int64_t top = heap[0];
        result = top / 10000;
        int64_t row = (top % 10000) / 100;
        int64_t col = top % 100;

        heap_size--;
        heap[0] = heap[heap_size];
        heap_down_min(heap, heap_size, 0);

        int64_t next_col = col + 1;
        if (next_col < cols) {
            int64_t encoded = mat[row * cols + next_col] * 10000 + row * 100 + next_col;
            heap[heap_size] = encoded;
            heap_size++;
            heap_up(heap, heap_size - 1);
        }
        count++;
    }
    return result;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t m1[] = {1, 5, 9, 10, 11, 13, 12, 13, 15};
    printf("Test 1: %lld (expected 13)\n", kth_smallest(m1, 3, 3, 8));

    int64_t m2[] = {1, 2, 3, 4};
    printf("Test 2: %lld (expected 3)\n", kth_smallest(m2, 2, 2, 3));

    int64_t rows = 10, cols = 10;
    int64_t mat[100];
    for (int64_t i = 0; i < rows; i++) {
        for (int64_t j = 0; j < cols; j++) {
            mat[i * cols + j] = i * 10 + j + 1;
        }
    }
    int64_t k = 50;
    printf("Bench check k=50: %lld\n", kth_smallest(mat, rows, cols, k));

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = kth_smallest(mat, rows, cols, k);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Kth Smallest in Matrix ===\n");
    printf("Matrix: %lldx%lld, k=%lld\n", rows, cols, k);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
