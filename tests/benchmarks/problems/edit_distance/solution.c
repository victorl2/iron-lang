#include <stdio.h>
#include <time.h>
#include <sys/resource.h>

/* Edit distance using two-row DP. Strings encoded as int arrays. */
int editDistance(int* w1, int n1, int* w2, int n2) {
    int prev[21], curr[21];

    for (int j = 0; j <= n2; j++) prev[j] = j;

    for (int i = 1; i <= n1; i++) {
        curr[0] = i;
        for (int j = 1; j <= n2; j++) {
            if (w1[i-1] == w2[j-1]) {
                curr[j] = prev[j-1];
            } else {
                int mn = prev[j-1];
                if (prev[j] < mn) mn = prev[j];
                if (curr[j-1] < mn) mn = curr[j-1];
                curr[j] = mn + 1;
            }
        }
        for (int j = 0; j <= n2; j++) prev[j] = curr[j];
    }
    return prev[n2];
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    /* "horse" -> "ros": 3 */
    int w1[] = {8,15,18,19,5}; int w2[] = {18,15,19};
    /* "intention" -> "execution": 5 */
    int w3[] = {9,14,20,5,14,20,9,15,14}; int w4[] = {5,24,5,3,21,20,9,15,14};
    printf("Test 1: %d (expected 3)\n", editDistance(w1,5,w2,3));
    printf("Test 2: %d (expected 5)\n", editDistance(w3,9,w4,9));

    /* Benchmark with longer strings */
    int s1[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
    int s2[] = {20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = editDistance(s1, 20, s2, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Edit Distance ===\n");
    printf("String lengths: 20, 20\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %d\n", (int)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
