/*
 * title: Sparse matrix formats COO, CSR and CSC with conversions
 * topic: data_structures
 * covers: sparse matrix, coordinate list, CSR, CSC, duplicate summing, sort by (row, col), counting-sort conversion, transpose as format swap, spmv, dense oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R 37
#define C 29

static unsigned long long rs = 0x5BA25EULL * 0x9E3779ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int r, c; long v; } Triple;
typedef struct { int n; Triple *t; } Coo;
typedef struct { int rows, cols, nnz; int *ptr, *idx; long *val; } Comp;   /* CSR (ptr over rows, idx = cols) or CSC (ptr over cols, idx = rows) */

static int cmp_rc(const void *a, const void *b) {
    const Triple *x = a, *y = b;
    if (x->r != y->r) return x->r < y->r ? -1 : 1;
    return (x->c > y->c) - (x->c < y->c);
}
/* sort and sum duplicates, drop zeros */
static void coo_canon(Coo *m) {
    qsort(m->t, (size_t)m->n, sizeof(Triple), cmp_rc);
    int w = 0;
    for (int i = 0; i < m->n; ) {
        Triple t = m->t[i]; int j = i + 1;
        while (j < m->n && m->t[j].r == t.r && m->t[j].c == t.c) { t.v += m->t[j].v; j++; }
        if (t.v != 0) m->t[w++] = t;
        i = j;
    }
    m->n = w;
}
/* generic counting-sort build: key = row for CSR, col for CSC. Input must be canonical (row-major) so that
 * CSR stays sorted by column and CSC ends sorted by row (stable pass). */
static Comp build(const Coo *m, int by_row) {
    Comp k; k.rows = R; k.cols = C; k.nnz = m->n;
    int nk = by_row ? R : C;
    k.ptr = calloc((size_t)nk + 1, sizeof(int)); k.idx = malloc((size_t)(m->n + 1) * sizeof(int)); k.val = malloc((size_t)(m->n + 1) * sizeof(long));
    for (int i = 0; i < m->n; i++) k.ptr[(by_row ? m->t[i].r : m->t[i].c) + 1]++;
    for (int i = 0; i < nk; i++) k.ptr[i + 1] += k.ptr[i];
    int *pos = malloc((size_t)nk * sizeof(int)); memcpy(pos, k.ptr, (size_t)nk * sizeof(int));
    for (int i = 0; i < m->n; i++) {
        int key = by_row ? m->t[i].r : m->t[i].c, p = pos[key]++;
        k.idx[p] = by_row ? m->t[i].c : m->t[i].r; k.val[p] = m->t[i].v;
    }
    free(pos);
    return k;
}
static void comp_free(Comp *k) { free(k->ptr); free(k->idx); free(k->val); }
/* CSR -> CSC by direct counting pass, without going through COO */
static Comp csr_to_csc(const Comp *a) {
    Comp k; k.rows = a->rows; k.cols = a->cols; k.nnz = a->nnz;
    k.ptr = calloc((size_t)a->cols + 1, sizeof(int)); k.idx = malloc((size_t)(a->nnz + 1) * sizeof(int)); k.val = malloc((size_t)(a->nnz + 1) * sizeof(long));
    for (int p = 0; p < a->nnz; p++) k.ptr[a->idx[p] + 1]++;
    for (int c = 0; c < a->cols; c++) k.ptr[c + 1] += k.ptr[c];
    int *pos = malloc((size_t)a->cols * sizeof(int)); memcpy(pos, k.ptr, (size_t)a->cols * sizeof(int));
    for (int r = 0; r < a->rows; r++) for (int p = a->ptr[r]; p < a->ptr[r + 1]; p++) { int q = pos[a->idx[p]]++; k.idx[q] = r; k.val[q] = a->val[p]; }
    free(pos);
    return k;
}
static int comp_equal(const Comp *a, const Comp *b, int nk) {
    return a->nnz == b->nnz && memcmp(a->ptr, b->ptr, (size_t)(nk + 1) * sizeof(int)) == 0 &&
           memcmp(a->idx, b->idx, (size_t)a->nnz * sizeof(int)) == 0 && memcmp(a->val, b->val, (size_t)a->nnz * sizeof(long)) == 0;
}
static void spmv_csr(const Comp *a, const long *x, long *y) {
    for (int r = 0; r < a->rows; r++) { long s = 0; for (int p = a->ptr[r]; p < a->ptr[r + 1]; p++) s += a->val[p] * x[a->idx[p]]; y[r] = s; }
}
static void spmv_csc(const Comp *a, const long *x, long *y) {
    for (int r = 0; r < a->rows; r++) y[r] = 0;
    for (int c = 0; c < a->cols; c++) for (int p = a->ptr[c]; p < a->ptr[c + 1]; p++) y[a->idx[p]] += a->val[p] * x[c];
}

