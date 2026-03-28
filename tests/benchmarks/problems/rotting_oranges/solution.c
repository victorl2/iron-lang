#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

int rotting_oranges(int* grid, int rows, int cols) {
    int g[100];
    memcpy(g, grid, rows * cols * sizeof(int));
    int queue[200];
    int q_front = 0, q_back = 0;
    int fresh = 0;

    for (int i = 0; i < rows * cols; i++) {
        if (g[i] == 2) queue[q_back++] = i;
        else if (g[i] == 1) fresh++;
    }
    if (fresh == 0) return 0;

    int minutes = 0;
    while (q_front < q_back) {
        int size = q_back - q_front;
        int any = 0;
        for (int s = 0; s < size; s++) {
            int pos = queue[q_front++];
            int r = pos / cols, c = pos % cols;
            if (r > 0 && g[(r-1)*cols+c] == 1) { g[(r-1)*cols+c] = 2; fresh--; queue[q_back++] = (r-1)*cols+c; any = 1; }
            if (r < rows-1 && g[(r+1)*cols+c] == 1) { g[(r+1)*cols+c] = 2; fresh--; queue[q_back++] = (r+1)*cols+c; any = 1; }
            if (c > 0 && g[r*cols+c-1] == 1) { g[r*cols+c-1] = 2; fresh--; queue[q_back++] = r*cols+c-1; any = 1; }
            if (c < cols-1 && g[r*cols+c+1] == 1) { g[r*cols+c+1] = 2; fresh--; queue[q_back++] = r*cols+c+1; any = 1; }
        }
        if (any) minutes++;
    }
    return fresh == 0 ? minutes : -1;
}

void make_grid_10x10(int* g) {
    for (int i = 0; i < 100; i++) g[i] = 1;
    g[0] = 2;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int g1[100];
    make_grid_10x10(g1);
    printf("Test 1: %d (expected 18)\n", rotting_oranges(g1, 10, 10));

    int g2[] = {2, 1, 1, 1, 1, 0, 0, 1, 1};
    printf("Test 2: %d (expected 4)\n", rotting_oranges(g2, 3, 3));

    int g3[] = {2, 1, 1, 0, 1, 1, 1, 0, 1};
    printf("Test 3: %d (expected -1)\n", rotting_oranges(g3, 3, 3));

    int g4[] = {2, 2, 2, 2, 2, 2};
    printf("Test 4: %d (expected 0)\n", rotting_oranges(g4, 2, 3));

    long mem_before = get_memory_kb();
    int iterations = 200000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        int g[100];
        make_grid_10x10(g);
        result = rotting_oranges(g, 10, 10);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Rotting Oranges ===\n");
    printf("Grid: 10x10\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
