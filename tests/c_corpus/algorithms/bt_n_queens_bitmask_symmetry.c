/*
 * title: N-queens with bitmasks and symmetry classes
 * topic: algorithms
 * covers: backtracking, bitmask column and diagonal sets, lowest set bit extraction, dihedral symmetry, canonical forms
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 12

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long nodes;

/* Counts solutions using three bit sets: columns, and both diagonal directions. */
static long solve_bits(unsigned all, unsigned cols, unsigned d1, unsigned d2) {
    if (cols == all)
        return 1;
    long cnt = 0;
    unsigned avail = all & ~(cols | d1 | d2);
    while (avail) {
        unsigned bit = avail & (0u - avail);
        avail ^= bit;
        nodes++;
        cnt += solve_bits(all, cols | bit, ((d1 | bit) << 1) & all, (d2 | bit) >> 1);
    }
    return cnt;
}

/* Array-based reference solver with explicit conflict scans. */
static int ref_pos[MAXN];

static long solve_ref(int n, int row) {
    if (row == n)
        return 1;
    long c = 0;
    for (int col = 0; col < n; col++) {
        int ok = 1;
        for (int r = 0; r < row && ok; r++)
            if (ref_pos[r] == col || abs(ref_pos[r] - col) == row - r)
                ok = 0;
        if (ok) {
            ref_pos[row] = col;
            c += solve_ref(n, row + 1);
        }
    }
    return c;
}

static int sols[100000][MAXN];
static int nsol;

static void collect(int n, int row, unsigned cols, unsigned d1, unsigned d2, int *cur) {
    unsigned all = (1u << n) - 1;
    if (row == n) {
        memcpy(sols[nsol++], cur, sizeof(int) * (size_t)n);
        return;
    }
    unsigned avail = all & ~(cols | d1 | d2);
    while (avail) {
        unsigned bit = avail & (0u - avail);
        avail ^= bit;
        int col = 0;
        while (!((bit >> col) & 1u))
            col++;
        cur[row] = col;
        collect(n, row + 1, cols | bit, ((d1 | bit) << 1) & all, (d2 | bit) >> 1, cur);
    }
}

/* the 8 dihedral images of a placement given as pos[row] = col */
static void transform(const int *p, int n, int which, int *out) {
    for (int r = 0; r < n; r++) {
        int c = p[r], nr, nc;
        switch (which & 3) {
        case 0: nr = r; nc = c; break;
        case 1: nr = c; nc = n - 1 - r; break;
        case 2: nr = n - 1 - r; nc = n - 1 - c; break;
        default: nr = n - 1 - c; nc = r; break;
        }
        if (which & 4)
            nc = n - 1 - nc;
        out[nr] = nc;
    }
}

static int cmp_lex(const int *a, const int *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

int main(void) {
    long known[] = {1, 0, 0, 2, 10, 4, 40, 92, 352, 724, 2680, 14200};
    for (int n = 1; n <= 12; n++) {
        nodes = 0;
        long c = solve_bits((1u << n) - 1, 0, 0, 0);
        check(c == known[n - 1], "known solution count");
        if (n <= 9)
            check(solve_ref(n, 0) == c, "array solver agrees");
        printf("n=%2d solutions=%6ld nodes=%8ld\n", n, c, nodes);
    }
    /* symmetry classes for n = 4..9 via canonical (lexicographically least) dihedral image */
    for (int n = 4; n <= 9; n++) {
        nsol = 0;
        int cur[MAXN];
        collect(n, 0, 0, 0, 0, cur);
        int classes = 0, self_sym = 0;
        for (int i = 0; i < nsol; i++) {
            int best[MAXN], img[MAXN], same = 0;
            transform(sols[i], n, 0, best);
            for (int w = 1; w < 8; w++) {
                transform(sols[i], n, w, img);
                if (cmp_lex(img, best, n) < 0)
                    memcpy(best, img, sizeof(int) * (size_t)n);
                if (cmp_lex(img, sols[i], n) == 0)
                    same++;
            }
            if (cmp_lex(best, sols[i], n) == 0) {
                classes++;
                if (same > 0)
                    self_sym++;
            }
        }
        printf("n=%d: %d solutions, %d fundamental, %d with a symmetry\n", n, nsol, classes, self_sym);
        if (n == 8)
            check(classes == 12, "twelve fundamental solutions for 8 queens");
    }
    /* first lexicographic solution for n = 8 */
    nsol = 0;
    int cur[MAXN];
    collect(8, 0, 0, 0, 0, cur);
    printf("first 8-queens solution (row -> col):");
    for (int r = 0; r < 8; r++)
        printf(" %d", sols[0][r]);
    printf("\n");
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++)
            putchar(sols[0][r] == c ? 'Q' : '.');
        putchar('\n');
    }
    return 0;
}
