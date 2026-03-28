#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

void merge(int64_t* arr, int64_t* tmp, int64_t lo, int64_t mid, int64_t hi) {
    for (int64_t i = lo; i <= hi; i++) tmp[i] = arr[i];
    int64_t i = lo, j = mid + 1, k = lo;
    while (i <= mid && j <= hi) {
        if (tmp[i] <= tmp[j]) {
            arr[k] = tmp[i];
            i++;
        } else {
            arr[k] = tmp[j];
            j++;
        }
        k++;
    }
    while (i <= mid) {
        arr[k] = tmp[i];
        i++;
        k++;
    }
    while (j <= hi) {
        arr[k] = tmp[j];
        j++;
        k++;
    }
}

void merge_sort_rec(int64_t* arr, int64_t* tmp, int64_t lo, int64_t hi) {
    if (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        merge_sort_rec(arr, tmp, lo, mid);
        merge_sort_rec(arr, tmp, mid + 1, hi);
        merge(arr, tmp, lo, mid, hi);
    }
}

int64_t merge_sort_checksum(int64_t* orig, int64_t n) {
    int64_t arr[200], tmp[200];
    for (int64_t i = 0; i < n; i++) arr[i] = orig[i];
    merge_sort_rec(arr, tmp, 0, n - 1);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) sum += arr[i] * (i + 1);
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {3,6,8,10,1,2,1};
    printf("Test 1: %lld\n", merge_sort_checksum(a1, 7));

    int64_t a2[] = {5,4,3,2,1};
    printf("Test 2: %lld (expected 55)\n", merge_sort_checksum(a2, 5));

    int64_t a3[] = {1,1,1,1};
    printf("Test 3: %lld (expected 10)\n", merge_sort_checksum(a3, 4));

    /* Benchmark: 200 elements, arr[i] = (i*37+13) % 1000 */
    int64_t bench[200];
    for (int i = 0; i < 200; i++) bench[i] = (i * 37 + 13) % 1000;
    int64_t bench_result = merge_sort_checksum(bench, 200);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = merge_sort_checksum(bench, 200);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Merge Sort ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
