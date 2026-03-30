#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

void flood_fill(int* g, int rows, int cols, int sr, int sc, int new_color) {
    int old_color = g[sr * cols + sc];
    if (old_color == new_color) return;
    int stack[400];
    int top = 0;
    stack[top++] = sr * cols + sc;
    g[sr * cols + sc] = new_color;
    while (top > 0) {
        int pos = stack[--top];
        int cr = pos / cols;
        int cc = pos % cols;
        if (cr > 0 && g[(cr-1)*cols+cc] == old_color) { g[(cr-1)*cols+cc] = new_color; stack[top++] = (cr-1)*cols+cc; }
        if (cr < rows-1 && g[(cr+1)*cols+cc] == old_color) { g[(cr+1)*cols+cc] = new_color; stack[top++] = (cr+1)*cols+cc; }
        if (cc > 0 && g[cr*cols+cc-1] == old_color) { g[cr*cols+cc-1] = new_color; stack[top++] = cr*cols+cc-1; }
        if (cc < cols-1 && g[cr*cols+cc+1] == old_color) { g[cr*cols+cc+1] = new_color; stack[top++] = cr*cols+cc+1; }
    }
}

long checksum(int* grid, int size) {
    long s = 0;
    for (int i = 0; i < size; i++) s += (long)grid[i] * (i + 1);
    return s;
}

void make_grid(int* grid) {
    for (int i = 0; i < 400; i++) grid[i] = 1;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            grid[i * 20 + j] = 2;
    for (int i = 8; i < 14; i++)
        for (int j = 8; j < 14; j++)
            grid[i * 20 + j] = 3;
    for (int i = 0; i < 20; i++) {
        grid[i * 20 + 7] = 0;
        grid[7 * 20 + i] = 0;
    }
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int g[400];

    make_grid(g); flood_fill(g, 20, 20, 0, 0, 5);
    printf("Test 1: %ld (expected 101870)\n", checksum(g, 400));

    make_grid(g); flood_fill(g, 20, 20, 0, 10, 7);
    printf("Test 2: %ld (expected 130010)\n", checksum(g, 400));

    make_grid(g); flood_fill(g, 20, 20, 0, 0, 2);
    printf("Test 3: %ld (expected 92462)\n", checksum(g, 400));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile long result = 0;
    for (int it = 0; it < iterations; it++) {
        make_grid(g);
        flood_fill(g, 20, 20, 0, 0, 5);
        result = checksum(g, 400);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Flood Fill ===\n");
    printf("Grid: 20x20\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
