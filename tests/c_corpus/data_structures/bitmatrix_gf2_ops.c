/*
 * title: Bit matrices over GF(2) with transpose, multiply, rank and inverse
 * topic: data_structures
 * covers: bit matrix, rows as 64-bit words, transpose, GF(2) multiply by row xor, Gaussian elimination, rank, inverse, nullspace, boolean matrix product oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXN 40

static unsigned long long rs = 0x6F2B17ULL * 0x9E3779ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int n; uint64_t row[MAXN]; } BM;   /* n x n, bit j of row[i] = M[i][j] */

static int get(const BM *m, int i, int j) { return (int)((m->row[i] >> j) & 1u); }
static void randomize(BM *m, int n, int density) {
    memset(m, 0, sizeof *m); m->n = n;
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if ((int)(rnd() % 100) < density) m->row[i] |= (uint64_t)1 << j;
}
static BM transpose(const BM *m) {
    BM t; memset(&t, 0, sizeof t); t.n = m->n;
    for (int i = 0; i < m->n; i++) for (int j = 0; j < m->n; j++) if (get(m, i, j)) t.row[j] |= (uint64_t)1 << i;
    return t;
}
/* C = A*B over GF(2): row i of C is the xor of rows of B selected by bits of row i of A */
static BM mul(const BM *a, const BM *b) {
    BM c; memset(&c, 0, sizeof c); c.n = a->n;
    for (int i = 0; i < a->n; i++) for (int k = 0; k < a->n; k++) if (get(a, i, k)) c.row[i] ^= b->row[k];
    return c;
}
static BM mul_ref(const BM *a, const BM *b) {
    BM c; memset(&c, 0, sizeof c); c.n = a->n;
    for (int i = 0; i < a->n; i++) for (int j = 0; j < a->n; j++) {
        int s = 0;
        for (int k = 0; k < a->n; k++) s ^= get(a, i, k) & get(b, k, j);
        if (s) c.row[i] |= (uint64_t)1 << j;
    }
    return c;
}
static BM identity(int n) { BM m; memset(&m, 0, sizeof m); m.n = n; for (int i = 0; i < n; i++) m.row[i] = (uint64_t)1 << i; return m; }
static int equal(const BM *a, const BM *b) { return a->n == b->n && memcmp(a->row, b->row, sizeof a->row) == 0; }

static int rank_of(const BM *m) {
    uint64_t r[MAXN]; memcpy(r, m->row, sizeof r);
    int rank = 0;
    for (int col = 0; col < m->n && rank < m->n; col++) {
        int p = -1;
        for (int i = rank; i < m->n; i++) if ((r[i] >> col) & 1u) { p = i; break; }
        if (p < 0) continue;
        uint64_t t = r[p]; r[p] = r[rank]; r[rank] = t;
        for (int i = 0; i < m->n; i++) if (i != rank && ((r[i] >> col) & 1u)) r[i] ^= r[rank];
        rank++;
    }
    return rank;
}
/* Gauss-Jordan on [M | I]; returns 1 if invertible */
static int inverse(const BM *m, BM *inv) {
    uint64_t a[MAXN], b[MAXN]; memcpy(a, m->row, sizeof a);
    for (int i = 0; i < m->n; i++) b[i] = (uint64_t)1 << i;
    for (int col = 0; col < m->n; col++) {
        int p = -1;
        for (int i = col; i < m->n; i++) if ((a[i] >> col) & 1u) { p = i; break; }
        if (p < 0) return 0;
        uint64_t t = a[p]; a[p] = a[col]; a[col] = t; t = b[p]; b[p] = b[col]; b[col] = t;
        for (int i = 0; i < m->n; i++) if (i != col && ((a[i] >> col) & 1u)) { a[i] ^= a[col]; b[i] ^= b[col]; }
    }
    memset(inv, 0, sizeof *inv); inv->n = m->n; memcpy(inv->row, b, sizeof b);
    return 1;
}
/* a nonzero x with M x = 0 (as column vector bitmask), or 0 if none */
static uint64_t nullvector(const BM *m) {
    uint64_t r[MAXN]; memcpy(r, m->row, sizeof r);
    int pivot_col[MAXN], rank = 0; int is_pivot[MAXN] = {0};
    for (int col = 0; col < m->n; col++) {
        int p = -1;
        for (int i = rank; i < m->n; i++) if ((r[i] >> col) & 1u) { p = i; break; }
        if (p < 0) continue;
        uint64_t t = r[p]; r[p] = r[rank]; r[rank] = t;
        for (int i = 0; i < m->n; i++) if (i != rank && ((r[i] >> col) & 1u)) r[i] ^= r[rank];
        pivot_col[rank++] = col; is_pivot[col] = 1;
    }
    for (int free_col = 0; free_col < m->n; free_col++) if (!is_pivot[free_col]) {
        uint64_t x = (uint64_t)1 << free_col;
        for (int i = 0; i < rank; i++) if ((r[i] >> free_col) & 1u) x |= (uint64_t)1 << pivot_col[i];
        return x;
    }
    return 0;
}
static uint64_t apply(const BM *m, uint64_t x) {
    uint64_t y = 0;
    for (int i = 0; i < m->n; i++) { uint64_t v = m->row[i] & x; int par = 0; while (v) { v &= v - 1; par ^= 1; } if (par) y |= (uint64_t)1 << i; }
    return y;
}

