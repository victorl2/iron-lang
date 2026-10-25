/*
 * title: Gaussian elimination over a prime field
 * topic: algorithms
 * covers: row reduction mod p, rank, determinant, matrix inverse, solution space of linear systems, free variables, modular inverses
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long i64;
#define P 1000003ll
#define MAXN 12

static i64 powmod(i64 b, i64 e) {
    i64 r = 1;
    b %= P; if (b < 0) b += P;
    while (e) { if (e & 1) r = r * b % P; b = b * b % P; e >>= 1; }
    return r;
}
static i64 inv(i64 a) { return powmod(a, P - 2); }
static i64 md(i64 a) { a %= P; return a < 0 ? a + P : a; }

typedef struct { int n, m; i64 a[MAXN][2 * MAXN + 1]; } Mat;

/* reduce to reduced row echelon form using the first `cols` columns for pivots; returns rank, sign tracks swaps */
static int rref(Mat *M, int cols, int *pivcol, int *swaps) {
    int r = 0;
    *swaps = 0;
    for (int c = 0; c < cols && r < M->n; c++) {
        int piv = -1;
        for (int i = r; i < M->n; i++) if (M->a[i][c]) { piv = i; break; }
        if (piv < 0) continue;
        if (piv != r) {
            for (int j = 0; j < M->m; j++) { i64 t = M->a[piv][j]; M->a[piv][j] = M->a[r][j]; M->a[r][j] = t; }
            (*swaps)++;
        }
        i64 iv = inv(M->a[r][c]);
        for (int j = 0; j < M->m; j++) M->a[r][j] = M->a[r][j] * iv % P;
        for (int i = 0; i < M->n; i++) {
            if (i == r || !M->a[i][c]) continue;
            i64 f = M->a[i][c];
            for (int j = 0; j < M->m; j++) M->a[i][j] = md(M->a[i][j] - f * M->a[r][j]);
        }
        pivcol[r++] = c;
    }
    return r;
}

static i64 determinant(const i64 *flat, int n) {
    i64 a[MAXN][MAXN];
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) a[i][j] = md(flat[i * n + j]);
    i64 det = 1;
    for (int c = 0; c < n; c++) {
        int piv = -1;
        for (int i = c; i < n; i++) if (a[i][c]) { piv = i; break; }
        if (piv < 0) return 0;
        if (piv != c) { for (int j = 0; j < n; j++) { i64 t = a[piv][j]; a[piv][j] = a[c][j]; a[c][j] = t; } det = md(-det); }
        det = det * a[c][c] % P;
        i64 iv = inv(a[c][c]);
        for (int i = c + 1; i < n; i++) {
            i64 f = a[i][c] * iv % P;
            for (int j = c; j < n; j++) a[i][j] = md(a[i][j] - f * a[c][j]);
        }
    }
    return det;
}

static unsigned st = 2463534242u;
static unsigned rnd(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }

