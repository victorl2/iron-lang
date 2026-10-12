/*
 * title: Sparse tables for idempotent queries
 * topic: data_structures
 * covers: sparse table, overlapping power-of-two blocks, min, max, gcd, bitwise and, argmin index table, log table
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define MAXN 1024
#define LOG 11

typedef unsigned (*Op)(unsigned, unsigned);

static unsigned op_min(unsigned a, unsigned b) { return a < b ? a : b; }
static unsigned op_max(unsigned a, unsigned b) { return a > b ? a : b; }
static unsigned op_and(unsigned a, unsigned b) { return a & b; }
static unsigned op_gcd(unsigned a, unsigned b) {
    while (b) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int lg[MAXN + 1];

typedef struct {
    int n;
    unsigned t[LOG][MAXN];
    Op op;
} Sparse;

static void build(Sparse *s, const unsigned *a, int n, Op op) {
    s->n = n;
    s->op = op;
    memcpy(s->t[0], a, sizeof(unsigned) * (size_t)n);
    for (int k = 1; (1 << k) <= n; k++)
        for (int i = 0; i + (1 << k) <= n; i++)
            s->t[k][i] = op(s->t[k - 1][i], s->t[k - 1][i + (1 << (k - 1))]);
}

/* inclusive [l, r]: two overlapping blocks are fine because op is idempotent */
static unsigned query(const Sparse *s, int l, int r) {
    int k = lg[r - l + 1];
    return s->op(s->t[k][l], s->t[k][r - (1 << k) + 1]);
}

/* argmin table stores indices; ties go to the leftmost */
static int amin[LOG][MAXN];

static int better(const unsigned *a, int i, int j) {
    if (a[i] != a[j])
        return a[i] < a[j] ? i : j;
    return i < j ? i : j;
}

static void build_arg(const unsigned *a, int n) {
    for (int i = 0; i < n; i++)
        amin[0][i] = i;
    for (int k = 1; (1 << k) <= n; k++)
        for (int i = 0; i + (1 << k) <= n; i++)
            amin[k][i] = better(a, amin[k - 1][i], amin[k - 1][i + (1 << (k - 1))]);
}

static int query_arg(const unsigned *a, int l, int r) {
    int k = lg[r - l + 1];
    return better(a, amin[k][l], amin[k][r - (1 << k) + 1]);
}

static Sparse tables[4];

int main(void) {
    lg[1] = 0;
    for (int i = 2; i <= MAXN; i++)
        lg[i] = lg[i / 2] + 1;
    const char *names[4] = {"min", "max", "and", "gcd"};
    Op ops[4] = {op_min, op_max, op_and, op_gcd};
    int n = 1000;
    unsigned a[MAXN];
    for (int i = 0; i < n; i++) {
        a[i] = 1 + rnd(1000000);
        if (i % 3 == 0) {
            unsigned mult = 1 + rnd(5);
            a[i] = 720720u * mult * (1 + (unsigned)(i % 2)); /* many shared factors */
        }
    }
    for (int t = 0; t < 4; t++) {
        unsigned src[MAXN];
        for (int i = 0; i < n; i++) {
            if (t == 2) {
                unsigned b1 = rnd(16);
                unsigned b2 = rnd(16);
                src[i] = 0xFFFFu & ~((1u << b1) | (1u << b2));
            } else
                src[i] = a[i];
        }
        build(&tables[t], src, n, ops[t]);
        unsigned long long digest = 0;
        for (int q = 0; q < 3000; q++) {
            int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
            if (l > r) {
                int x = l;
                l = r;
                r = x;
            }
            unsigned want = src[l];
            for (int i = l + 1; i <= r; i++)
                want = ops[t](want, src[i]);
            unsigned got = query(&tables[t], l, r);
            check(got == want, "sparse query");
            digest = digest * 1000003ULL + got;
        }
        printf("%-3s digest=%llu\n", names[t], digest);
    }
    build_arg(a, n);
    long isum = 0;
    for (int q = 0; q < 3000; q++) {
        int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
        if (l > r) {
            int x = l;
            l = r;
            r = x;
        }
        int want = l;
        for (int i = l + 1; i <= r; i++)
            if (a[i] < a[want])
                want = i;
        check(query_arg(a, l, r) == want, "argmin");
        isum += want;
    }
    printf("argmin index sum=%ld\n", isum);
    int levels = 0;
    while ((1 << levels) <= n)
        levels++;
    printf("levels=%d table cells=%d\n", levels, levels * n);
    return 0;
}
