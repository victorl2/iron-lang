/*
 * title: Min-max heap
 * topic: data_structures
 * covers: min-max heap, double-ended priority queue, alternating levels, grandparent bubbling, trickle down
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 1618033;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 30);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

enum { CAP = 512 };
typedef struct {
    int a[CAP];
    int n;
} MM;

static int is_min_level(int i) {
    int lvl = 0;
    for (i = i + 1; i > 1; i >>= 1)
        lvl++;
    return lvl % 2 == 0;
}

static void swap(MM *h, int i, int j) {
    int t = h->a[i];
    h->a[i] = h->a[j];
    h->a[j] = t;
}

static void bubble_up_min(MM *h, int i) {
    while (i >= 3) {
        int gp = ((i - 1) / 2 - 1) / 2;
        if (h->a[i] >= h->a[gp])
            break;
        swap(h, i, gp);
        i = gp;
    }
}

static void bubble_up_max(MM *h, int i) {
    while (i >= 3) {
        int gp = ((i - 1) / 2 - 1) / 2;
        if (h->a[i] <= h->a[gp])
            break;
        swap(h, i, gp);
        i = gp;
    }
}

static void push(MM *h, int v) {
    check(h->n < CAP, "capacity");
    int i = h->n++;
    h->a[i] = v;
    if (i == 0)
        return;
    int p = (i - 1) / 2;
    if (is_min_level(i)) {
        if (h->a[i] > h->a[p]) {
            swap(h, i, p);
            bubble_up_max(h, p);
        } else
            bubble_up_min(h, i);
    } else {
        if (h->a[i] < h->a[p]) {
            swap(h, i, p);
            bubble_up_min(h, p);
        } else
            bubble_up_max(h, i);
    }
}

/* index of the extreme (min or max) among children and grandchildren of i, or -1 */
static int extreme_desc(const MM *h, int i, int want_min) {
    int best = -1;
    int cand[6] = {2 * i + 1, 2 * i + 2, 4 * i + 3, 4 * i + 4, 4 * i + 5, 4 * i + 6};
    for (int k = 0; k < 6; k++) {
        int c = cand[k];
        if (c >= h->n)
            continue;
        if (best < 0 || (want_min ? h->a[c] < h->a[best] : h->a[c] > h->a[best]))
            best = c;
    }
    return best;
}

static void trickle_down(MM *h, int i) {
    int want_min = is_min_level(i);
    while (2 * i + 1 < h->n) {
        int m = extreme_desc(h, i, want_min);
        int better = want_min ? h->a[m] < h->a[i] : h->a[m] > h->a[i];
        if (m >= 4 * i + 3) { /* grandchild */
            if (!better)
                break;
            swap(h, m, i);
            int p = (m - 1) / 2;
            if (want_min ? h->a[m] > h->a[p] : h->a[m] < h->a[p])
                swap(h, m, p);
            i = m;
        } else {
            if (better)
                swap(h, m, i);
            break;
        }
    }
}

static int find_max(const MM *h) {
    if (h->n == 1)
        return 0;
    if (h->n == 2)
        return 1;
    return h->a[1] > h->a[2] ? 1 : 2;
}

static int pop_min(MM *h) {
    int v = h->a[0];
    h->a[0] = h->a[--h->n];
    if (h->n > 0)
        trickle_down(h, 0);
    return v;
}

static int pop_max(MM *h) {
    int mi = find_max(h), v = h->a[mi];
    h->a[mi] = h->a[--h->n];
    if (mi < h->n)
        trickle_down(h, mi);
    return v;
}

static void inv(const MM *h, int i, int lo, int hi) {
    if (i >= h->n)
        return;
    check(h->a[i] >= lo && h->a[i] <= hi, "min-max ordering against ancestors");
    if (is_min_level(i)) {
        inv(h, 2 * i + 1, h->a[i], hi);
        inv(h, 2 * i + 2, h->a[i], hi);
    } else {
        inv(h, 2 * i + 1, lo, h->a[i]);
        inv(h, 2 * i + 2, lo, h->a[i]);
    }
}

int main(void) {
    static MM h;
    int model[CAP], mn = 0;
    int pushes = 0, pmin = 0, pmax = 0;
    long sum_min = 0, sum_max = 0;
    for (int op = 0; op < 8000; op++) {
        int r = (int)(rng() % 100);
        if ((r < 50 || mn == 0) && mn < CAP) {
            int v = (int)(rng() % 400) - 200;
            model[mn++] = v;
            push(&h, v);
            pushes++;
        } else {
            int bi = 0, bx = 0;
            for (int i = 1; i < mn; i++) {
                if (model[i] < model[bi])
                    bi = i;
                if (model[i] > model[bx])
                    bx = i;
            }
            if (r < 75) {
                int want = model[bi];
                model[bi] = model[--mn];
                int got = pop_min(&h);
                check(got == want, "pop_min");
                sum_min += got;
                pmin++;
            } else {
                int want = model[bx];
                model[bx] = model[--mn];
                int got = pop_max(&h);
                check(got == want, "pop_max");
                sum_max += got;
                pmax++;
            }
        }
        check(h.n == mn, "size");
        if (mn > 0) {
            inv(&h, 0, -1000000, 1000000);
            int lo = h.a[0], hi = h.a[find_max(&h)];
            for (int i = 0; i < mn; i++)
                check(model[i] >= lo && model[i] <= hi, "root and max bound everything");
        }
    }
    printf("pushes=%d pop_min=%d pop_max=%d size=%d\n", pushes, pmin, pmax, h.n);
    printf("sum_min=%ld sum_max=%ld\n", sum_min, sum_max);
    /* alternate draining from both ends yields a sorted sequence from outside in */
    int lo = -1000000, hi = 1000000, taken = 0;
    while (h.n > 0) {
        int a = pop_min(&h);
        check(a >= lo, "lo side non-decreasing");
        lo = a;
        taken++;
        if (h.n == 0)
            break;
        int b = pop_max(&h);
        check(b <= hi && b >= a, "hi side non-increasing and above lo");
        hi = b;
        taken++;
    }
    printf("drained=%d lo_end=%d hi_end=%d\n", taken, lo, hi);
    return 0;
}
