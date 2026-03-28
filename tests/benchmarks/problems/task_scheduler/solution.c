#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

int64_t task_scheduler(int64_t* tasks, int64_t n, int64_t cooldown) {
    int64_t freq[26] = {0};
    for (int64_t i = 0; i < n; i++) {
        freq[tasks[i]]++;
    }

    int64_t max_freq = 0;
    for (int i = 0; i < 26; i++) {
        if (freq[i] > max_freq) max_freq = freq[i];
    }

    int64_t max_count = 0;
    for (int i = 0; i < 26; i++) {
        if (freq[i] == max_freq) max_count++;
    }

    int64_t formula_result = (max_freq - 1) * (cooldown + 1) + max_count;
    return n > formula_result ? n : formula_result;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {0, 0, 0, 1, 1, 1};
    printf("Test 1: %lld (expected 8)\n", task_scheduler(t1, 6, 2));

    int64_t t2[] = {0, 0, 0, 1, 1, 1};
    printf("Test 2: %lld (expected 6)\n", task_scheduler(t2, 6, 0));

    int64_t t3[] = {0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6};
    printf("Test 3: %lld (expected 16)\n", task_scheduler(t3, 12, 2));

    int64_t n = 50;
    int64_t tasks[50];
    for (int64_t i = 0; i < n; i++) {
        tasks[i] = (i * 7 + 3) % 10;
    }
    printf("Bench check: %lld\n", task_scheduler(tasks, n, 3));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = task_scheduler(tasks, n, 3);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Task Scheduler ===\n");
    printf("Tasks: %lld, Cooldown: 3\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