int main(void) {
    /* determinant of Vandermonde matrices vs product formula */
    for (int n = 2; n <= 8; n += 2) {
        i64 flat[MAXN * MAXN], xs[MAXN];
        for (int i = 0; i < n; i++) xs[i] = (i64)(rnd() % 1000) + 1;
        for (int i = 0; i < n; i++) { i64 v = 1; for (int j = 0; j < n; j++) { flat[i * n + j] = v; v = v * xs[i] % P; } }
        i64 want = 1;
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) want = want * md(xs[j] - xs[i]) % P;
        i64 got = determinant(flat, n);
        printf("Vandermonde n=%d det=%lld formula=%lld\n", n, got, want);
        if (got != want) { fprintf(stderr, "vandermonde mismatch\n"); return 1; }
    }
    /* inverse of a random matrix: [A | I] -> [I | A^-1], verify A * A^-1 = I */
    int inv_ok = 0, singular = 0;
    for (int trial = 0; trial < 30; trial++) {
        int n = 5;
        Mat M;
        M.n = n; M.m = 2 * n;
        for (int i = 0; i < n; i++) for (int j = 0; j < 2 * n; j++) M.a[i][j] = j < n ? (i64)(rnd() % 7) : (j - n == i);
        i64 orig[MAXN][MAXN];
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) orig[i][j] = M.a[i][j];
        int piv[MAXN], sw;
        int rk = rref(&M, n, piv, &sw);
        if (rk < n) { singular++; continue; }
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
            i64 s = 0;
            for (int k = 0; k < n; k++) s = (s + orig[i][k] * M.a[k][n + j]) % P;
            if (s != (i == j)) { fprintf(stderr, "inverse wrong\n"); return 1; }
        }
        inv_ok++;
    }
    printf("random 5x5 matrices with entries in 0..6: %d inverted, %d singular\n", inv_ok, singular);

    /* consistent system with a unique solution */
    {
        Mat M;
        i64 x[4] = {3, 1, 4, 1};
        M.n = 4; M.m = 5;
        for (int i = 0; i < 4; i++) {
            i64 rhs = 0;
            for (int j = 0; j < 4; j++) { M.a[i][j] = (i64)(rnd() % 100); rhs += M.a[i][j] * x[j]; }
            M.a[i][4] = rhs % P;
        }
        int piv[MAXN], sw;
        int rk = rref(&M, 4, piv, &sw);
        printf("unique system rank %d, solution:", rk);
        for (int i = 0; i < 4; i++) printf(" %lld", M.a[i][4]);
        printf("\n");
        for (int i = 0; i < 4; i++) if (M.a[i][4] != x[i]) return 1;
    }
    /* underdetermined 3x5 system: report pivots and a particular solution */
    {
        Mat M;
        i64 base[3][6] = {{1, 2, 0, 3, 1, 5}, {2, 4, 1, 7, 2, 12}, {3, 6, 1, 10, 3, 17}};
        M.n = 3; M.m = 6;
        for (int i = 0; i < 3; i++) for (int j = 0; j < 6; j++) M.a[i][j] = base[i][j];
        int piv[MAXN], sw;
        int rk = rref(&M, 5, piv, &sw);
        printf("3x5 system rank %d, pivots at columns:", rk);
        for (int i = 0; i < rk; i++) printf(" %d", piv[i]);
        printf("\nfree variables: %d\n", 5 - rk);
        i64 sol[5] = {0};
        for (int i = 0; i < rk; i++) sol[piv[i]] = M.a[i][5];
        for (int i = 0; i < 3; i++) {
            i64 s = 0;
            for (int j = 0; j < 5; j++) s += base[i][j] * sol[j];
            if (md(s) != md(base[i][5])) { fprintf(stderr, "particular solution fails\n"); return 1; }
        }
        printf("particular solution:");
        for (int j = 0; j < 5; j++) printf(" %lld", sol[j]);
        printf("\n");
    }
    /* inconsistent system detection */
    {
        Mat M;
        i64 base[2][4] = {{1, 1, 1, 2}, {2, 2, 2, 5}};
        M.n = 2; M.m = 4;
        for (int i = 0; i < 2; i++) for (int j = 0; j < 4; j++) M.a[i][j] = base[i][j];
        int piv[MAXN], sw;
        int rk = rref(&M, 3, piv, &sw);
        int bad = 0;
        for (int i = rk; i < 2; i++) if (M.a[i][3]) bad = 1;
        printf("inconsistent system detected: %s (rank %d)\n", bad ? "yes" : "no", rk);
        if (!bad) return 1;
    }
    /* rank of a 6x6 matrix built as a sum of 3 outer products is at most 3 */
    {
        Mat M;
        M.n = 6; M.m = 6;
        for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) M.a[i][j] = 0;
        for (int t = 0; t < 3; t++) {
            i64 u[6], v[6];
            for (int i = 0; i < 6; i++) { u[i] = (i64)(rnd() % 50); v[i] = (i64)(rnd() % 50); }
            for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) M.a[i][j] = (M.a[i][j] + u[i] * v[j]) % P;
        }
        int piv[MAXN], sw;
        int rk = rref(&M, 6, piv, &sw);
        printf("rank of sum of three outer products: %d\n", rk);
        if (rk > 3) return 1;
    }
    return 0;
}
