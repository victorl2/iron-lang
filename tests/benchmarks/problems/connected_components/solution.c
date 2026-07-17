#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

__attribute__((noinline))
int find_root(int* parent, int x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

__attribute__((noinline))
int count_components(int* edges_a, int* edges_b, int num_edges, int n) {
    int parent[50], rnk[50];
    for (int i = 0; i < n; i++) { parent[i] = i; rnk[i] = 0; }

    for (int e = 0; e < num_edges; e++) {
        int rx = find_root(parent, edges_a[e]);
        int ry = find_root(parent, edges_b[e]);
        if (rx != ry) {
            if (rnk[rx] < rnk[ry]) { int t = rx; rx = ry; ry = t; }
            parent[ry] = rx;
            if (rnk[rx] == rnk[ry]) rnk[rx]++;
        }
    }

    int count = 0;
    for (int i = 0; i < n; i++) {
        if (find_root(parent, i) == i) count++;
    }
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int n = 50;

    printf("Test 1: %d (expected 50)\n", count_components(NULL, NULL, 0, n));

    int ea2[49], eb2[49];
    for (int i = 0; i < 49; i++) { ea2[i] = i; eb2[i] = i + 1; }
    printf("Test 2: %d (expected 1)\n", count_components(ea2, eb2, 49, n));

    int ea3[40], eb3[40];
    int idx = 0;
    for (int c = 0; c < 10; c++)
        for (int j = 1; j < 5; j++) {
            ea3[idx] = c * 5; eb3[idx] = c * 5 + j; idx++;
        }
    printf("Test 3: %d (expected 10)\n", count_components(ea3, eb3, idx, n));

    int ea4[25], eb4[25];
    for (int i = 0; i < 25; i++) { ea4[i] = i * 2; eb4[i] = i * 2 + 1; }
    printf("Test 4: %d (expected 25)\n", count_components(ea4, eb4, 25, n));

    long mem_before = get_memory_kb();
    int iterations = 180180;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = count_components(ea3, eb3, 40, n);
        /* count_components only writes locals, so a compiler that infers it as
         * readonly can hoist this loop-invariant call out — `volatile result`
         * pins the store, not the call, and `noinline` stops inlining, not LICM.
         * Apple clang does hoist it: the timed loop reported 0.06ms for 180180
         * iterations (~0.3ns each, about one cycle) where the same source takes
         * ~47ms on Linux, so the C baseline this benchmark compares Iron against
         * was not running at all and the reported ratio was measuring nothing.
         * The clobber makes the arguments potentially-modified each iteration,
         * so the call has to happen. */
        __asm__ __volatile__("" ::: "memory");
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Connected Components ===\n");
    printf("Nodes: %d\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
