/*
 * title: Iterative segment tree over pluggable monoids
 * topic: data_structures
 * covers: bottom-up segment tree, function pointers, monoid identity, point update, range query, brute force
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

typedef long (*CombineFn)(long, long);

typedef struct {
    const char *name;
    long identity;
    CombineFn op;
} Monoid;

typedef struct {
    int n;
    long *t;
    const Monoid *m;
} SegTree;

static long op_sum(long a, long b) { return a + b; }
static long op_min(long a, long b) { return a < b ? a : b; }
static long op_max(long a, long b) { return a > b ? a : b; }
static long op_xor(long a, long b) { return a ^ b; }
static long op_gcd(long a, long b) {
    while (b) {
        long t = a % b;
        a = b;
        b = t;
    }
    return a;
}
static long op_mulmod(long a, long b) { return (a * b) % 1000003; }

static const Monoid MONOIDS[] = {
    {"sum", 0, op_sum},      {"min", 1000000000L, op_min}, {"max", -1000000000L, op_max},
    {"xor", 0, op_xor},      {"gcd", 0, op_gcd},           {"mulmod", 1, op_mulmod},
};

static void st_init(SegTree *s, int n, const Monoid *m, const long *init) {
    s->n = n;
    s->m = m;
    s->t = malloc(sizeof(long) * (size_t)(2 * n));
    check(s->t != NULL, "alloc");
    for (int i = 0; i < n; i++)
        s->t[n + i] = init[i];
    for (int i = n - 1; i >= 1; i--)
        s->t[i] = m->op(s->t[2 * i], s->t[2 * i + 1]);
}

static void st_set(SegTree *s, int i, long v) {
    i += s->n;
    s->t[i] = v;
    for (i >>= 1; i >= 1; i >>= 1)
        s->t[i] = s->m->op(s->t[2 * i], s->t[2 * i + 1]);
}

/* half-open [l, r); non-power-of-two n works because ops are commutative */
static long st_query(const SegTree *s, int l, int r) {
    long lo = s->m->identity, hi = s->m->identity;
    for (l += s->n, r += s->n; l < r; l >>= 1, r >>= 1) {
        if (l & 1)
            lo = s->m->op(lo, s->t[l++]);
        if (r & 1)
            hi = s->m->op(s->t[--r], hi);
    }
    return s->m->op(lo, hi);
}

static long brute(const Monoid *m, const long *a, int l, int r) {
    long acc = m->identity;
    for (int i = l; i < r; i++)
        acc = m->op(acc, a[i]);
    return acc;
}

int main(void) {
    int sizes[] = {1, 2, 7, 64, 100, 257};
    for (size_t mi = 0; mi < sizeof MONOIDS / sizeof MONOIDS[0]; mi++) {
        const Monoid *m = &MONOIDS[mi];
        long checksum = 0;
        int nq = 0;
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            int n = sizes[si];
            long *a = malloc(sizeof(long) * (size_t)n);
            check(a != NULL, "alloc");
            for (int i = 0; i < n; i++)
                a[i] = 1 + (long)rnd(1000);
            SegTree s;
            st_init(&s, n, m, a);
            check(st_query(&s, 0, n) == brute(m, a, 0, n), "full range");
            for (int step = 0; step < 300; step++) {
                if (rnd(3) == 0) {
                    int i = (int)rnd((unsigned)n);
                    long v = 1 + (long)rnd(1000);
                    a[i] = v;
                    st_set(&s, i, v);
                } else {
                    int l = (int)rnd((unsigned)n + 1);
                    int r = (int)rnd((unsigned)n + 1);
                    if (l > r) {
                        int t = l;
                        l = r;
                        r = t;
                    }
                    long got = st_query(&s, l, r);
                    check(got == brute(m, a, l, r), "range query");
                    checksum = (checksum * 31 + got) % 1000000007L;
                    nq++;
                }
            }
            free(a);
            free(s.t);
        }
        printf("%-7s queries=%d checksum=%ld\n", m->name, nq, checksum);
    }
    return 0;
}
