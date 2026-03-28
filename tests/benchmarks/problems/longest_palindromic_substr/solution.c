#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t longest_palindrome(int64_t* arr, int n) {
    int64_t best = 1;

    for (int center = 0; center < n; center++) {
        int left = center, right = center;
        while (left >= 0 && right < n && arr[left] == arr[right]) {
            int length = right - left + 1;
            if (length > best) best = length;
            left--;
            right++;
        }

        left = center;
        right = center + 1;
        while (left >= 0 && right < n && arr[left] == arr[right]) {
            int length = right - left + 1;
            if (length > best) best = length;
            left--;
            right++;
        }
    }
    return best;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {2, 1, 2, 1, 4};
    printf("Test 1: %lld (expected 3)\n", longest_palindrome(t1, 5));

    int64_t t2[] = {3, 2, 2, 4};
    printf("Test 2: %lld (expected 2)\n", longest_palindrome(t2, 4));

    int64_t t3[] = {18, 1, 3, 5, 3, 1, 18};
    printf("Test 3: %lld (expected 7)\n", longest_palindrome(t3, 7));

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = (i * 7 + 3) % 26 + 1;
    printf("Test 4: %lld (expected 1)\n", longest_palindrome(bench, 100));

    long mem_before = get_memory_kb();
    int iterations = 5000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = longest_palindrome(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Longest Palindromic Substring ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
