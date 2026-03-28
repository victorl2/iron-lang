#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t min_stack_ops(int n) {
    int64_t data[2000];
    int64_t mins[2000];
    int top = 0;
    int64_t checksum = 0;

    for (int i = 0; i < n; i++) {
        int r = (i * 13 + 7) % 10;
        if (r < 5 || top < 3) {
            int64_t v = (i * 31 + 17) % 100 + 1;
            data[top] = v;
            if (top == 0) {
                mins[top] = v;
            } else {
                mins[top] = v < mins[top - 1] ? v : mins[top - 1];
            }
            top++;
        } else if (r < 7) {
            if (top > 0) top--;
        } else {
            if (top > 0) checksum += mins[top - 1];
        }
    }
    return checksum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    /* Test 1: manual push/pop/getMin */
    int64_t d1[10], m1[10];
    int top1 = 0;
    int64_t cs1 = 0;
    d1[0] = 5; m1[0] = 5; top1 = 1;
    d1[1] = 3; m1[1] = 3; top1 = 2;
    d1[2] = 7; m1[2] = 3; top1 = 3;
    cs1 += m1[top1 - 1]; /* getMin=3 */
    top1--; /* pop */
    cs1 += m1[top1 - 1]; /* getMin=3 */
    d1[top1] = 1; m1[top1] = 1; top1++;
    cs1 += m1[top1 - 1]; /* getMin=1 */
    printf("Test 1: %lld (expected 7)\n", cs1);

    printf("Test 2: %lld (expected 1845)\n", min_stack_ops(1000));

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = min_stack_ops(1000);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Min Stack ===\n");
    printf("Operations: 1000\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
