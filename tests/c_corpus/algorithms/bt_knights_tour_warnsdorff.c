/*
 * title: Knight's tour with Warnsdorff ordering and backtracking
 * topic: algorithms
 * covers: backtracking, Warnsdorff heuristic, move ordering, closed tours, dead-end counting, board printing
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 8

static const int DX[8] = {1, 2, 2, 1, -1, -2, -2, -1};
static const int DY[8] = {2, 1, -1, -2, -2, -1, 1, 2};

static int N;
static int board[MAXN][MAXN];
static long visits, dead_ends, limit;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int ok(int x, int y) {
    return x >= 0 && y >= 0 && x < N && y < N && board[x][y] == 0;
}

static int degree(int x, int y) {
    int d = 0;
    for (int k = 0; k < 8; k++)
        d += ok(x + DX[k], y + DY[k]);
    return d;
}

static int is_neighbour(int x1, int y1, int x2, int y2) {
    int dx = abs(x1 - x2), dy = abs(y1 - y2);
    return (dx == 1 && dy == 2) || (dx == 2 && dy == 1);
}

static int sx, sy;

/* order = 1: sort candidate moves by onward degree (ties by move index); order = 0: fixed order. */
static int tour(int x, int y, int step, int order, int closed) {
    board[x][y] = step;
    visits++;
    if (visits > limit) {
        board[x][y] = 0;
        return -1;
    }
    if (step == N * N) {
        if (!closed || is_neighbour(x, y, sx, sy))
            return 1;
        board[x][y] = 0;
        dead_ends++;
        return 0;
    }
    int cand[8], deg[8], nc = 0;
    for (int k = 0; k < 8; k++) {
        int nx = x + DX[k], ny = y + DY[k];
        if (ok(nx, ny)) {
            cand[nc] = k;
            deg[nc++] = order ? degree(nx, ny) : 0;
        }
    }
    for (int i = 1; i < nc; i++) { /* stable insertion sort on degree */
        int kc = cand[i], kd = deg[i], j = i - 1;
        while (j >= 0 && deg[j] > kd) {
            cand[j + 1] = cand[j];
            deg[j + 1] = deg[j];
            j--;
        }
        cand[j + 1] = kc;
        deg[j + 1] = kd;
    }
    if (nc == 0)
        dead_ends++;
    for (int i = 0; i < nc; i++) {
        int r = tour(x + DX[cand[i]], y + DY[cand[i]], step + 1, order, closed);
        if (r != 0)
            return r;
    }
    board[x][y] = 0;
    return 0;
}

static int verify(void) {
    int px[MAXN * MAXN + 1], py[MAXN * MAXN + 1];
    for (int x = 0; x < N; x++)
        for (int y = 0; y < N; y++) {
            int s = board[x][y];
            if (s < 1 || s > N * N)
                return 0;
            px[s] = x;
            py[s] = y;
        }
    for (int s = 1; s < N * N; s++)
        if (!is_neighbour(px[s], py[s], px[s + 1], py[s + 1]))
            return 0;
    return 1;
}

static void run(int n, int x, int y, int order, int closed, const char *label, int print) {
    N = n;
    sx = x;
    sy = y;
    memset(board, 0, sizeof board);
    visits = dead_ends = 0;
    limit = 2000000;
    int r = tour(x, y, 1, order, closed);
    printf("%-30s -> %s, visits=%ld dead_ends=%ld\n", label, r == 1 ? "found" : (r == 0 ? "none" : "gave up"),
           visits, dead_ends);
    if (r == 1) {
        check(verify(), "tour is a valid knight path");
        if (closed)
            for (int i = 0; i < N * N; i++)
                if (board[i / N][i % N] == N * N)
                    check(is_neighbour(i / N, i % N, sx, sy), "closed tour returns to start");
        if (print)
            for (int i = 0; i < N; i++) {
                for (int j = 0; j < N; j++)
                    printf("%3d", board[i][j]);
                printf("\n");
            }
    }
}

int main(void) {
    run(5, 0, 0, 1, 0, "5x5 open, Warnsdorff", 1);
    run(6, 0, 0, 1, 0, "6x6 open, Warnsdorff", 0);
    run(6, 0, 0, 0, 0, "6x6 open, fixed move order", 0);
    run(8, 0, 0, 1, 0, "8x8 open, Warnsdorff", 1);
    run(8, 2, 3, 1, 1, "8x8 closed from (2,3)", 0);
    run(5, 0, 1, 1, 0, "5x5 open from odd square", 0);
    run(4, 0, 0, 1, 0, "4x4 open (no tour exists)", 0);
    run(3, 0, 0, 1, 0, "3x3 open (no tour exists)", 0);
    run(5, 0, 0, 1, 1, "5x5 closed (parity forbids)", 0);
    return 0;
}
