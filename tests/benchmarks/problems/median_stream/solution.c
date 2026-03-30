#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

void heap_up_max(int64_t* heap, int64_t idx) {
    int64_t i = idx;
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (heap[i] > heap[parent]) {
            int64_t t = heap[i]; heap[i] = heap[parent]; heap[parent] = t;
            i = parent;
        } else break;
    }
}

void heap_down_max(int64_t* heap, int64_t size, int64_t idx) {
    int64_t i = idx;
    while (1) {
        int64_t largest = i, l = 2*i+1, r = 2*i+2;
        if (l < size && heap[l] > heap[largest]) largest = l;
        if (r < size && heap[r] > heap[largest]) largest = r;
        if (largest == i) break;
        int64_t t = heap[i]; heap[i] = heap[largest]; heap[largest] = t;
        i = largest;
    }
}

void heap_up_min(int64_t* heap, int64_t idx) {
    int64_t i = idx;
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (heap[i] < heap[parent]) {
            int64_t t = heap[i]; heap[i] = heap[parent]; heap[parent] = t;
            i = parent;
        } else break;
    }
}

void heap_down_min(int64_t* heap, int64_t size, int64_t idx) {
    int64_t i = idx;
    while (1) {
        int64_t smallest = i, l = 2*i+1, r = 2*i+2;
        if (l < size && heap[l] < heap[smallest]) smallest = l;
        if (r < size && heap[r] < heap[smallest]) smallest = r;
        if (smallest == i) break;
        int64_t t = heap[i]; heap[i] = heap[smallest]; heap[smallest] = t;
        i = smallest;
    }
}

int64_t median_stream(int64_t* nums, int64_t n) {
    int64_t max_heap[100], min_heap[100];
    int64_t max_size = 0, min_size = 0;
    int64_t last_median = 0;

    for (int64_t i = 0; i < n; i++) {
        int64_t num = nums[i];

        if (max_size == 0) {
            max_heap[0] = num;
            max_size = 1;
        } else if (num <= max_heap[0]) {
            max_heap[max_size] = num;
            max_size++;
            heap_up_max(max_heap, max_size - 1);
        } else {
            min_heap[min_size] = num;
            min_size++;
            heap_up_min(min_heap, min_size - 1);
        }

        if (max_size > min_size + 1) {
            int64_t top = max_heap[0];
            max_size--;
            max_heap[0] = max_heap[max_size];
            heap_down_max(max_heap, max_size, 0);
            min_heap[min_size] = top;
            min_size++;
            heap_up_min(min_heap, min_size - 1);
        }
        if (min_size > max_size) {
            int64_t top = min_heap[0];
            min_size--;
            min_heap[0] = min_heap[min_size];
            heap_down_min(min_heap, min_size, 0);
            max_heap[max_size] = top;
            max_size++;
            heap_up_max(max_heap, max_size - 1);
        }

        if ((max_size + min_size) % 2 == 1) {
            last_median = max_heap[0];
        } else {
            last_median = (max_heap[0] + min_heap[0]) / 2;
        }
    }
    return last_median;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {2, 1, 5, 7, 2, 0, 5};
    printf("Test 1: %lld (expected 2)\n", median_stream(a1, 7));

    int64_t a2[] = {1, 2, 3, 4};
    printf("Test 2: %lld (expected 2)\n", median_stream(a2, 4));

    int64_t a3[] = {5, 15, 1, 3};
    printf("Test 3: %lld (expected 4)\n", median_stream(a3, 4));

    int64_t n = 100;
    int64_t nums[100];
    for (int64_t i = 0; i < n; i++) {
        nums[i] = (i * 31 + 17) % 200;
    }
    printf("Bench check: %lld\n", median_stream(nums, n));

    long mem_before = get_memory_kb();
    int64_t iterations = 400000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int64_t it = 0; it < iterations; it++) {
        nums[0] = (int64_t)((it * 7 + 13) % 200);
        result = median_stream(nums, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Median Stream ===\n");
    printf("Stream size: %lld\n", n);
    printf("Iterations: %lld\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
