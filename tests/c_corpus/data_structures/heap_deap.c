/*
 * title: Deap double-ended priority queue
 * topic: data_structures
 * covers: deap, min-heap and max-heap in one array, partner index mapping, double-ended priority queue
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 8675309;
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

/* Slots 2..n are used; slot 1 is the empty root. Subtree of 2 is a min-heap, subtree of 3 a max-heap. */
enum { CAP = 1030 };
typedef struct {
    int a[CAP];
    int n; /* last used index; size is n-1 */
} Deap;

static int level(int i) {
    int l = 0;
    while (i > 1) {
        i >>= 1;
        l++;
    }
    return l;
}

static int in_min_side(int i) { return (i >> (level(i) - 1)) == 2; }

/* partner of i in the opposite half; if that slot is beyond n, its parent */
static int partner(const Deap *d, int i) {
    int half = 1 << (level(i) - 1);
    int j = in_min_side(i) ? i + half : i - half;
    if (j > d->n)
        j /= 2;
    return j;
}

static void swp(Deap *d, int i, int j) {
    int t = d->a[i];
    d->a[i] = d->a[j];
    d->a[j] = t;
}

static void bubble_min(Deap *d, int i) {
    while (i / 2 >= 2 && d->a[i] < d->a[i / 2]) {
        swp(d, i, i / 2);
        i /= 2;
    }
}

static void bubble_max(Deap *d, int i) {
    while (i / 2 >= 3 && d->a[i] > d->a[i / 2]) {
        swp(d, i, i / 2);
        i /= 2;
    }
}

/* value at i just placed; restore the partner condition and heap order */
static void settle(Deap *d, int i) {
    if (i < 2)
        return;
    int j = partner(d, i);
    if (in_min_side(i)) {
        if (j >= 2 && d->a[i] > d->a[j]) {
            swp(d, i, j);
            bubble_min(d, i);
            bubble_max(d, j);
        } else
            bubble_min(d, i);
    } else {
        /* min-side children of the partner may use i as their fallback partner too */
        int m = j;
        for (int c = 2 * j; c <= 2 * j + 1; c++)
            if (c <= d->n && partner(d, c) == i && d->a[c] > d->a[m])
                m = c;
        if (d->a[i] < d->a[m]) {
            swp(d, i, m);
            bubble_max(d, i);
            bubble_min(d, m);
        } else
            bubble_max(d, i);
    }
}

static void insert(Deap *d, int v) {
    check(d->n + 1 < CAP, "capacity");
    if (d->n == 0)
        d->n = 1;
    d->a[++d->n] = v;
    settle(d, d->n);
}

static int size(const Deap *d) { return d->n < 2 ? 0 : d->n - 1; }
static int min_of(const Deap *d) { return d->a[2]; }
static int max_of(const Deap *d) { return d->n >= 3 ? d->a[3] : d->a[2]; }

static int pop_min(Deap *d) {
    int v = d->a[2];
    if (d->n == 2) {
        d->n = 1;
        return v;
    }
    int x = d->a[d->n--];
    int i = 2;
    for (;;) {
        int l = 2 * i, r = l + 1, c = l;
        if (l > d->n)
            break;
        if (r <= d->n && d->a[r] < d->a[l])
            c = r;
        d->a[i] = d->a[c];
        i = c;
    }
    d->a[i] = x;
    settle(d, i);
    return v;
}

static int pop_max(Deap *d) {
    if (d->n == 2) {
        d->n = 1;
        return d->a[2];
    }
    int v = d->a[3];
    int x = d->a[d->n--];
    if (d->n < 3)
        return v;
    int i = 3;
    for (;;) {
        int l = 2 * i, r = l + 1, c = l;
        if (l > d->n)
            break;
        if (r <= d->n && d->a[r] > d->a[l])
            c = r;
        d->a[i] = d->a[c];
        i = c;
    }
    d->a[i] = x;
    settle(d, i);
    return v;
}

static void invariant(const Deap *d) {
    for (int i = 2; i <= d->n; i++) {
        if (i / 2 >= 2)
            check(in_min_side(i) ? d->a[i / 2] <= d->a[i] : d->a[i / 2] >= d->a[i], "half heap order");
        if (in_min_side(i)) {
            int j = partner(d, i);
            if (j >= 2)
                check(d->a[i] <= d->a[j], "min-side value <= partner");
        }
    }
}

int main(void) {
    static Deap d;
    int model[CAP], mn = 0, pmin = 0, pmax = 0;
    long smin = 0, smax = 0;
    for (int op = 0; op < 8000; op++) {
        int r = (int)(rng() % 100);
        if ((r < 54 || mn == 0) && mn < CAP - 4) {
            int v = (int)(rng() % 700) - 100;
            model[mn++] = v;
            insert(&d, v);
        } else {
            int bi = 0, bx = 0;
            for (int i = 1; i < mn; i++) {
                if (model[i] < model[bi])
                    bi = i;
                if (model[i] > model[bx])
                    bx = i;
            }
            check(min_of(&d) == model[bi] && max_of(&d) == model[bx], "peeks");
            int got, want;
            if (r < 77) {
                want = model[bi];
                model[bi] = model[--mn];
                got = pop_min(&d);
                smin += got;
                pmin++;
            } else {
                want = model[bx];
                model[bx] = model[--mn];
                got = pop_max(&d);
                smax += got;
                pmax++;
            }
            check(got == want, "pop equals model");
        }
        check(size(&d) == mn, "size");
        invariant(&d);
    }
    printf("size=%d pop_min=%d pop_max=%d sum_min=%ld sum_max=%ld\n", size(&d), pmin, pmax, smin, smax);
    printf("levels=%d partner(4)=%d partner(7)=%d partner(13)=%d\n", level(d.n), partner(&d, 4), partner(&d, 7), partner(&d, 13));
    int lo = -1000, hi = 100000, cnt = 0;
    while (size(&d) > 0) {
        int a = pop_min(&d);
        check(a >= lo, "min side ascending");
        lo = a;
        cnt++;
        if (size(&d) == 0)
            break;
        int b = pop_max(&d);
        check(b <= hi && b >= a, "max side descending");
        hi = b;
        cnt++;
    }
    printf("drained=%d meet=%d/%d\n", cnt, lo, hi);
    return 0;
}
