/*
 * title: Maximal square and rectangle in a binary matrix
 * topic: algorithms
 * covers: dynamic programming, largest square of ones, histogram heights per row, maximal rectangle via column DP with left/right bounds, brute force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

#define R 12
#define C 14

static int g[R][C];

static int brute_square(void) {
    int best = 0;
    for (int i = 0; i < R; i++)
        for (int j = 0; j < C; j++)
            for (int s = 1; i + s <= R && j + s <= C; s++) {
                int ok = 1;
                for (int a = 0; a < s && ok; a++) for (int b = 0; b < s; b++) if (!g[i + a][j + b]) { ok = 0; break; }
                if (ok && s > best) best = s;
            }
    return best;
}

static int brute_rect(void) {
    int best = 0;
    for (int i = 0; i < R; i++)
        for (int j = 0; j < C; j++)
            for (int h = 1; i + h <= R; h++)
                for (int w = 1; j + w <= C; w++) {
                    int ok = 1;
                    for (int a = 0; a < h && ok; a++) for (int b = 0; b < w; b++) if (!g[i + a][j + b]) { ok = 0; break; }
                    if (ok && h * w > best) best = h * w;
                }
    return best;
}

int main(void) {
    for (int t = 0; t < 6; t++) {
        int density = 55 + t * 7;
        for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) g[i][j] = rr(100) < density;
        /* square DP */
        int sq[R][C], best_sq = 0, bi = 0, bj = 0;
        for (int i = 0; i < R; i++)
            for (int j = 0; j < C; j++) {
                if (!g[i][j]) { sq[i][j] = 0; continue; }
                if (i == 0 || j == 0) sq[i][j] = 1;
                else {
                    int m = sq[i - 1][j];
                    if (sq[i][j - 1] < m) m = sq[i][j - 1];
                    if (sq[i - 1][j - 1] < m) m = sq[i - 1][j - 1];
                    sq[i][j] = m + 1;
                }
                if (sq[i][j] > best_sq) { best_sq = sq[i][j]; bi = i - sq[i][j] + 1; bj = j - sq[i][j] + 1; }
            }
        /* rectangle: per column height, left/right boundaries (the classic three-array DP) */
        int height[C] = {0}, left[C], right[C], best_rect = 0;
        for (int j = 0; j < C; j++) right[j] = C;
        for (int j = 0; j < C; j++) left[j] = 0;
        for (int i = 0; i < R; i++) {
            int cl = 0, cr = C;
            for (int j = 0; j < C; j++) height[j] = g[i][j] ? height[j] + 1 : 0;
            for (int j = 0; j < C; j++) {
                if (g[i][j]) { if (cl > left[j]) left[j] = cl; }
                else { left[j] = 0; cl = j + 1; }
            }
            for (int j = C - 1; j >= 0; j--) {
                if (g[i][j]) { if (cr < right[j]) right[j] = cr; }
                else { right[j] = C; cr = j; }
            }
            for (int j = 0; j < C; j++) {
                int area = (right[j] - left[j]) * height[j];
                if (area > best_rect) best_rect = area;
            }
        }
        CHECK(best_sq == brute_square());
        CHECK(best_rect == brute_rect());
        int ones = 0;
        for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) ones += g[i][j];
        printf("density=%d%% ones=%d square=%d at (%d,%d) rectangle=%d\n", density, ones, best_sq, bi, bj, best_rect);
    }
    return 0;
}
