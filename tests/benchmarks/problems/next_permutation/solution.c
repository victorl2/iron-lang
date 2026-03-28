#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t next_permutation(int64_t* arr, int64_t n) {
    int64_t i = n - 2;
    int64_t found = 0;
    while (i >= 0 && !found) {
        if (arr[i] < arr[i + 1]) {
            found = 1;
            int64_t j = n - 1;
            while (arr[j] <= arr[i]) j--;
            int64_t temp = arr[i]; arr[i] = arr[j]; arr[j] = temp;
            int64_t left = i + 1, right = n - 1;
            while (left < right) {
                temp = arr[left]; arr[left] = arr[right]; arr[right] = temp;
                left++; right--;
            }
        }
        i--;
    }
    if (!found) {
        int64_t left = 0, right = n - 1;
        while (left < right) {
            int64_t temp = arr[left]; arr[left] = arr[right]; arr[right] = temp;
            left++; right--;
        }
    }
    return found;
}

int64_t checksum(int64_t* arr, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum += arr[i] * (i + 1);
    }
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {1, 2, 3};
    next_permutation(a1, 3);
    printf("Test 1: %lld %lld %lld (expected 1 3 2)\n", a1[0], a1[1], a1[2]);

    int64_t a2[] = {3, 2, 1};
    next_permutation(a2, 3);
    printf("Test 2: %lld %lld %lld (expected 1 2 3)\n", a2[0], a2[1], a2[2]);

    int64_t a3[] = {1, 5, 1};
    next_permutation(a3, 3);
    printf("Test 3: %lld %lld %lld (expected 5 1 1)\n", a3[0], a3[1], a3[2]);

    int64_t n = 20;
    int64_t arr[20];
    for (int64_t i = 0; i < n; i++) arr[i] = i + 1;
    printf("Bench init checksum: %lld\n", checksum(arr, n));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result += next_permutation(arr, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("Bench after checksum: %lld\n", checksum(arr, n));

    printf("\n=== Benchmark: Next Permutation ===\n");
    printf("Array size: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
