#include <stdio.h>
#include <time.h>
#include <sys/resource.h>

static int solutions;
static int cols[20], diag1[40], diag2[40], queens[20];

void solve(int row, int n) {
    if (row == n) { solutions++; return; }
    for (int col = 0; col < n; col++) {
        if (!cols[col] && !diag1[row - col + n] && !diag2[row + col]) {
            queens[row] = col;
            cols[col] = diag1[row - col + n] = diag2[row + col] = 1;
            solve(row + 1, n);
            cols[col] = diag1[row - col + n] = diag2[row + col] = 0;
        }
    }
}

int nQueens(int n) {
    solutions = 0;
    for (int i = 0; i < 20; i++) cols[i] = 0;
    for (int i = 0; i < 40; i++) { diag1[i] = 0; diag2[i] = 0; }
    solve(0, n);
    return solutions;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("N=4: %d solutions (expected 2)\n", nQueens(4));
    printf("N=8: %d solutions (expected 92)\n", nQueens(8));
    printf("N=12: %d solutions (expected 14200)\n", nQueens(12));

    long mem_before = get_memory_kb();
    int iterations = 100;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = nQueens(12);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: N-Queens ===\n");
    printf("N=12\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
