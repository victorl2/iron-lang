#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t eval_rpn(int64_t* tokens, int n) {
    int64_t stack[100];
    int top = 0;

    for (int i = 0; i < n; i++) {
        int64_t t = tokens[i];
        if (t >= 0) {
            stack[top++] = t;
        } else {
            int64_t b = stack[--top];
            int64_t a = stack[--top];
            int64_t r = 0;
            if (t == -1) r = a + b;
            if (t == -2) r = a - b;
            if (t == -3) r = a * b;
            if (t == -4) r = a / b;
            stack[top++] = r;
        }
    }
    return stack[0];
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {2, 1, -1, 3, -3};
    printf("Test 1: %lld (expected 9)\n", eval_rpn(t1, 5));

    int64_t t2[] = {4, 13, 5, -4, -1};
    printf("Test 2: %lld (expected 6)\n", eval_rpn(t2, 5));

    int64_t t3[] = {3, 4, -1, 2, -3, 7, -4};
    printf("Test 3: %lld (expected 2)\n", eval_rpn(t3, 7));

    int64_t bench[] = {5, 3, -1, 2, -3, 4, -2, 6, -4, 7, -1, 8, -3, 3, -2, 2, -1, 9, -3, 1, -2};
    printf("Test 4: %lld (expected 638)\n", eval_rpn(bench, 21));

    long mem_before = get_memory_kb();
    int iterations = 4000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = eval_rpn(bench, 21);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Evaluate Reverse Polish Notation ===\n");
    printf("Tokens: 21\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
