#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

int is_bipartite(int* adj, int n) {
    int color[20];
    memset(color, -1, sizeof(int) * n);
    int queue[400];

    for (int start = 0; start < n; start++) {
        if (color[start] != -1) continue;
        color[start] = 0;
        int q_front = 0, q_back = 0;
        queue[q_back++] = start;
        while (q_front < q_back) {
            int node = queue[q_front++];
            for (int j = 0; j < n; j++) {
                if (adj[node * n + j] == 1) {
                    if (color[j] == -1) {
                        color[j] = 1 - color[node];
                        queue[q_back++] = j;
                    } else if (color[j] == color[node]) {
                        return 0;
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
    for (int i = 0; i < n; i += 2) {
        if (i + 1 < n) { adj1[i*n+i+1] = 1; adj1[(i+1)*n+i] = 1; }
    }
    printf("Test 1: %d (expected 1)\n", is_bipartite(adj1, n));

    int adj2[400];
    memset(adj2, 0, sizeof(adj2));
    adj2[0*n+1]=1; adj2[1*n+0]=1; adj2[1*n+2]=1; adj2[2*n+1]=1; adj2[0*n+2]=1; adj2[2*n+0]=1;
    printf("Test 2: %d (expected 0)\n", is_bipartite(adj2, n));

    int adj3[400];
    memset(adj3, 0, sizeof(adj3));
    adj3[0*n+1]=1; adj3[1*n+0]=1; adj3[1*n+2]=1; adj3[2*n+1]=1;
    adj3[2*n+3]=1; adj3[3*n+2]=1; adj3[3*n+0]=1; adj3[0*n+3]=1;
    printf("Test 3: %d (expected 1)\n", is_bipartite(adj3, n));

    int adj4[400];
    memset(adj4, 0, sizeof(adj4));
    adj4[0*n+1]=1; adj4[1*n+0]=1; adj4[1*n+2]=1; adj4[2*n+1]=1;
    adj4[2*n+3]=1; adj4[3*n+2]=1; adj4[3*n+4]=1; adj4[4*n+3]=1;
    adj4[4*n+0]=1; adj4[0*n+4]=1;
    printf("Test 4: %d (expected 0)\n", is_bipartite(adj4, n));

    long mem_before = get_memory_kb();
    int iterations = 2000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = is_bipartite(adj1, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Graph Bipartite ===\n");
    printf("Nodes: %d\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
