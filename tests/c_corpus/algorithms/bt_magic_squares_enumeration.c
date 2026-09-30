/*
 * title: Enumerating magic squares by line-constrained backtracking
 * topic: algorithms
 * covers: backtracking, cell ordering, forced values, line sum bounds, used-value bitmask, dihedral symmetry breaking
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLINES 10

static int N, M;      /* order and magic constant */
static int cellv[16]; /* row-major values, 0 = empty */
static unsigned used;
static int nlines;
static int line_sum[MAXLINES], line_rem[MAXLINES];
static int cell_lines[16][4], cell_nl[16];
static const int *order;
static int break_sym, want_semi;
static long found, nodes;
static int first[16], have_first;
static int corner_hist[17];
static int store[8][9];
static int nstore;

static const int ORDER2[4] = {0, 1, 2, 3};
static const int ORDER3[9] = {4, 0, 2, 6, 8, 1, 3, 5, 7};
static const int ORDER4[16] = {0, 3, 12, 15, 5, 6, 9, 10, 1, 2, 4, 7, 8, 11, 13, 14};

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void setup(int n, int semi) {
    N = n;
    M = n * (n * n + 1) / 2;
    want_semi = semi;
    memset(cellv, 0, sizeof cellv);
    memset(cell_nl, 0, sizeof cell_nl);
    used = 0;
    nlines = 2 * n + (semi ? 0 : 2);
    for (int i = 0; i < nlines; i++) {
        line_sum[i] = 0;
        line_rem[i] = n;
    }
    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++) {
            int cell = r * n + c;
            cell_lines[cell][cell_nl[cell]++] = r;
            cell_lines[cell][cell_nl[cell]++] = n + c;
            if (!semi && r == c)
                cell_lines[cell][cell_nl[cell]++] = 2 * n;
            if (!semi && r + c == n - 1)
                cell_lines[cell][cell_nl[cell]++] = 2 * n + 1;
        }
    order = n == 2 ? ORDER2 : (n == 3 ? ORDER3 : ORDER4);
    found = nodes = 0;
    have_first = 0;
    nstore = 0;
}

/* Can value v go in this cell? Each line through it must stay completable. */
static int fits(int cell, int v) {
    for (int k = 0; k < cell_nl[cell]; k++) {
        int l = cell_lines[cell][k];
        int sum = line_sum[l] + v, rem = line_rem[l] - 1;
        if (rem == 0) {
            if (sum != M)
                return 0;
        } else {
            int left = M - sum;
            if (left < rem * (rem + 1) / 2 || left > rem * (N * N) - rem * (rem - 1) / 2)
                return 0;
        }
    }
    return 1;
}

static void apply(int cell, int v, int sign) {
    for (int k = 0; k < cell_nl[cell]; k++) {
        int l = cell_lines[cell][k];
        line_sum[l] += sign * v;
        line_rem[l] -= sign;
    }
}

static void place(int idx) {
    nodes++;
    if (idx == N * N) {
        found++;
        if (!have_first) {
            memcpy(first, cellv, sizeof cellv);
            have_first = 1;
        }
        if (N == 3 && !want_semi) {
            check(nstore < 8, "at most eight 3x3 squares");
            memcpy(store[nstore++], cellv, sizeof(int) * 9);
        }
        if (N == 4)
            corner_hist[cellv[0]]++;
        return;
    }
    int cell = order[idx];
    int lo = 1, hi = N * N;
    /* if this cell completes a line, its value is forced */
    for (int k = 0; k < cell_nl[cell]; k++) {
        int l = cell_lines[cell][k];
        if (line_rem[l] == 1) {
            lo = hi = M - line_sum[l];
            break;
        }
    }
    if (lo < 1)
        return;
    for (int v = lo; v <= hi && v <= N * N; v++) {
        if (used & (1u << v))
            continue;
        if (break_sym) {
            /* one square per dihedral orbit: cell 0 is the smallest corner and
             * the top-right corner is smaller than the bottom-left corner */
            if (cell == 3 && v < cellv[0])
                continue;
            if (cell == 12 && (v < cellv[0] || v < cellv[3]))
                continue;
            if (cell == 15 && v < cellv[0])
                continue;
        }
        if (!fits(cell, v))
            continue;
        cellv[cell] = v;
        used |= 1u << v;
        apply(cell, v, +1);
        place(idx + 1);
        apply(cell, v, -1);
        used &= ~(1u << v);
        cellv[cell] = 0;
    }
}

static long run(int n, int semi) {
    setup(n, semi);
    place(0);
    return found;
}

int main(void) {
    long c3 = run(3, 0);
    printf("order 3: constant=%d squares=%ld nodes=%ld\n", M, c3, nodes);
    check(c3 == 8, "eight 3x3 magic squares");
    printf("first found: ");
    for (int i = 0; i < 9; i++)
        printf("%d%s", first[i], i == 8 ? "\n" : " ");
    for (int a = 0; a < 8; a++) {
        check(store[a][4] == 5, "centre is 5");
        for (int b = a + 1; b < 8; b++)
            check(memcmp(store[a], store[b], sizeof(int) * 9) != 0, "distinct squares");
    }
    long s3 = run(3, 1);
    printf("order 3 semi-magic (rows and columns only): %ld squares, nodes=%ld\n", s3, nodes);
    check(s3 == 72, "72 semi-magic squares of order 3");
    long c2 = run(2, 0);
    printf("order 2: squares=%ld\n", c2);
    check(c2 == 0, "no 2x2 magic squares");
    break_sym = 1;
    long c4 = run(4, 0);
    printf("order 4: constant=%d fundamental squares=%ld nodes=%ld\n", M, c4, nodes);
    check(c4 == 880, "880 fundamental magic squares of order 4");
    printf("times 8 symmetries = %ld squares in total\n", c4 * 8);
    printf("first fundamental square:\n");
    for (int i = 0; i < 4; i++)
        printf("  %2d %2d %2d %2d\n", first[i * 4], first[i * 4 + 1], first[i * 4 + 2], first[i * 4 + 3]);
    printf("smallest-corner histogram (value:count):");
    long sum = 0;
    for (int v = 1; v <= 16; v++)
        if (corner_hist[v]) {
            printf(" %d:%d", v, corner_hist[v]);
            sum += corner_hist[v];
        }
    printf("\n");
    check(sum == 880, "histogram totals");
    /* verify the first square really is magic */
    for (int i = 0; i < 4; i++) {
        int r = 0, c = 0;
        for (int j = 0; j < 4; j++) {
            r += first[i * 4 + j];
            c += first[j * 4 + i];
        }
        check(r == 34 && c == 34, "rows and columns sum to 34");
    }
    check(first[0] + first[5] + first[10] + first[15] == 34 && first[3] + first[6] + first[9] + first[12] == 34,
          "diagonals sum to 34");
    return 0;
}
