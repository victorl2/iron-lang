/*
 * title: Calendar booking structures: conflict-free sorted intervals and overlap-depth event map
 * topic: data_structures
 * covers: half-open intervals, sorted interval array, lower-bound conflict test, boundary delta map, max concurrent bookings, first-fit free slot
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 112358u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define HORIZON 1000
#define MAXB 600

/* structure 1: non-overlapping bookings, sorted by start */
typedef struct { int s, e; } Iv;
typedef struct { Iv v[MAXB]; int n; } Cal;

static int cal_lower(const Cal *c, int start) { /* first booking with s >= start */
    int lo = 0, hi = c->n;
    while (lo < hi) { int m = (lo + hi) / 2; if (c->v[m].s < start) lo = m + 1; else hi = m; }
    return lo;
}
static int cal_conflicts(const Cal *c, int s, int e) {
    int i = cal_lower(c, s);
    if (i < c->n && c->v[i].s < e) return 1;
    if (i > 0 && c->v[i - 1].e > s) return 1;
    return 0;
}
static int cal_book(Cal *c, int s, int e) {
    if (c->n >= MAXB || cal_conflicts(c, s, e)) return 0;
    int i = cal_lower(c, s);
    memmove(&c->v[i + 1], &c->v[i], (size_t)(c->n - i) * sizeof(Iv));
    c->v[i].s = s; c->v[i].e = e;
    c->n++;
    return 1;
}
static int cal_cancel(Cal *c, int s) {
    int i = cal_lower(c, s);
    if (i >= c->n || c->v[i].s != s) return 0;
    memmove(&c->v[i], &c->v[i + 1], (size_t)(c->n - i - 1) * sizeof(Iv));
    c->n--;
    return 1;
}
/* earliest start >= from where a gap of `len` fits inside [0, HORIZON) */
static int cal_first_fit(const Cal *c, int from, int len) {
    int t = from;
    int i = cal_lower(c, 0);
    for (; i < c->n; i++) {
        if (c->v[i].e <= t) continue;
        if (c->v[i].s >= t + len) break;
        t = c->v[i].e;
    }
    return t + len <= HORIZON ? t : -1;
}

/* structure 2: overlap depth via a sorted map of boundary deltas */
typedef struct { int t, d; } Ev;
typedef struct { Ev v[2 * MAXB]; int n; } Depth;
static int dep_lower(const Depth *d, int t) {
    int lo = 0, hi = d->n;
    while (lo < hi) { int m = (lo + hi) / 2; if (d->v[m].t < t) lo = m + 1; else hi = m; }
    return lo;
}
static void dep_add(Depth *d, int t, int delta) {
    int i = dep_lower(d, t);
    if (i < d->n && d->v[i].t == t) {
        d->v[i].d += delta;
        if (d->v[i].d == 0) { memmove(&d->v[i], &d->v[i + 1], (size_t)(d->n - i - 1) * sizeof(Ev)); d->n--; }
        return;
    }
    memmove(&d->v[i + 1], &d->v[i], (size_t)(d->n - i) * sizeof(Ev));
    d->v[i].t = t; d->v[i].d = delta;
    d->n++;
}
static void dep_book(Depth *d, int s, int e) { dep_add(d, s, 1); dep_add(d, e, -1); }
static void dep_cancel(Depth *d, int s, int e) { dep_add(d, s, -1); dep_add(d, e, 1); }
static int dep_max(const Depth *d) {
    int cur = 0, best = 0;
    for (int i = 0; i < d->n; i++) { cur += d->v[i].d; if (cur > best) best = cur; }
    return best;
}

int main(void) {
    /* part 1: conflict-free calendar against a per-minute occupancy array */
    static Cal cal;
    static unsigned char occ[HORIZON];
    long tries = 0, booked = 0, rejected = 0, cancelled = 0, fits_checked = 0;
    for (int step = 0; step < 5000; step++) {
        unsigned op = rnd() % 10;
        if (op < 7) {
            int s = (int)(rnd() % (HORIZON - 1)), len = 1 + (int)(rnd() % 30);
            int e = s + len > HORIZON ? HORIZON : s + len;
            int clash = 0;
            for (int t = s; t < e; t++) if (occ[t]) clash = 1;
            CHECK(cal_conflicts(&cal, s, e) == clash);
            tries++;
            int ok = cal_book(&cal, s, e);
            CHECK(ok == !clash || cal.n >= MAXB);
            if (ok) { for (int t = s; t < e; t++) occ[t] = 1; booked++; } else rejected++;
        } else if (op < 9 && cal.n > 0) {
            Iv v = cal.v[rnd() % (unsigned)cal.n];
            CHECK(cal_cancel(&cal, v.s));
            for (int t = v.s; t < v.e; t++) occ[t] = 0;
            cancelled++;
        } else {
            int from = (int)(rnd() % HORIZON), len = 1 + (int)(rnd() % 40);
            int expect = -1;
            for (int t = from; t + len <= HORIZON && expect < 0; t++) {
                int free_ = 1;
                for (int k = t; k < t + len; k++) if (occ[k]) { free_ = 0; break; }
                if (free_) expect = t;
            }
            CHECK(cal_first_fit(&cal, from, len) == expect);
            fits_checked++;
        }
        for (int i = 1; i < cal.n; i++) CHECK(cal.v[i - 1].e <= cal.v[i].s);
    }
    int busy = 0;
    for (int t = 0; t < HORIZON; t++) busy += occ[t];
    printf("calendar: tries=%ld booked=%ld rejected=%ld cancelled=%ld fit queries=%ld\n", tries, booked, rejected, cancelled, fits_checked);
    printf("calendar: %d bookings live, %d of %d minutes busy, first slot for 25 free min from 0 at %d\n", cal.n, busy, HORIZON, cal_first_fit(&cal, 0, 25));

    /* part 2: overlapping bookings with running maximum depth */
    static Depth dp;
    static int depth[HORIZON];
    static Iv live[MAXB];
    int nlive = 0;
    long peak_changes = 0;
    int last_max = 0;
    for (int step = 0; step < 3000; step++) {
        if ((rnd() % 4) != 0 && nlive < MAXB) {
            int s = (int)(rnd() % (HORIZON - 1)), len = 1 + (int)(rnd() % 120);
            int e = s + len > HORIZON ? HORIZON : s + len;
            dep_book(&dp, s, e);
            for (int t = s; t < e; t++) depth[t]++;
            live[nlive].s = s; live[nlive].e = e; nlive++;
        } else if (nlive > 0) {
            int k = (int)(rnd() % (unsigned)nlive);
            dep_cancel(&dp, live[k].s, live[k].e);
            for (int t = live[k].s; t < live[k].e; t++) depth[t]--;
            live[k] = live[--nlive];
        }
        int mx = 0;
        for (int t = 0; t < HORIZON; t++) if (depth[t] > mx) mx = depth[t];
        CHECK(dep_max(&dp) == mx);
        if (mx != last_max) { peak_changes++; last_max = mx; }
    }
    /* boundary map has no zero entries and stays sorted */
    for (int i = 0; i < dp.n; i++) { CHECK(dp.v[i].d != 0); if (i) CHECK(dp.v[i - 1].t < dp.v[i].t); }
    printf("depth map: %d live bookings, %d boundaries, max overlap %d, peak changed %ld times\n", nlive, dp.n, dep_max(&dp), peak_changes);
    return 0;
}
