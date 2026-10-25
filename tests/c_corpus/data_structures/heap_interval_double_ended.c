/*
 * title: Interval heap double-ended priority queue
 * topic: data_structures
 * covers: interval heap, double-ended priority queue, paired min and max chains, bounded smallest-k window
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 606060;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Flat array: node k holds a[2k] (low end) and a[2k+1] (high end). The last node may hold one item. */
enum { CAP = 1024 };
typedef struct {
    int a[CAP];
    int n;
} IH;

static void swp(IH *h, int i, int j) {
    int t = h->a[i];
    h->a[i] = h->a[j];
    h->a[j] = t;
}

static void up_lo(IH *h, int k) {
    while (k > 0) {
        int p = (k - 1) / 2;
        if (h->a[2 * k] >= h->a[2 * p])
            break;
        swp(h, 2 * k, 2 * p);
        k = p;
    }
}

static void up_hi(IH *h, int k) {
    while (k > 0) {
        int p = (k - 1) / 2;
        int hi = 2 * k + 1 < h->n ? 2 * k + 1 : 2 * k;
        if (h->a[hi] <= h->a[2 * p + 1])
            break;
        swp(h, hi, 2 * p + 1);
        k = p;
    }
}

static void insert(IH *h, int x) {
    check(h->n < CAP, "capacity");
    int i = h->n++, k = i / 2;
    h->a[i] = x;
    if (i & 1) {
        if (h->a[i] < h->a[i - 1])
            swp(h, i, i - 1);
        up_lo(h, k);
        up_hi(h, k);
    } else if (k > 0) {
        int p = (k - 1) / 2;
        if (x < h->a[2 * p]) {
            swp(h, i, 2 * p);
            up_lo(h, p);
        } else if (x > h->a[2 * p + 1]) {
            swp(h, i, 2 * p + 1);
            up_hi(h, p);
        }
    }
}

static int min_of(const IH *h) { return h->a[0]; }
static int max_of(const IH *h) { return h->n >= 2 ? h->a[1] : h->a[0]; }

static void down_lo(IH *h, int k) {
    for (;;) {
        if (2 * k + 1 < h->n && h->a[2 * k] > h->a[2 * k + 1])
            swp(h, 2 * k, 2 * k + 1);
        int best = -1;
        for (int c = 2 * k + 1; c <= 2 * k + 2; c++)
            if (2 * c < h->n && (best < 0 || h->a[2 * c] < h->a[2 * best]))
                best = c;
        if (best < 0 || h->a[2 * best] >= h->a[2 * k])
            return;
        swp(h, 2 * k, 2 * best);
        k = best;
    }
}

static void down_hi(IH *h, int k) {
    for (;;) {
        if (2 * k + 1 >= h->n)
            return;
        if (h->a[2 * k] > h->a[2 * k + 1])
            swp(h, 2 * k, 2 * k + 1);
        int best = -1, bv = 0;
        for (int c = 2 * k + 1; c <= 2 * k + 2; c++) {
            if (2 * c >= h->n)
                continue;
            int idx = 2 * c + 1 < h->n ? 2 * c + 1 : 2 * c;
            if (best < 0 || h->a[idx] > bv) {
                best = idx;
                bv = h->a[idx];
            }
        }
        if (best < 0 || bv <= h->a[2 * k + 1])
            return;
        swp(h, 2 * k + 1, best);
        k = best / 2;
    }
}

static int pop_min(IH *h) {
    int v = h->a[0];
    h->n--;
    if (h->n > 0) {
        h->a[0] = h->a[h->n];
        down_lo(h, 0);
    }
    return v;
}

static int pop_max(IH *h) {
    if (h->n == 1) {
        h->n = 0;
        return h->a[0];
    }
    int v = h->a[1];
    h->n--;
    if (h->n > 1) {
        h->a[1] = h->a[h->n];
        down_hi(h, 0);
    }
    return v;
}

static void invariant(const IH *h) {
    int nodes = (h->n + 1) / 2;
    for (int k = 0; k < nodes; k++) {
        int lo = h->a[2 * k], hi = 2 * k + 1 < h->n ? h->a[2 * k + 1] : lo;
        check(lo <= hi, "node interval ordered");
        if (k > 0) {
            int p = (k - 1) / 2;
            check(lo >= h->a[2 * p] && hi <= h->a[2 * p + 1], "child interval nested in parent interval");
        }
    }
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    static IH h;
    int model[CAP], mn = 0, pmin = 0, pmax = 0;
    long smin = 0, smax = 0;
    for (int op = 0; op < 9000; op++) {
        int r = (int)(rng() % 100);
        if ((r < 56 || mn == 0) && mn < CAP - 1) {
            int v = (int)(rng() % 600);
            model[mn++] = v;
            insert(&h, v);
        } else {
            int bi = 0, bx = 0;
            for (int i = 1; i < mn; i++) {
                if (model[i] < model[bi])
                    bi = i;
                if (model[i] > model[bx])
                    bx = i;
            }
            check(min_of(&h) == model[bi] && max_of(&h) == model[bx], "peek both ends");
            int want, got;
            if (r < 75) {
                want = model[bi];
                model[bi] = model[--mn];
                got = pop_min(&h);
                smin += got;
                pmin++;
            } else {
                want = model[bx];
                model[bx] = model[--mn];
                got = pop_max(&h);
                smax += got;
                pmax++;
            }
            check(got == want, "pop matches model");
        }
        check(h.n == mn, "size");
        invariant(&h);
    }
    printf("size=%d pop_min=%d pop_max=%d sum_min=%ld sum_max=%ld\n", h.n, pmin, pmax, smin, smax);

    /* keep the 40 smallest of a stream: insert, then evict the maximum when over capacity */
    IH w = {{0}, 0};
    static int stream[3000];
    for (int i = 0; i < 3000; i++) {
        stream[i] = (int)(rng() % 100000);
        insert(&w, stream[i]);
        if (w.n > 40)
            pop_max(&w);
        invariant(&w);
    }
    qsort(stream, 3000, sizeof(int), cmp_int);
    printf("smallest40:");
    for (int i = 0; i < 40; i++) {
        int v = pop_min(&w);
        check(v == stream[i], "bounded window equals 40 smallest");
        if (i < 6)
            printf(" %d", v);
    }
    printf(" ...\n");
    return 0;
}
