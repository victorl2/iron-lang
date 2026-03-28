#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

long dfs_checksum(int* adj, int n, int start) {
    int visited[30];
    memset(visited, 0, sizeof(int) * n);
    int stack[900];
    int top = 0;
    stack[top++] = start;
    long checksum = 0;
    int order = 0;

    while (top > 0) {
        int node = stack[--top];
        if (visited[node]) continue;
        visited[node] = 1;
        checksum += (long)node * (order + 1);
        order++;
        for (int j = n - 1; j >= 0; j--) {
            if (adj[node * n + j] && !visited[j]) {
                stack[top++] = j;
            }
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
    int n = 30;

    int adj1[900];
    memset(adj1, 0, sizeof(adj1));
    for (int i = 0; i < 29; i++) adj1[i * n + i + 1] = 1;
    printf("Test 1: %ld (expected 8990)\n", dfs_checksum(adj1, n, 0));

    int adj2[900];
    memset(adj2, 0, sizeof(adj2));
    for (int i = 0; i < 15; i++) {
        if (2*i+1 < n) adj2[i*n+2*i+1] = 1;
        if (2*i+2 < n) adj2[i*n+2*i+2] = 1;
    }
    printf("Test 2: %ld (expected 8035)\n", dfs_checksum(adj2, n, 0));

    int adj3[900];
    memset(adj3, 0, sizeof(adj3));
    for (int i = 1; i < n; i++) adj3[0*n+i] = 1;
    printf("Test 3: %ld (expected 8990)\n", dfs_checksum(adj3, n, 0));

    int adj4[900];
    memset(adj4, 0, sizeof(adj4));
    adj4[0*n+1] = 1; adj4[1*n+2] = 1;
    printf("Test 4: %ld (expected 8)\n", dfs_checksum(adj4, n, 0));

    long mem_before = get_memory_kb();
    int iterations = 200000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile long result = 0;
    for (int it = 0; it < iterations; it++) {
        result = dfs_checksum(adj2, n, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Graph DFS Traversal ===\n");
    printf("Nodes: %d\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
