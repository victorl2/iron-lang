/*
 * title: Matrix exponentiation for linear recurrences and path counting
 * topic: algorithms
 * covers: matrix power by squaring, companion matrices, Fibonacci, tribonacci, walks in graphs, modular arithmetic, fast doubling
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
#define MOD 1000000007ull
#define MAXD 8

typedef struct { int n; u64 a[MAXD][MAXD]; } Mat;

static Mat ident(int n) { Mat m; memset(&m, 0, sizeof m); m.n = n; for (int i = 0; i < n; i++) m.a[i][i] = 1; return m; }

static Mat mul(const Mat *x, const Mat *y) {
    Mat r;
    memset(&r, 0, sizeof r);
    r.n = x->n;
    for (int i = 0; i < x->n; i++)
        for (int k = 0; k < x->n; k++) {
            if (!x->a[i][k]) continue;
            for (int j = 0; j < x->n; j++) r.a[i][j] = (r.a[i][j] + x->a[i][k] * y->a[k][j]) % MOD;
        }
    return r;
}

static Mat mpow(Mat b, u64 e, int *mults) {
    Mat r = ident(b.n);
    while (e) {
        if (e & 1) { r = mul(&r, &b); (*mults)++; }
        b = mul(&b, &b);
        (*mults)++;
        e >>= 1;
    }
    return r;
}

/* fast doubling Fibonacci mod MOD: returns F(n), F(n+1) */
static void fib_fd(u64 n, u64 *f, u64 *g) {
    if (n == 0) { *f = 0; *g = 1; return; }
    u64 a, b;
    fib_fd(n / 2, &a, &b);
    u64 c = a * ((2 * b + MOD - a) % MOD) % MOD;
    u64 d = (a * a + b * b) % MOD;
    if (n & 1) { *f = d; *g = (c + d) % MOD; }
    else { *f = c; *g = d; }
}

/* nth term of a linear recurrence with given coefficients: x_n = sum c[i] x_{n-1-i} */
static u64 recurrence(const u64 *c, const u64 *init, int d, u64 n) {
    if (n < (u64)d) return init[n] % MOD;
    Mat m;
    memset(&m, 0, sizeof m);
    m.n = d;
    for (int j = 0; j < d; j++) m.a[0][j] = c[j] % MOD;
    for (int i = 1; i < d; i++) m.a[i][i - 1] = 1;
    int mults = 0;
    Mat p = mpow(m, n - (u64)d + 1, &mults);
    u64 r = 0;
    for (int j = 0; j < d; j++) r = (r + p.a[0][j] * init[d - 1 - j]) % MOD;
    return r;
}

int main(void) {
    u64 fibc[2] = {1, 1}, fibi[2] = {0, 1};
    u64 ns[] = {1, 10, 50, 90, 1000, 1000000, 1000000000000ull, 1000000000000000000ull};
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        u64 f, g;
        fib_fd(ns[i], &f, &g);
        u64 m = recurrence(fibc, fibi, 2, ns[i]);
        if (f != m) { fprintf(stderr, "fib mismatch\n"); return 1; }
        printf("F(%llu) mod 1e9+7 = %llu\n", ns[i], f);
    }
    /* tribonacci and a 4th order recurrence checked against direct iteration */
    u64 tc[3] = {1, 1, 1}, ti[3] = {0, 0, 1};
    u64 qc[4] = {2, 0, 3, MOD - 1}, qi[4] = {1, 2, 3, 4};
    u64 t[300], q[300];
    for (int i = 0; i < 3; i++) t[i] = ti[i];
    for (int i = 3; i < 300; i++) t[i] = (t[i - 1] + t[i - 2] + t[i - 3]) % MOD;
    for (int i = 0; i < 4; i++) q[i] = qi[i];
    for (int i = 4; i < 300; i++) q[i] = (2 * q[i - 1] + 3 * q[i - 3] + (MOD - 1) * q[i - 4]) % MOD;
    for (u64 i = 0; i < 300; i += 37) {
        if (recurrence(tc, ti, 3, i) != t[i]) { fprintf(stderr, "trib mismatch %llu\n", i); return 1; }
        if (recurrence(qc, qi, 4, i) != q[i]) { fprintf(stderr, "quad mismatch %llu\n", i); return 1; }
    }
    printf("tribonacci T(299) = %llu, T(10^18) = %llu\n", t[299], recurrence(tc, ti, 3, 1000000000000000000ull));
    printf("4th order q(299) = %llu, q(10^15) = %llu\n", q[299], recurrence(qc, qi, 4, 1000000000000000ull));

    /* number of walks of length L in a 5-cycle with a chord */
    Mat g;
    memset(&g, 0, sizeof g);
    g.n = 5;
    int edges[][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 0}, {0, 2}};
    for (size_t i = 0; i < 6; i++) { g.a[edges[i][0]][edges[i][1]] = 1; g.a[edges[i][1]][edges[i][0]] = 1; }
    int mults = 0;
    for (u64 L = 1; L <= 4; L++) {
        Mat p = mpow(g, L, &mults);
        u64 tot = 0;
        for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) tot += p.a[i][j];
        printf("closed walks length %llu: trace %llu, total walks %llu\n", L, (p.a[0][0] + p.a[1][1] + p.a[2][2] + p.a[3][3] + p.a[4][4]), tot);
    }
    mults = 0;
    Mat big = mpow(g, 1000000000000ull, &mults);
    printf("walks 0->4 of length 10^12: %llu using %d matrix products\n", big.a[0][4], mults);
    /* 2x2 tilings: ways to tile 2 x n board with dominoes = F(n+1) */
    u64 f, gg;
    fib_fd(31, &f, &gg);
    printf("domino tilings of 2x30 board: %llu\n", f);
    /* linear-time DP check of the fast doubling identity F(2n)=F(n)(2F(n+1)-F(n)) at n=300 */
    u64 a = 0, b = 1;
    for (int i = 0; i < 300; i++) { u64 c = (a + b) % MOD; a = b; b = c; }
    u64 f300, f301;
    fib_fd(300, &f300, &f301);
    if (a != f300 || b != f301) return 1;
    printf("F(300) mod p = %llu\n", f300);
    return 0;
}
