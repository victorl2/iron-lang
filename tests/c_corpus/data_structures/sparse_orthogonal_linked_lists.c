/*
 * title: Sparse matrix as orthogonal linked lists with node pool
 * topic: data_structures
 * covers: orthogonal lists, row and column header chains, indirect-pointer insertion, deletion from both chains, node free list, row and column scans, dense oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R 25
#define C 20
#define POOL 600

static unsigned long long rs = 0x0271A1ULL * 0x9E3779B1ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int r, c; long v; int right, down; } Node;   /* indices into pool, -1 = none */
static Node pool[POOL];
static int free_head, nused, nnz;
static int row_head[R], col_head[C];

static void init(void) {
    for (int i = 0; i < POOL; i++) pool[i].right = i + 1 < POOL ? i + 1 : -1;
    free_head = 0; nused = 0; nnz = 0;
    for (int i = 0; i < R; i++) row_head[i] = -1;
    for (int j = 0; j < C; j++) col_head[j] = -1;
}
/* pointer to the link that points at (r,c) or at the first node after it in the row chain */
static int *row_link(int r, int c) {
    int *p = &row_head[r];
    while (*p >= 0 && pool[*p].c < c) p = &pool[*p].right;
    return p;
}
static int *col_link(int r, int c) {
    int *p = &col_head[c];
    while (*p >= 0 && pool[*p].r < r) p = &pool[*p].down;
    return p;
}
static long get(int r, int c) { int *p = row_link(r, c); return (*p >= 0 && pool[*p].c == c) ? pool[*p].v : 0; }
static void erase(int r, int c) {
    int *rp = row_link(r, c);
    if (*rp < 0 || pool[*rp].c != c) return;
    int id = *rp;
    int *cp = col_link(r, c);
    check(*cp == id, "row and column chains reach the same node");
    *rp = pool[id].right; *cp = pool[id].down;
    pool[id].right = free_head; free_head = id; nused--; nnz--;
}
static void set(int r, int c, long v) {
    if (v == 0) { erase(r, c); return; }
    int *rp = row_link(r, c);
    if (*rp >= 0 && pool[*rp].c == c) { pool[*rp].v = v; return; }
    check(free_head >= 0, "pool not exhausted");
    int id = free_head; free_head = pool[id].right;
    pool[id].r = r; pool[id].c = c; pool[id].v = v;
    pool[id].right = *rp; *rp = id;
    int *cp = col_link(r, c);
    pool[id].down = *cp; *cp = id;
    nused++; nnz++;
}
static long row_sum(int r) { long s = 0; for (int p = row_head[r]; p >= 0; p = pool[p].right) s += pool[p].v; return s; }
static long col_sum(int c) { long s = 0; for (int p = col_head[c]; p >= 0; p = pool[p].down) s += pool[p].v; return s; }
static void verify(long d[R][C]) {
    int cnt = 0;
    for (int r = 0; r < R; r++) { int prev = -1; for (int p = row_head[r]; p >= 0; p = pool[p].right) { check(pool[p].r == r && pool[p].c > prev, "row chain sorted"); prev = pool[p].c; check(pool[p].v == d[r][prev], "row value"); cnt++; } }
    check(cnt == nnz, "row chains cover all nodes");
    cnt = 0;
    for (int c = 0; c < C; c++) { int prev = -1; for (int p = col_head[c]; p >= 0; p = pool[p].down) { check(pool[p].c == c && pool[p].r > prev, "col chain sorted"); prev = pool[p].r; check(pool[p].v == d[prev][c], "col value"); cnt++; } }
    check(cnt == nnz, "col chains cover all nodes");
}

int main(void) {
    init();
    static long d[R][C];
    long updates = 0, inserts = 0, deletes = 0;
    for (int step = 0; step < 20000; step++) {
        int r = (int)(rnd() % R), c = (int)(rnd() % C); unsigned op = rnd() % 10;
        if (op < 5) {
            long v = (long)(rnd() % 21) - 10;
            if (d[r][c] == 0 && v != 0) inserts++; else if (d[r][c] != 0 && v != 0) updates++; else if (d[r][c] != 0) deletes++;
            if (nnz >= POOL - 1 && d[r][c] == 0 && v != 0) continue;
            set(r, c, v); d[r][c] = v;
        } else if (op < 7) {
            if (d[r][c] != 0) deletes++;
            erase(r, c); d[r][c] = 0;
        } else if (op < 9) {
            check(get(r, c) == d[r][c], "get");
        } else {
            long s1 = 0, s2 = 0; for (int j = 0; j < C; j++) s1 += d[r][j]; for (int i = 0; i < R; i++) s2 += d[i][c];
            check(row_sum(r) == s1 && col_sum(c) == s2, "row/col sums");
        }
        if (step % 2000 == 0) verify(d);
    }
    verify(d);
    /* transpose by rebuilding into a second structure with swapped roles: check via sums */
    long total = 0, tr = 0; int maxr = 0, maxrn = 0;
    for (int r = 0; r < R; r++) {
        total += row_sum(r);
        int n = 0; for (int p = row_head[r]; p >= 0; p = pool[p].right) n++;
        if (n > maxrn) { maxrn = n; maxr = r; }
        if (r < C) tr += get(r, r);
    }
    long total_c = 0; for (int c = 0; c < C; c++) total_c += col_sum(c);
    check(total == total_c, "row-sum total equals column-sum total");
    printf("inserts=%ld updates=%ld deletes=%ld nnz=%d free_nodes=%d\n", inserts, updates, deletes, nnz, POOL - nused);
    printf("total=%ld trace=%ld densest row=%d (%d entries)\n", total, tr, maxr, maxrn);
    for (int r = 0; r < 4; r++) { printf("row %d:", r); for (int p = row_head[r]; p >= 0; p = pool[p].right) printf(" (%d:%ld)", pool[p].c, pool[p].v); printf("\n"); }
    /* clear everything, pool returns to full */
    for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) erase(r, c);
    check(nnz == 0 && nused == 0, "cleared");
    return 0;
}
