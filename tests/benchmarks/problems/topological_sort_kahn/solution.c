#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

long kahn_checksum(int* adj, int n) {
    int in_deg[20];
    memset(in_deg, 0, sizeof(int) * n);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (adj[i * n + j]) in_deg[j]++;

    int queue[20], q_front = 0, q_back = 0;
    for (int i = 0; i < n; i++)
        if (in_deg[i] == 0) queue[q_back++] = i;

    long checksum = 0;
    int count = 0, pos = 0;
    while (q_front < q_back) {
        int node = queue[q_front++];
        checksum += (long)node * (pos + 1);
        pos++;
        count++;
        for (int j = 0; j < n; j++) {
            if (adj[node * n + j]) {
                in_deg[j]--;
                if (in_deg[j] == 0) queue[q_back++] = j;
            }
        }
    }
    return count == n ? checksum : -1;
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
    for (int i = 0; i < 19; i++) adj1[i * n + i + 1] = 1;
    printf("Test 1: %ld (expected 2660)\n", kahn_checksum(adj1, n));

    int adj2[400];
    memset(adj2, 0, sizeof(adj2));
    printf("Test 2: %ld (expected 2660)\n", kahn_checksum(adj2, n));

    int adj3[400];
    memset(adj3, 0, sizeof(adj3));
    adj3[0*n+1]=1; adj3[0*n+2]=1; adj3[1*n+3]=1; adj3[2*n+3]=1;
    printf("Test 3: %ld (expected 2204)\n", kahn_checksum(adj3, n));

    int adj4[400];
    memset(adj4, 0, sizeof(adj4));
    adj4[0*n+1]=1; adj4[1*n+2]=1; adj4[2*n+0]=1;
    printf("Test 4: %ld (expected -1)\n", kahn_checksum(adj4, n));

    long mem_before = get_memory_kb();
    int iterations = 200000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile long result = 0;
    for (int it = 0; it < iterations; it++) {
        result = kahn_checksum(adj1, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Topological Sort Kahn ===\n");
    printf("Nodes: %d\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
