/*
 * title: Wavelet matrix with bit-vector rank and select
 * topic: data_structures
 * covers: wavelet matrix, succinct bit vectors with popcount rank tables, select by binary search, quantile, count-less, predecessor and successor, access
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

#define BITS 10
#define N 2000
typedef unsigned long long u64;

typedef struct {
    u64 *w;
    int *cum; /* ones before each 64-bit word */
    int n;
} BitVec;

static int popcount64(u64 x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static void bv_build(BitVec *b, const unsigned char *bits, int n) {
    int nw = n / 64 + 1;
    b->n = n;
    b->w = calloc((size_t)nw, sizeof(u64));
    b->cum = calloc((size_t)nw + 1, sizeof(int));
    check(b->w && b->cum, "alloc");
    for (int i = 0; i < n; i++)
        if (bits[i])
            b->w[i / 64] |= 1ULL << (i % 64);
    for (int i = 0; i < nw; i++)
        b->cum[i + 1] = b->cum[i] + popcount64(b->w[i]);
}

static int bv_rank1(const BitVec *b, int i) { /* ones in [0, i) */
    u64 mask = (i % 64) ? ((1ULL << (i % 64)) - 1) : 0;
    return b->cum[i / 64] + popcount64(b->w[i / 64] & mask);
}

static int bv_get(const BitVec *b, int i) { return (int)((b->w[i / 64] >> (i % 64)) & 1); }

/* position of the k-th (0-based) bit equal to `bit` */
static int bv_select(const BitVec *b, int bit, int k) {
    int lo = 0, hi = b->n - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int cnt = bit ? bv_rank1(b, mid + 1) : (mid + 1) - bv_rank1(b, mid + 1);
        if (cnt > k)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static BitVec lv[BITS];
static int zeros[BITS];

static void build(const int *src) {
    int cur[N], nxt[N];
    unsigned char bits[N];
    memcpy(cur, src, sizeof cur);
    for (int d = 0; d < BITS; d++) {
        int sh = BITS - 1 - d, nz = 0;
        for (int i = 0; i < N; i++) {
            bits[i] = (unsigned char)((cur[i] >> sh) & 1);
            nz += !bits[i];
        }
        bv_build(&lv[d], bits, N);
        zeros[d] = nz;
        int pz = 0, po = nz;
        for (int i = 0; i < N; i++)
            nxt[bits[i] ? po++ : pz++] = cur[i];
        memcpy(cur, nxt, sizeof cur);
    }
}

static int access(int i) {
    int v = 0;
    for (int d = 0; d < BITS; d++) {
        int bit = bv_get(&lv[d], i);
        v = v * 2 + bit;
        i = bit ? zeros[d] + bv_rank1(&lv[d], i) : i - bv_rank1(&lv[d], i);
    }
    return v;
}

/* number of values < x among positions [l, r) */
static int count_less(int l, int r, int x) {
    if (x >= (1 << BITS))
        return r - l;
    int res = 0;
    for (int d = 0; d < BITS; d++) {
        int l1 = bv_rank1(&lv[d], l), r1 = bv_rank1(&lv[d], r);
        if ((x >> (BITS - 1 - d)) & 1) {
            res += (r - l) - (r1 - l1); /* the zero side is entirely smaller */
            l = zeros[d] + l1;
            r = zeros[d] + r1;
        } else {
            l -= l1;
            r -= r1;
        }
    }
    return res;
}

static int quantile(int l, int r, int k) { /* k-th smallest, 0-based */
    int v = 0;
    for (int d = 0; d < BITS; d++) {
        int l1 = bv_rank1(&lv[d], l), r1 = bv_rank1(&lv[d], r);
        int nz = (r - l) - (r1 - l1);
        if (k < nz) {
            l -= l1;
            r -= r1;
            v = v * 2;
        } else {
            k -= nz;
            l = zeros[d] + l1;
            r = zeros[d] + r1;
            v = v * 2 + 1;
        }
    }
    return v;
}

static int count_eq(int l, int r, int v) { return count_less(l, r, v + 1) - count_less(l, r, v); }

/* position of the k-th (0-based) occurrence of v in the whole array, or -1 */
static int select_v(int v, int k) {
    if (k >= count_eq(0, N, v))
        return -1;
    int p = 0;
    for (int d = 0; d < BITS; d++) { /* where does v's block start at the bottom? */
        int bit = (v >> (BITS - 1 - d)) & 1;
        int r1 = bv_rank1(&lv[d], p);
        p = bit ? zeros[d] + r1 : p - r1;
    }
    int q = p + k;
    for (int d = BITS - 1; d >= 0; d--) {
        int bit = (v >> (BITS - 1 - d)) & 1;
        q = bit ? bv_select(&lv[d], 1, q - zeros[d]) : bv_select(&lv[d], 0, q);
    }
    return q;
}

int main(void) {
    static int a[N];
    for (int i = 0; i < N; i++)
        a[i] = (int)rnd(1u << BITS);
    for (int i = 0; i < N; i += 11)
        a[i] = 77; /* a frequent value for select */
    build(a);
    for (int i = 0; i < N; i++)
        check(access(i) == a[i], "access");
    long qsum = 0, csum = 0, nsum = 0, psum = 0;
    for (int q = 0; q < 2000; q++) {
        int l = (int)rnd(N), r = (int)rnd(N + 1);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        if (l == r)
            continue;
        int len = r - l;
        int x = (int)rnd(1u << BITS);
        int less = 0;
        for (int i = l; i < r; i++)
            less += a[i] < x;
        check(count_less(l, r, x) == less, "count_less");
        csum += less;
        int k = (int)rnd((unsigned)len);
        /* k-th smallest by brute force */
        int kk = -1, cnt_smaller = 0;
        for (int v = 0; v < (1 << BITS) && kk < 0; v++) {
            int c = 0;
            for (int i = l; i < r; i++)
                c += a[i] == v;
            if (k < cnt_smaller + c)
                kk = v;
            cnt_smaller += c;
        }
        check(quantile(l, r, k) == kk, "quantile");
        qsum += kk;
        /* successor: smallest value >= x ; predecessor: largest value < x */
        int succ = -1, pred = -1;
        for (int i = l; i < r; i++) {
            if (a[i] >= x && (succ < 0 || a[i] < succ))
                succ = a[i];
            if (a[i] < x && a[i] > pred)
                pred = a[i];
        }
        int c = count_less(l, r, x);
        int gs = c == len ? -1 : quantile(l, r, c);
        int gp = c == 0 ? -1 : quantile(l, r, c - 1);
        check(gs == succ && gp == pred, "successor and predecessor");
        nsum += gs;
        psum += gp;
    }
    printf("count_less digest=%ld quantile digest=%ld successor digest=%ld predecessor digest=%ld\n", csum,
           qsum, nsum, psum);
    int f77 = 0;
    for (int i = 0; i < N; i++)
        f77 += a[i] == 77;
    for (int k = 0; k < f77; k += 5) {
        int want = -1, seen = 0;
        for (int i = 0; i < N; i++)
            if (a[i] == 77 && seen++ == k) {
                want = i;
                break;
            }
        check(select_v(77, k) == want, "select");
    }
    check(select_v(77, f77) == -1, "select past the end");
    printf("value 77 occurs %d times; positions of occurrences #0, #3, #last: %d %d %d\n", f77,
           select_v(77, 0), select_v(77, 3), select_v(77, f77 - 1));
    for (int d = 0; d < BITS; d++) {
        free(lv[d].w);
        free(lv[d].cum);
    }
    return 0;
}
