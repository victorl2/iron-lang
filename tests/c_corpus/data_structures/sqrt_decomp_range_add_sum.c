/*
 * title: Square root decomposition with lazy blocks
 * topic: data_structures
 * covers: sqrt decomposition, block lazy add, block sum and max, partial block rebuild, block size sweep, brute force
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

typedef struct {
    int n, bs, nb;
    long *a;
    long *lazy, *sum, *mx;
    long touched; /* elements visited individually */
} Sq;

static void rebuild_block(Sq *s, int b) {
    int lo = b * s->bs, hi = lo + s->bs < s->n ? lo + s->bs : s->n;
    long sm = 0, m = s->a[lo];
    for (int i = lo; i < hi; i++) {
        sm += s->a[i];
        if (s->a[i] > m)
            m = s->a[i];
    }
    s->sum[b] = sm + s->lazy[b] * (hi - lo);
    s->mx[b] = m + s->lazy[b];
}

static void sq_init(Sq *s, const long *src, int n, int bs) {
    s->n = n;
    s->bs = bs;
    s->nb = (n + bs - 1) / bs;
    s->touched = 0;
    s->a = malloc(sizeof(long) * (size_t)n);
    s->lazy = calloc((size_t)s->nb, sizeof(long));
    s->sum = calloc((size_t)s->nb, sizeof(long));
    s->mx = calloc((size_t)s->nb, sizeof(long));
    check(s->a && s->lazy && s->sum && s->mx, "alloc");
    memcpy(s->a, src, sizeof(long) * (size_t)n);
    for (int b = 0; b < s->nb; b++)
        rebuild_block(s, b);
}

static void sq_free(Sq *s) {
    free(s->a);
    free(s->lazy);
    free(s->sum);
    free(s->mx);
}

static int block_len(const Sq *s, int b) {
    int lo = b * s->bs, hi = lo + s->bs < s->n ? lo + s->bs : s->n;
    return hi - lo;
}

/* inclusive [l, r] */
static void sq_add(Sq *s, int l, int r, long v) {
    int bl = l / s->bs, br = r / s->bs;
    if (bl == br) {
        for (int i = l; i <= r; i++)
            s->a[i] += v;
        s->touched += r - l + 1;
        rebuild_block(s, bl);
        return;
    }
    for (int i = l; i < (bl + 1) * s->bs; i++)
        s->a[i] += v, s->touched++;
    rebuild_block(s, bl);
    for (int b = bl + 1; b < br; b++) {
        s->lazy[b] += v;
        s->sum[b] += v * block_len(s, b);
        s->mx[b] += v;
    }
    for (int i = br * s->bs; i <= r; i++)
        s->a[i] += v, s->touched++;
    rebuild_block(s, br);
}

static long sq_sum(Sq *s, int l, int r) {
    int bl = l / s->bs, br = r / s->bs;
    long res = 0;
    if (bl == br) {
        for (int i = l; i <= r; i++)
            res += s->a[i] + s->lazy[bl];
        s->touched += r - l + 1;
        return res;
    }
    for (int i = l; i < (bl + 1) * s->bs; i++)
        res += s->a[i] + s->lazy[bl], s->touched++;
    for (int b = bl + 1; b < br; b++)
        res += s->sum[b];
    for (int i = br * s->bs; i <= r; i++)
        res += s->a[i] + s->lazy[br], s->touched++;
    return res;
}

static long sq_max(Sq *s, int l, int r) {
    int bl = l / s->bs, br = r / s->bs;
    long res = s->a[l] + s->lazy[bl];
    for (int i = l; i <= r; i++) {
        if (i % s->bs == 0 && i + s->bs - 1 <= r) {
            int b = i / s->bs;
            if (s->mx[b] > res)
                res = s->mx[b];
            i += s->bs - 1;
            continue;
        }
        long x = s->a[i] + s->lazy[i / s->bs];
        s->touched++;
        if (x > res)
            res = x;
    }
    (void)br;
    return res;
}

int main(void) {
    int n = 1000;
    int sizes[] = {1, 7, 32, 100, 1000};
    for (int si = 0; si < 5; si++) {
        long src[1000], ref[1000];
        for (int i = 0; i < n; i++)
            src[i] = ref[i] = (long)rnd(1000) - 500;
        Sq s;
        sq_init(&s, src, n, sizes[si]);
        long digest = 0;
        for (int step = 0; step < 3000; step++) {
            int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
            if (l > r) {
                int t = l;
                l = r;
                r = t;
            }
            unsigned op = rnd(3);
            if (op == 0) {
                long v = (long)rnd(101) - 50;
                sq_add(&s, l, r, v);
                for (int i = l; i <= r; i++)
                    ref[i] += v;
            } else if (op == 1) {
                long want = 0;
                for (int i = l; i <= r; i++)
                    want += ref[i];
                long got = sq_sum(&s, l, r);
                check(got == want, "sum");
                digest = (digest * 31 + got) % 1000000007L;
            } else {
                long want = ref[l];
                for (int i = l; i <= r; i++)
                    if (ref[i] > want)
                        want = ref[i];
                long got = sq_max(&s, l, r);
                check(got == want, "max");
                digest = (digest * 31 + got) % 1000000007L;
            }
        }
        printf("block size %-4d blocks=%-4d digest=%ld elements touched=%ld\n", sizes[si], s.nb, digest,
               s.touched);
        sq_free(&s);
    }
    return 0;
}
