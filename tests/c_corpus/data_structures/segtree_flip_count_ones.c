/*
 * title: Lazy segment tree with range flip and k-th set bit
 * topic: data_structures
 * covers: lazy involution tags, range flip, count ones, k-th one descent, bit array brute force
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

#define MAXN 400
static int ones[4 * MAXN];
static unsigned char flip[4 * MAXN];
static int flips_pushed;

static void pull(int o) { ones[o] = ones[2 * o] + ones[2 * o + 1]; }

static void apply(int o, int l, int r) {
    ones[o] = (r - l + 1) - ones[o];
    flip[o] ^= 1;
}

static void push(int o, int l, int r) {
    if (flip[o]) {
        int m = (l + r) / 2;
        apply(2 * o, l, m);
        apply(2 * o + 1, m + 1, r);
        flip[o] = 0;
        flips_pushed++;
    }
}

static void build(int o, int l, int r, const unsigned char *bits) {
    flip[o] = 0;
    if (l == r) {
        ones[o] = bits[l];
        return;
    }
    int m = (l + r) / 2;
    build(2 * o, l, m, bits);
    build(2 * o + 1, m + 1, r, bits);
    pull(o);
}

static void flip_range(int o, int l, int r, int ql, int qr) {
    if (qr < l || r < ql)
        return;
    if (ql <= l && r <= qr) {
        apply(o, l, r);
        return;
    }
    push(o, l, r);
    int m = (l + r) / 2;
    flip_range(2 * o, l, m, ql, qr);
    flip_range(2 * o + 1, m + 1, r, ql, qr);
    pull(o);
}

static int count(int o, int l, int r, int ql, int qr) {
    if (qr < l || r < ql)
        return 0;
    if (ql <= l && r <= qr)
        return ones[o];
    push(o, l, r);
    int m = (l + r) / 2;
    return count(2 * o, l, m, ql, qr) + count(2 * o + 1, m + 1, r, ql, qr);
}

/* position of the k-th (1-based) set bit, or -1 */
static int kth_one(int o, int l, int r, int k) {
    if (k > ones[o])
        return -1;
    if (l == r)
        return l;
    push(o, l, r);
    int m = (l + r) / 2;
    if (k <= ones[2 * o])
        return kth_one(2 * o, l, m, k);
    return kth_one(2 * o + 1, m + 1, r, k - ones[2 * o]);
}

int main(void) {
    int n = 383;
    unsigned char bits[MAXN];
    for (int i = 0; i < n; i++)
        bits[i] = (unsigned char)(rnd(2));
    build(1, 0, n - 1, bits);
    long csum = 0, ksum = 0;
    int nflip = 0, nq = 0, nk = 0;
    for (int step = 0; step < 6000; step++) {
        int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        unsigned op = rnd(3);
        if (op == 0) {
            flip_range(1, 0, n - 1, l, r);
            for (int i = l; i <= r; i++)
                bits[i] ^= 1;
            nflip++;
        } else if (op == 1) {
            int want = 0;
            for (int i = l; i <= r; i++)
                want += bits[i];
            int got = count(1, 0, n - 1, l, r);
            check(got == want, "count ones");
            csum += got;
            nq++;
        } else {
            int total = 0;
            for (int i = 0; i < n; i++)
                total += bits[i];
            int k = 1 + (int)rnd((unsigned)total + 2);
            int want = -1, seen = 0;
            for (int i = 0; i < n; i++)
                if (bits[i] && ++seen == k) {
                    want = i;
                    break;
                }
            int got = kth_one(1, 0, n - 1, k);
            check(got == want, "k-th one");
            ksum += got;
            nk++;
        }
    }
    printf("flips=%d counts=%d kth=%d\n", nflip, nq, nk);
    printf("count sum=%ld kth sum=%ld pushes=%d\n", csum, ksum, flips_pushed);
    printf("ones now=%d of %d\n", ones[1], n);
    return 0;
}
