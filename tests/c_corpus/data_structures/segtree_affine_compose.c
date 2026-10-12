/*
 * title: Iterative segment tree composing affine maps
 * topic: data_structures
 * covers: non-commutative monoid, affine function composition modulo prime, bottom-up query with left and right accumulators
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

#define MOD 998244353ULL

typedef struct {
    unsigned long long a, b; /* x -> a*x + b (mod MOD) */
} Aff;

/* apply f first, then g */
static Aff then(Aff f, Aff g) {
    Aff r = {g.a * f.a % MOD, (g.a * f.b + g.b) % MOD};
    return r;
}

static const Aff ID = {1, 0};

#define MAXN 300
static Aff tr[2 * MAXN];
static int n_;

static void build(const Aff *f, int n) {
    n_ = n;
    for (int i = 0; i < n; i++)
        tr[n + i] = f[i];
    for (int i = n - 1; i >= 1; i--)
        tr[i] = then(tr[2 * i], tr[2 * i + 1]);
}

static void set(int i, Aff f) {
    i += n_;
    tr[i] = f;
    for (i >>= 1; i >= 1; i >>= 1)
        tr[i] = then(tr[2 * i], tr[2 * i + 1]);
}

/* composition of f[l], f[l+1], ..., f[r-1] applied in that order */
static Aff query(int l, int r) {
    Aff left = ID, right = ID;
    for (l += n_, r += n_; l < r; l >>= 1, r >>= 1) {
        if (l & 1)
            left = then(left, tr[l++]);
        if (r & 1)
            right = then(tr[--r], right);
    }
    return then(left, right);
}

static unsigned long long eval(Aff f, unsigned long long x) { return (f.a * x + f.b) % MOD; }

static Aff rand_aff(void) {
    Aff f;
    f.a = rnd(1000000);
    f.b = rnd(1000000);
    return f;
}

int main(void) {
    int sizes[] = {1, 2, 5, 17, 100, 300};
    for (int si = 0; si < 6; si++) {
        int n = sizes[si];
        Aff f[MAXN];
        for (int i = 0; i < n; i++)
            f[i] = rand_aff();
        build(f, n);
        unsigned long long acc = 0;
        int nq = 0;
        for (int step = 0; step < 1200; step++) {
            if (rnd(4) == 0) {
                int i = (int)rnd((unsigned)n);
                f[i] = rand_aff();
                set(i, f[i]);
                continue;
            }
            int l = (int)rnd((unsigned)n + 1), r = (int)rnd((unsigned)n + 1);
            if (l > r) {
                int t = l;
                l = r;
                r = t;
            }
            unsigned long long x = rnd(1000000);
            unsigned long long want = x;
            for (int i = l; i < r; i++)
                want = eval(f[i], want);
            unsigned long long got = eval(query(l, r), x);
            check(got == want, "composition");
            acc = (acc * 31 + got) % MOD;
            nq++;
        }
        printf("n=%d queries=%d digest=%llu\n", n, nq, acc);
    }
    /* order sensitivity: f = x+1, g = 2x */
    Aff pair[2] = {{1, 1}, {2, 0}};
    build(pair, 2);
    Aff fg = query(0, 2);
    pair[0].a = 2;
    pair[0].b = 0;
    pair[1].a = 1;
    pair[1].b = 1;
    build(pair, 2);
    Aff gf = query(0, 2);
    check(eval(fg, 5) == 12 && eval(gf, 5) == 11, "order matters");
    printf("(x+1 then 2x)(5)=%llu, (2x then x+1)(5)=%llu\n", eval(fg, 5), eval(gf, 5));
    return 0;
}
