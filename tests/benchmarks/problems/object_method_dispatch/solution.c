#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

typedef struct {
    int64_t x;
    int64_t y;
} Point;

int64_t distance_sq(Point self, Point other) {
    int64_t dx = self.x - other.x;
    int64_t dy = self.y - other.y;
    return dx * dx + dy * dy;
}

Point translate(Point self, int64_t dx, int64_t dy) {
    Point result = {self.x + dx, self.y + dy};
    return result;
}

int64_t manhattan(Point self) {
    int64_t ax = self.x;
    int64_t ay = self.y;
    if (ax < 0) ax = -ax;
    if (ay < 0) ay = -ay;
    return ax + ay;
}

int64_t compute_checksum(int64_t n) {
    Point origin = {0, 0};
    int64_t checksum = 0;
    for (int64_t i = 0; i < n; i++) {
        Point p = {i * 3 - n, i * 7 % 100 - 50};
        Point moved = translate(p, 10, -5);
        int64_t dsq = distance_sq(moved, origin);
        checksum += dsq % 1000000 + manhattan(moved) % 1000;
    }
    return checksum;
}

int main(void) {
    Point p1 = {3, 4};
    Point p2 = {0, 0};
    printf("Test 1 dist_sq: %lld (expected 25)\n", (long long)distance_sq(p1, p2));
    printf("Test 2 manhattan: %lld (expected 7)\n", (long long)manhattan(p1));

    Point p3 = translate(p1, 1, -1);
    printf("Test 3 translate: %lld,%lld (expected 4,3)\n", (long long)p3.x, (long long)p3.y);

    printf("Test 4 checksum(100): %lld (expected 1189670)\n", (long long)compute_checksum(100));

    int iterations = 5000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = compute_checksum(100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Object Method Dispatch ===\n");
    printf("Points per call: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
