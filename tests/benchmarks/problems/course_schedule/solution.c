#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

int can_finish(int* adj, int n) {
    int state[20];
    memset(state, 0, sizeof(state));
    int stack[400], stack_type[400];
    int top = 0;

    for (int start = 0; start < n; start++) {
        if (state[start] == 0) {
            stack[top] = start;
            stack_type[top] = 0;
            top++;

            while (top > 0) {
                top--;
                int node = stack[top];
                int typ = stack_type[top];

                if (typ == 1) {
                    state[node] = 2;
                } else {
                    if (state[node] == 1) return 0;
                    if (state[node] == 0) {
                        state[node] = 1;
                        stack[top] = node;
                        stack_type[top] = 1;
                        top++;
                        for (int j = 0; j < n; j++) {
                            if (adj[node * n + j] == 1) {
                                if (state[j] == 1) return 0;
                                if (state[j] == 0) {
                                    stack[top] = j;
                                    stack_type[top] = 0;
                                    top++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return 1;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int n = 20;

    int adj1[400];
    memset(adj1, 0, sizeof(adj1));
    for (int i = 0; i < n - 1; i++) adj1[i * n + i + 1] = 1;
    printf("Test 1: %d (expected 1)\n", can_finish(adj1, n));

    int adj2[400];
    memset(adj2, 0, sizeof(adj2));
    adj2[0 * n + 1] = 1;
    adj2[1 * n + 2] = 1;
    adj2[2 * n + 0] = 1;
    printf("Test 2: %d (expected 0)\n", can_finish(adj2, n));

    int adj3[400];
    memset(adj3, 0, sizeof(adj3));
    adj3[0 * n + 1] = 1;
    adj3[0 * n + 2] = 1;
    adj3[1 * n + 3] = 1;
    adj3[2 * n + 3] = 1;
    adj3[3 * n + 4] = 1;
    adj3[5 * n + 6] = 1;
    adj3[6 * n + 7] = 1;
    adj3[7 * n + 8] = 1;
    adj3[10 * n + 11] = 1;
    adj3[11 * n + 12] = 1;
    printf("Test 3: %d (expected 1)\n", can_finish(adj3, n));

    int adj4[400];
    memset(adj4, 0, sizeof(adj4));
    adj4[0 * n + 1] = 1;
    adj4[1 * n + 2] = 1;
    adj4[2 * n + 3] = 1;
    adj4[3 * n + 4] = 1;
    adj4[4 * n + 5] = 1;
    adj4[5 * n + 6] = 1;
    adj4[6 * n + 7] = 1;
    adj4[7 * n + 3] = 1;
    printf("Test 4: %d (expected 0)\n", can_finish(adj4, n));

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = can_finish(adj3, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Course Schedule ===\n");
    printf("Nodes: %d\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
