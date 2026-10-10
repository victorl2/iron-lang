/*
 * title: Order-statistic multiset on a Fenwick tree
 * topic: data_structures
 * covers: binary lifting descent, k-th element, rank, predecessor, successor, running median, brute force counts
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

#define U 1024 /* universe [0, U) */

static int bit[U + 1];
static int cnt[U]; /* brute force multiplicities */
static int size_;

static void add(int v, int d) {
    for (int i = v + 1; i <= U; i += i & -i)
        bit[i] += d;
    cnt[v] += d;
    size_ += d;
}

/* number of elements <= v */
static int rank_le(int v) {
    int s = 0;
    for (int i = v + 1; i > 0; i -= i & -i)
        s += bit[i];
    return s;
}

/* k-th smallest (1-based) by binary lifting; -1 if k out of range */
static int kth(int k) {
    if (k < 1 || k > size_)
        return -1;
    int pos = 0;
    for (int step = U; step > 0; step >>= 1)
        if (pos + step <= U && bit[pos + step] < k) {
            pos += step;
            k -= bit[pos];
        }
    return pos; /* value = pos (0-based) */
}

static int pred(int v) { /* largest element < v */
    int r = rank_le(v - 1);
    return r == 0 ? -1 : kth(r);
}

static int succ(int v) { /* smallest element > v */
    int r = rank_le(v);
    return r == size_ ? -1 : kth(r + 1);
}

static int brute_kth(int k) {
    for (int v = 0; v < U; v++) {
        if (k <= cnt[v])
            return v;
        k -= cnt[v];
    }
    return -1;
}

int main(void) {
    long ksum = 0, psum = 0, ssum = 0;
    int ins = 0, del = 0;
    for (int step = 0; step < 8000; step++) {
        unsigned op = rnd(8);
        int v = (int)rnd(U);
        if (op < 3) {
            add(v, 1);
            ins++;
        } else if (op < 5) {
            if (cnt[v] > 0) {
                add(v, -1);
                del++;
            }
        } else if (op == 5) {
            if (size_ > 0) {
                int k = 1 + (int)rnd((unsigned)size_);
                int got = kth(k);
                check(got == brute_kth(k), "kth");
                ksum += got;
            }
        } else if (op == 6) {
            int want = -1;
            for (int u = v - 1; u >= 0; u--)
                if (cnt[u]) {
                    want = u;
                    break;
                }
            int got = pred(v);
            check(got == want, "pred");
            psum += got;
        } else {
            int want = -1;
            for (int u = v + 1; u < U; u++)
                if (cnt[u]) {
                    want = u;
                    break;
                }
            int got = succ(v);
            check(got == want, "succ");
            ssum += got;
            int rk = 0;
            for (int u = 0; u <= v; u++)
                rk += cnt[u];
            check(rank_le(v) == rk, "rank");
        }
    }
    printf("inserts=%d deletes=%d size=%d\n", ins, del, size_);
    printf("kth digest=%ld pred digest=%ld succ digest=%ld\n", ksum, psum, ssum);
    check(kth(0) == -1 && kth(size_ + 1) == -1, "out of range kth");

    /* running median of a stream via k-th */
    memset(bit, 0, sizeof bit);
    memset(cnt, 0, sizeof cnt);
    size_ = 0;
    long med_sum = 0;
    int last_med = 0;
    for (int i = 1; i <= 2000; i++) {
        int x = (int)rnd(500) + (i / 10);
        if (x >= U)
            x = U - 1;
        add(x, 1);
        int m = kth((size_ + 1) / 2);
        check(m == brute_kth((size_ + 1) / 2), "median");
        med_sum += m;
        last_med = m;
    }
    printf("running median sum=%ld last=%d\n", med_sum, last_med);
    return 0;
}