int main(void) {
    for (int trial = 0; trial < 4; trial++) {
        int raw = 30 + trial * 150;
        Coo m; m.n = raw; m.t = malloc((size_t)raw * sizeof(Triple));
        static long dense[R][C]; memset(dense, 0, sizeof dense);
        for (int i = 0; i < raw; i++) {
            m.t[i].r = (int)(rnd() % R); m.t[i].c = (int)(rnd() % C); m.t[i].v = (long)(rnd() % 19) - 9;
            dense[m.t[i].r][m.t[i].c] += m.t[i].v;
        }
        int dups = 0;
        coo_canon(&m);
        long nz = 0; for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) nz += dense[r][c] != 0;
        check(m.n == nz, "canonical nnz equals dense nonzeros");
        for (int i = 0; i < m.n; i++) check(dense[m.t[i].r][m.t[i].c] == m.t[i].v, "canonical value");
        dups = raw - m.n;
        Comp csr = build(&m, 1), csc = build(&m, 0), csc2 = csr_to_csc(&csr);
        check(comp_equal(&csc, &csc2, C), "CSC via COO equals CSC via CSR pass");
        /* transpose is a format swap: CSR of A^T has the arrays of CSC of A */
        Coo tm; tm.n = m.n; tm.t = malloc((size_t)(m.n + 1) * sizeof(Triple));
        for (int i = 0; i < m.n; i++) { tm.t[i].r = m.t[i].c; tm.t[i].c = m.t[i].r; tm.t[i].v = m.t[i].v; }
        qsort(tm.t, (size_t)tm.n, sizeof(Triple), cmp_rc);
        long xs[C], ys1[R], ys2[R], ys3[R];
        for (int c = 0; c < C; c++) xs[c] = (long)(rnd() % 11) - 5;
        spmv_csr(&csr, xs, ys1); spmv_csc(&csc, xs, ys2);
        unsigned long long chk = 0;
        for (int r = 0; r < R; r++) {
            long s = 0; for (int c = 0; c < C; c++) s += dense[r][c] * xs[c];
            ys3[r] = s; check(ys1[r] == s && ys2[r] == s, "spmv");
            chk = (chk * 7u + (unsigned long long)(s + 1000)) % 1000003u;
        }
        /* lookups by binary search in a CSR row */
        int probes = 0;
        for (int q = 0; q < 500; q++) {
            int r = (int)(rnd() % R), c = (int)(rnd() % C);
            int lo = csr.ptr[r], hi = csr.ptr[r + 1];
            while (lo < hi) { int mid = (lo + hi) / 2; if (csr.idx[mid] < c) lo = mid + 1; else hi = mid; }
            long got = (lo < csr.ptr[r + 1] && csr.idx[lo] == c) ? csr.val[lo] : 0;
            check(got == dense[r][c], "CSR point lookup");
            probes += got != 0;
        }
        int maxrow = 0, emptyrows = 0;
        for (int r = 0; r < R; r++) { int d = csr.ptr[r + 1] - csr.ptr[r]; if (d > maxrow) maxrow = d; emptyrows += d == 0; }
        long bytes_dense = (long)R * C * 8, bytes_csr = (long)(R + 1) * 4 + csr.nnz * 12L;
        printf("raw=%3d merged_dups=%3d nnz=%3d max_row=%2d empty_rows=%2d hits=%3d chk=%llu csr_bytes=%ld/%ld\n",
               raw, dups, m.n, maxrow, emptyrows, probes, chk, bytes_csr, bytes_dense);
        (void)ys3;
        comp_free(&csr); comp_free(&csc); comp_free(&csc2); free(m.t); free(tm.t);
    }
    return 0;
}
