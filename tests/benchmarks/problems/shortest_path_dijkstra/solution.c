#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#define INF 999999999

int dijkstra(int* adj, int n, int src, int dst) {
    int dist[20], visited[20];
    for (int i = 0; i < n; i++) { dist[i] = INF; visited[i] = 0; }
    dist[src] = 0;

    for (int iter = 0; iter < n; iter++) {
        int u = -1, min_d = INF;
        for (int i = 0; i < n; i++) {
            if (!visited[i] && dist[i] < min_d) { min_d = dist[i]; u = i; }
        }
        if (u == -1) break;
        visited[u] = 1;
        for (int v = 0; v < n; v++) {
            int w = adj[u * n + v];
            if (w > 0 && !visited[v] && dist[u] + w < dist[v])
                dist[v] = dist[u] + w;
        }
    }
    return dist[dst];
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
    printf("Test 1: %d (expected 19)\n", dijkstra(adj1, n, 0, 19));

    int adj2[400];
    memset(adj2, 0, sizeof(adj2));
    for (int i = 0; i < 19; i++) adj2[i * n + i + 1] = 3;
    adj2[0 * n + 10] = 5;
    adj2[10 * n + 19] = 5;
    printf("Test 2: %d (expected 10)\n", dijkstra(adj2, n, 0, 19));

    int adj3[400];
    memset(adj3, 0, sizeof(adj3));
    adj3[0 * n + 1] = 1;
    printf("Test 3: %d (expected 999999999)\n", dijkstra(adj3, n, 0, 19));

    int adj4[400];
    memset(adj4, 0, sizeof(adj4));
    adj4[0*n+1]=10; adj4[0*n+2]=3; adj4[1*n+3]=2; adj4[2*n+1]=4; adj4[2*n+3]=8; adj4[2*n+4]=2; adj4[4*n+3]=1;
    printf("Test 4: %d (expected 6)\n", dijkstra(adj4, n, 0, 3));

    long mem_before = get_memory_kb();
    int iterations = 400000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = dijkstra(adj2, n, 0, 19);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Shortest Path Dijkstra ===\n");
    printf("Nodes: %d\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
