/*
 * title: Sparse matrix product with Gustavson's row-wise accumulator
 * topic: data_structures
 * covers: CSR, SpGEMM, sparse accumulator with marker array, symbolic and numeric phases, sorting row output, matrix powers, transpose, dense oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 30

static unsigned long long rs = 0x59637ULL * 0x9E3779B9ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int n, nnz; int *ptr, *col; long *val; } Csr;

static Csr from_dense(long d[N][N]) {
    Csr m; m.n = N; m.nnz = 0;
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) m.nnz += d[i][j] != 0;
    m.ptr = malloc((N + 1) * sizeof(int)); m.col = malloc((size_t)(m.nnz + 1) * sizeof(int)); m.val = malloc((size_t)(m.nnz + 1) * sizeof(long));
    int k = 0;
    for (int i = 0; i < N; i++) { m.ptr[i] = k; for (int j = 0; j < N; j++) if (d[i][j]) { m.col[k] = j; m.val[k] = d[i][j]; k++; } }
    m.ptr[N] = k;
    return m;
}
static void to_dense(const Csr *m, long d[N][N]) {
    memset(d, 0, sizeof(long) * N * N);
    for (int i = 0; i < N; i++) for (int p = m->ptr[i]; p < m->ptr[i + 1]; p++) d[i][m->col[p]] = m->val[p];
}
static void freem(Csr *m) { free(m->ptr); free(m->col); free(m->val); }
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

/* symbolic phase counts structural nonzeros per row; numeric phase fills them */
static Csr spgemm(const Csr *a, const Csr *b, long *flops) {
    Csr c; c.n = a->n;
    int marker[N]; long acc[N]; int cols[N];
    for (int j = 0; j < N; j++) marker[j] = -1;
    c.ptr = malloc((N + 1) * sizeof(int)); c.ptr[0] = 0;
    int total = 0;
    for (int i = 0; i < N; i++) {           /* symbolic */
        int cnt = 0;
        for (int p = a->ptr[i]; p < a->ptr[i + 1]; p++) for (int q = b->ptr[a->col[p]]; q < b->ptr[a->col[p] + 1]; q++)
            if (marker[b->col[q]] != i) { marker[b->col[q]] = i; cnt++; }
        total += cnt; c.ptr[i + 1] = total;
    }
    c.nnz = total; c.col = malloc((size_t)(total + 1) * sizeof(int)); c.val = malloc((size_t)(total + 1) * sizeof(long));
    for (int j = 0; j < N; j++) marker[j] = -1;
    for (int i = 0; i < N; i++) {           /* numeric */
        int cnt = 0;
        for (int p = a->ptr[i]; p < a->ptr[i + 1]; p++) for (int q = b->ptr[a->col[p]]; q < b->ptr[a->col[p] + 1]; q++) {
            int j = b->col[q]; (*flops)++;
            if (marker[j] != i) { marker[j] = i; acc[j] = 0; cols[cnt++] = j; }
            acc[j] += a->val[p] * b->val[q];
        }
        qsort(cols, (size_t)cnt, sizeof(int), cmp_int);
        for (int k = 0; k < cnt; k++) { c.col[c.ptr[i] + k] = cols[k]; c.val[c.ptr[i] + k] = acc[cols[k]]; }
    }
    return c;
}
static Csr transpose(const Csr *a) {
    Csr t; t.n = a->n; t.nnz = a->nnz;
    t.ptr = calloc(N + 1, sizeof(int)); t.col = malloc((size_t)(a->nnz + 1) * sizeof(int)); t.val = malloc((size_t)(a->nnz + 1) * sizeof(long));
    for (int p = 0; p < a->nnz; p++) t.ptr[a->col[p] + 1]++;
    for (int i = 0; i < N; i++) t.ptr[i + 1] += t.ptr[i];
    int pos[N]; memcpy(pos, t.ptr, sizeof pos);
    for (int i = 0; i < N; i++) for (int p = a->ptr[i]; p < a->ptr[i + 1]; p++) { int q = pos[a->col[p]]++; t.col[q] = i; t.val[q] = a->val[p]; }
    return t;
}
static void dense_mul(long a[N][N], long b[N][N], long c[N][N]) {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { long s = 0; for (int k = 0; k < N; k++) s += a[i][k] * b[k][j]; c[i][j] = s; }
}

int main(void) {
    static long da[N][N], db[N][N], dc[N][N], dg[N][N];
    for (int trial = 0; trial < 3; trial++) {
        int dens = 4 + trial * 6;
        memset(da, 0, sizeof da); memset(db, 0, sizeof db);
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
            if ((int)(rnd() % 100) < dens) da[i][j] = (long)(rnd() % 7) - 3;
            if ((int)(rnd() % 100) < dens) db[i][j] = (long)(rnd() % 7) - 3;
        }
        Csr a = from_dense(da), b = from_dense(db);
        long flops = 0;
        Csr c = spgemm(&a, &b, &flops);
        dense_mul(da, db, dc); to_dense(&c, dg);
        check(memcmp(dc, dg, sizeof dc) == 0, "A*B vs dense");
        for (int i = 0; i < N; i++) for (int p = c.ptr[i] + 1; p < c.ptr[i + 1]; p++) check(c.col[p - 1] < c.col[p], "sorted columns");
        /* (AB)^T = B^T A^T */
        Csr at = transpose(&a), bt = transpose(&b), ct = transpose(&c), btat = spgemm(&bt, &at, &flops);
        long d1[N][N], d2[N][N]; to_dense(&ct, d1); to_dense(&btat, d2);
        check(memcmp(d1, d2, sizeof d1) == 0, "(AB)^T = B^T A^T");
        long trace = 0, tot = 0; for (int i = 0; i < N; i++) { trace += dg[i][i]; for (int j = 0; j < N; j++) tot += dg[i][j]; }
        long dense_flops = 0;
        for (int i = 0; i < N; i++) for (int k = 0; k < N; k++) if (da[i][k]) for (int j = 0; j < N; j++) dense_flops += db[k][j] != 0;
        printf("density=%2d%% nnzA=%3d nnzB=%3d nnzC=%3d products=%5ld trace=%ld sum=%ld\n", dens, a.nnz, b.nnz, c.nnz, dense_flops, trace, tot);
        freem(&a); freem(&b); freem(&c); freem(&at); freem(&bt); freem(&ct); freem(&btat);
    }
    /* matrix powers of an adjacency matrix count walks: A^k versus repeated dense products */
    memset(da, 0, sizeof da);
    for (int i = 0; i < N; i++) for (int t = 0; t < 2; t++) { unsigned j = rnd() % N; da[i][j] = 1; }
    Csr a = from_dense(da), p = from_dense(da); long fl = 0;
    memcpy(dc, da, sizeof dc);
    for (int k = 2; k <= 5; k++) {
        Csr np = spgemm(&p, &a, &fl); freem(&p); p = np;
        dense_mul(dc, da, dg); memcpy(dc, dg, sizeof dc);
        to_dense(&p, dg); check(memcmp(dc, dg, sizeof dc) == 0, "power vs dense");
        long walks = 0; for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) walks += dg[i][j];
        printf("A^%d: nnz=%3d walks=%ld\n", k, p.nnz, walks);
    }
    freem(&a); freem(&p);
    return 0;
}
