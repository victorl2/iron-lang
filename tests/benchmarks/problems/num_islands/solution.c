#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

void dfs_fill(int* grid, int rows, int cols, int r, int c) {
    int stack[400];
    int top = 0;
    stack[top++] = r * cols + c;
    grid[r * cols + c] = 0;

    while (top > 0) {
        int pos = stack[--top];
        int cr = pos / cols;
        int cc = pos % cols;

        if (cr > 0 && grid[(cr-1)*cols + cc] == 1) {
            grid[(cr-1)*cols + cc] = 0;
            stack[top++] = (cr-1)*cols + cc;
        }
        if (cr < rows-1 && grid[(cr+1)*cols + cc] == 1) {
            grid[(cr+1)*cols + cc] = 0;
            stack[top++] = (cr+1)*cols + cc;
        }
        if (cc > 0 && grid[cr*cols + cc - 1] == 1) {
            grid[cr*cols + cc - 1] = 0;
            stack[top++] = cr*cols + cc - 1;
        }
        if (cc < cols-1 && grid[cr*cols + cc + 1] == 1) {
            grid[cr*cols + cc + 1] = 0;
            stack[top++] = cr*cols + cc + 1;
        }
    }
}

int num_islands(int* grid, int rows, int cols) {
    int count = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (grid[r * cols + c] == 1) {
                count++;
                dfs_fill(grid, rows, cols, r, c);
            }
        }
    }
    return count;
}

void make_grid(int* grid) {
    memset(grid, 0, 400 * sizeof(int));
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            grid[i * 20 + j] = 1;
    for (int i = 0; i < 4; i++)
        for (int j = 15; j < 20; j++)
            grid[i * 20 + j] = 1;
    for (int i = 8; i < 12; i++)
        for (int j = 8; j < 12; j++)
            grid[i * 20 + j] = 1;
    for (int i = 16; i < 20; i++)
        for (int j = 0; j < 3; j++)
            grid[i * 20 + j] = 1;
    grid[19 * 20 + 19] = 1;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int grid1[400];
    make_grid(grid1);
    printf("Test 1: %d (expected 5)\n", num_islands(grid1, 20, 20));

    int grid2[] = {1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1};
    printf("Test 2: %d (expected 3)\n", num_islands(grid2, 4, 5));

    int grid3[] = {1, 0, 1, 0, 1, 0, 1, 0, 1};
    printf("Test 3: %d (expected 5)\n", num_islands(grid3, 3, 3));

    long mem_before = get_memory_kb();
    int iterations = 400000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        int g[400];
        make_grid(g);
        result = num_islands(g, 20, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Num Islands ===\n");
    printf("Grid: 20x20\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
