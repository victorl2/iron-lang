#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

void game_of_life(int64_t* grid, int64_t rows, int64_t cols, int64_t* out) {
    for (int64_t i = 0; i < rows; i++) {
        for (int64_t j = 0; j < cols; j++) {
            int64_t neighbors = 0;
            for (int di = -1; di <= 1; di++) {
                for (int dj = -1; dj <= 1; dj++) {
                    if (di == 0 && dj == 0) continue;
                    int64_t ni = i + di, nj = j + dj;
                    if (ni >= 0 && ni < rows && nj >= 0 && nj < cols) {
                        neighbors += grid[ni * cols + nj];
                    }
                }
            }
            int64_t cell = grid[i * cols + j];
            if (cell == 1) {
                out[i * cols + j] = (neighbors == 2 || neighbors == 3) ? 1 : 0;
            } else {
                out[i * cols + j] = (neighbors == 3) ? 1 : 0;
            }
        }
    }
}

int64_t count_alive(int64_t* grid, int64_t n) {
    int64_t count = 0;
    for (int64_t i = 0; i < n; i++) {
        if (grid[i] == 1) count++;
    }
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t g1[] = {0,0,0,0,0, 0,0,1,0,0, 0,0,1,0,0, 0,0,1,0,0, 0,0,0,0,0};
    int64_t o1[25] = {0};
    game_of_life(g1, 5, 5, o1);
    printf("Test 1 alive: %lld (expected 3)\n", count_alive(o1, 25));
    printf("Test 1 center: %lld %lld %lld (expected 1 1 1)\n", o1[11], o1[12], o1[13]);

    int64_t rows = 20, cols = 20;
    int64_t total = rows * cols;
    int64_t grid[400] = {0};
    for (int64_t i = 0; i < total; i++) {
        if ((i * 7 + 3) % 3 == 0) grid[i] = 1;
    }
    int64_t out[400] = {0};
    game_of_life(grid, rows, cols, out);
    printf("Bench check: %lld\n", count_alive(out, total));

    long mem_before = get_memory_kb();
    int iterations = 200000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        game_of_life(grid, rows, cols, out);
        result = count_alive(out, total);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Game of Life ===\n");
    printf("Grid: %lldx%lld\n", rows, cols);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
