#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t is_palindrome(int64_t* arr, int n) {
    int left = 0, right = n - 1;
    while (left < right) {
        if (arr[left] != arr[right]) return 0;
        left++;
        right--;
    }
    return 1;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {18, 1, 3, 5, 3, 1, 18};
    printf("Test 1: %lld (expected 1)\n", is_palindrome(t1, 7));

    int64_t t2[] = {8, 5, 12, 12, 15};
    printf("Test 2: %lld (expected 0)\n", is_palindrome(t2, 5));

    int64_t t3[] = {1, 2, 2, 1};
    printf("Test 3: %lld (expected 1)\n", is_palindrome(t3, 4));

    int64_t t4[] = {5};
    printf("Test 4: %lld (expected 1)\n", is_palindrome(t4, 1));

    int64_t bench[100];
    for (int i = 0; i < 50; i++) {
        int v = i % 26 + 1;
        bench[i] = v;
        bench[99 - i] = v;
    }
    printf("Test 5: %lld (expected 1)\n", is_palindrome(bench, 100));

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = is_palindrome(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Palindrome Check ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
