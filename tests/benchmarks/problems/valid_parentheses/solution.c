#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int is_open(int c) {
    return c == 1 || c == 3 || c == 5;
}

static int matches(int open, int close) {
    if (open == 1 && close == 2) return 1;
    if (open == 3 && close == 4) return 1;
    if (open == 5 && close == 6) return 1;
    return 0;
}

static int64_t is_valid(int64_t* arr, int n) {
    int64_t stack[100];
    int top = 0;
    for (int i = 0; i < n; i++) {
        int64_t c = arr[i];
        if (is_open(c)) {
            stack[top++] = c;
        } else {
            if (top == 0) return 0;
            top--;
            if (!matches(stack[top], c)) return 0;
        }
    }
    return top == 0 ? 1 : 0;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {1, 2};
    printf("Test 1: %lld (expected 1)\n", is_valid(t1, 2));

    int64_t t2[] = {1, 2, 3, 4, 5, 6};
    printf("Test 2: %lld (expected 1)\n", is_valid(t2, 6));

    int64_t t3[] = {1, 4};
    printf("Test 3: %lld (expected 0)\n", is_valid(t3, 2));

    int64_t t4[] = {1, 5, 3, 4, 6, 2};
    printf("Test 4: %lld (expected 1)\n", is_valid(t4, 6));

    int64_t bench[50];
    for (int i = 0; i < 25; i++) bench[i] = 1;
    for (int i = 0; i < 25; i++) bench[25 + i] = 2;
    printf("Test 5: %lld (expected 1)\n", is_valid(bench, 50));

    long mem_before = get_memory_kb();
    int iterations = 3000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = is_valid(bench, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Valid Parentheses ===\n");
    printf("Sequence length: 50\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