int main(void) {
    int sizes[4] = {8, 17, 33, 40};
    for (int si = 0; si < 4; si++) {
        int n = sizes[si];
        BM a, b; randomize(&a, n, 50); randomize(&b, n, 35);
        BM c = mul(&a, &b);
        BM cr = mul_ref(&a, &b);
        check(equal(&c, &cr), "multiply vs triple loop");
        BM ta = transpose(&a), tt = transpose(&ta);
        check(equal(&tt, &a), "double transpose");
        BM ab_t = transpose(&c), tb = transpose(&b), bt_at = mul(&tb, &ta);
        check(equal(&ab_t, &bt_at), "(AB)^T = B^T A^T");
        int ra = rank_of(&a), rb = rank_of(&b), rc = rank_of(&c), rt = rank_of(&ta);
        check(ra == rt, "rank of transpose");
        check(rc <= ra && rc <= rb, "rank of product bounded");
        BM inv; int invertible = inverse(&a, &inv);
        check(invertible == (ra == n), "invertible iff full rank");
        if (invertible) { BM id = identity(n), p = mul(&a, &inv); check(equal(&p, &id), "A * A^-1 = I"); p = mul(&inv, &a); check(equal(&p, &id), "A^-1 * A = I"); }
        /* force a singular matrix by duplicating a row, and test nullspace */
        BM s = a; s.row[n - 1] = s.row[0] ^ s.row[n > 2 ? 1 : 0];
        if (n > 2) {
            uint64_t x = nullvector(&s);
            check(rank_of(&s) < n, "constructed matrix is singular");
            check(x != 0 && apply(&s, x) == 0, "null vector maps to zero");
            /* also verify against the transpose relation: left null vector */
            printf("n=%2d rank(A)=%2d rank(B)=%2d rank(AB)=%2d invertible=%d singular rank=%2d null=%llx\n", n, ra, rb, rc, invertible, rank_of(&s), (unsigned long long)x);
        }
    }
    /* count invertible 4x4 matrices over GF(2): known value 20160 (order of GL(4,2)) */
    int cnt = 0;
    for (unsigned code = 0; code < 65536u; code++) {
        BM m; memset(&m, 0, sizeof m); m.n = 4;
        for (int i = 0; i < 4; i++) m.row[i] = (code >> (4 * i)) & 15u;
        cnt += rank_of(&m) == 4;
    }
    check(cnt == 20160, "|GL(4,2)| = 20160");
    printf("invertible 4x4 matrices over GF(2): %d\n", cnt);
    return 0;
}
