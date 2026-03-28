#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t count_parens(int n) {
    int open_stack[500];
    int close_stack[500];
    int top = 0;
    int64_t count = 0;

    open_stack[top] = 0;
    close_stack[top] = 0;
    top++;

    while (top > 0) {
        top--;
        int open_c = open_stack[top];
        int close_c = close_stack[top];

        if (open_c == n && close_c == n) {
            count++;
        } else {
            if (close_c < open_c) {
                open_stack[top] = open_c;
                close_stack[top] = close_c + 1;
                top++;
            }
            if (open_c < n) {
                open_stack[top] = open_c + 1;
                close_stack[top] = close_c;
                top++;
            }
        }
    }
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %lld (expected 1)\n", count_parens(1));
    printf("Test 2: %lld (expected 5)\n", count_parens(3));
    printf("Test 3: %lld (expected 42)\n", count_parens(5));
    printf("Test 4: %lld (expected 208012)\n", count_parens(12));

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = count_parens(12);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Generate Parentheses ===\n");
    printf("n=12\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
