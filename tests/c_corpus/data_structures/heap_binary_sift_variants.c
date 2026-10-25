/*
 * title: Binary heap with three sift-down strategies
 * topic: data_structures
 * covers: binary heap, sift up, sift down, hole method, bottom-up sift, comparison counting, model cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static unsigned rng(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1Dull) >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int *a;
    int n;
    int cap;
    long cmp;
} Heap;

typedef void (*SiftDown)(Heap *h, int i);

static int less(Heap *h, int x, int y) {
    h->cmp++;
    return x < y;
}

/* classic: swap with the smaller child until in place */
static void down_swap(Heap *h, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->n && less(h, h->a[l], h->a[m]))
            m = l;
        if (r < h->n && less(h, h->a[r], h->a[m]))
            m = r;
        if (m == i)
            return;
        int t = h->a[i];
        h->a[i] = h->a[m];
        h->a[m] = t;
        i = m;
    }
}

/* hole method: carry the element, move children up into the hole */
static void down_hole(Heap *h, int i) {
    int x = h->a[i];
    for (;;) {
        int l = 2 * i + 1;
        if (l >= h->n)
            break;
        int c = l;
        if (l + 1 < h->n && less(h, h->a[l + 1], h->a[l]))
            c = l + 1;
        if (!less(h, h->a[c], x))
            break;
        h->a[i] = h->a[c];
        i = c;
    }
    h->a[i] = x;
}

/* bottom-up: follow the smaller-child path to a leaf, then climb back */
static void down_bottom_up(Heap *h, int i) {
    int x = h->a[i], top = i;
    for (;;) {
        int l = 2 * i + 1;
        if (l >= h->n)
            break;
        int c = l;
        if (l + 1 < h->n && less(h, h->a[l + 1], h->a[l]))
            c = l + 1;
        h->a[i] = h->a[c];
        i = c;
    }
    while (i > top) {
        int p = (i - 1) / 2;
        if (!less(h, x, h->a[p]))
            break;
        h->a[i] = h->a[p];
        i = p;
    }
    h->a[i] = x;
}

static void up_hole(Heap *h, int i) {
    int x = h->a[i];
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!less(h, x, h->a[p]))
            break;
        h->a[i] = h->a[p];
        i = p;
    }
    h->a[i] = x;
}

static void hpush(Heap *h, int v) {
    check(h->n < h->cap, "capacity");
    h->a[h->n++] = v;
    up_hole(h, h->n - 1);
}

static int hpop(Heap *h, SiftDown sd) {
    int top = h->a[0];
    h->a[0] = h->a[--h->n];
    if (h->n > 0)
        sd(h, 0);
    return top;
}

static void invariant(const Heap *h) {
    for (int i = 1; i < h->n; i++)
        check(h->a[(i - 1) / 2] <= h->a[i], "heap order");
}

int main(void) {
    enum { CAP = 300, OPS = 4000 };
    SiftDown fn[3] = {down_swap, down_hole, down_bottom_up};
    const char *name[3] = {"swap", "hole", "bottom_up"};
    Heap h[3];
    long sumpop[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        h[k].a = malloc(sizeof(int) * CAP);
        h[k].n = 0;
        h[k].cap = CAP;
        h[k].cmp = 0;
    }
    int model[CAP], mn = 0;
    int pushes = 0, pops = 0;
    for (int op = 0; op < OPS; op++) {
        int doPush = mn == 0 || (mn < CAP && rng() % 100 < 52);
        if (doPush) {
            int v = (int)(rng() % 1000);
            model[mn++] = v;
            for (int k = 0; k < 3; k++)
                hpush(&h[k], v);
            pushes++;
        } else {
            int bi = 0;
            for (int i = 1; i < mn; i++)
                if (model[i] < model[bi])
                    bi = i;
            int want = model[bi];
            model[bi] = model[--mn];
            for (int k = 0; k < 3; k++) {
                int got = hpop(&h[k], fn[k]);
                check(got == want, "pop equals model minimum");
                sumpop[k] += got;
            }
            pops++;
        }
        for (int k = 0; k < 3; k++) {
            check(h[k].n == mn, "size");
            invariant(&h[k]);
        }
    }
    printf("pushes=%d pops=%d final_size=%d\n", pushes, pops, mn);
    for (int k = 0; k < 3; k++)
        printf("%-9s comparisons=%ld pop_sum=%ld\n", name[k], h[k].cmp, sumpop[k]);
    check(sumpop[0] == sumpop[1] && sumpop[1] == sumpop[2], "same pop sums");
    check(h[2].cmp <= h[0].cmp, "bottom-up does not exceed swap variant");
    for (int k = 0; k < 3; k++)
        free(h[k].a);
    return 0;
}
