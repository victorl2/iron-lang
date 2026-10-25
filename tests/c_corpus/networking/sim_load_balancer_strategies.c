/*
 * title: Load balancing strategies on heterogeneous backends
 * topic: networking
 * covers: round robin, smooth weighted round robin, random, least connections, weighted least connections, power of two choices, latency percentiles
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 45000
static unsigned rng_state = 1u;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

typedef struct { long t, ord; int type, node, a, b, c, d; } Ev;
static Ev evq[EVCAP];
static int evn;
static long now, ordc;
static int ev_less(const Ev *x, const Ev *y) { return x->t != y->t ? x->t < y->t : x->ord < y->ord; }
static void ev_push(long t, int type, int node, int a, int b, int c, int d) {
    if (evn >= EVCAP) fail("event queue overflow");
    Ev e = {t, ordc++, type, node, a, b, c, d};
    int i = evn++;
    evq[i] = e;
    while (i > 0 && ev_less(&evq[i], &evq[(i - 1) / 2])) {
        Ev tmp = evq[i]; evq[i] = evq[(i - 1) / 2]; evq[(i - 1) / 2] = tmp;
        i = (i - 1) / 2;
    }
}
static Ev ev_pop(void) {
    Ev top = evq[0];
    evq[0] = evq[--evn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < evn && ev_less(&evq[l], &evq[m])) m = l;
        if (r < evn && ev_less(&evq[r], &evq[m])) m = r;
        if (m == i) break;
        Ev tmp = evq[i]; evq[i] = evq[m]; evq[m] = tmp;
        i = m;
    }
    now = top.t;
    return top;
}

enum { NB = 4, NREQ = 20000 };
enum { S_RR, S_WRR, S_RANDOM, S_LC, S_WLC, S_P2C, S_P2CW, NSTRAT };
static const char *sname[NSTRAT] = {"round-robin", "smooth-wrr", "random", "least-conn", "weighted-lc", "p2c", "p2c-weighted"};
static const int speed[NB] = {1, 1, 2, 4};
enum { EV_ARRIVE, EV_DONE };

static int outstanding[NB];
static long free_at[NB];
static int rr_next;
static int wrr_cur[NB];
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

static int pick_smooth_wrr(void) {
    int total = 0, best = 0;
    for (int i = 0; i < NB; i++) { wrr_cur[i] += speed[i]; total += speed[i]; }
    for (int i = 1; i < NB; i++) if (wrr_cur[i] > wrr_cur[best]) best = i;
    wrr_cur[best] -= total;
    return best;
}
static int load_less(int a, int b, int weighted) { /* is backend a strictly less loaded than b? */
    long la = weighted ? (long)outstanding[a] * 1000 / speed[a] : outstanding[a];
    long lb = weighted ? (long)outstanding[b] * 1000 / speed[b] : outstanding[b];
    return la < lb || (la == lb && a < b);
}
static int pick(int strat) {
    switch (strat) {
    case S_RR: { int b = rr_next; rr_next = (rr_next + 1) % NB; return b; }
    case S_WRR: return pick_smooth_wrr();
    case S_RANDOM: { unsigned r = rnd(); return (int)(r % NB); }
    case S_LC:
    case S_WLC: {
        int best = 0;
        for (int i = 1; i < NB; i++) if (load_less(i, best, strat == S_WLC)) best = i;
        return best;
    }
    default: {
        unsigned r1 = rnd(), r2 = rnd();
        int a = (int)(r1 % NB), b = (int)(r2 % (NB - 1));
        if (b >= a) b++;
        return load_less(a, b, strat == S_P2CW) ? a : b;
    }
    }
}

typedef struct { long mean; int p50, p99, pmax; int share[NB]; } Result;

static Result run(int strat) {
    static int lat[NREQ];
    static long arrive[NREQ];
    Result r;
    memset(&r, 0, sizeof r);
    memset(outstanding, 0, sizeof outstanding);
    memset(free_at, 0, sizeof free_at);
    memset(wrr_cur, 0, sizeof wrr_cur);
    rr_next = 0; evn = 0; now = 0;
    rng_state = 0x10adba1u;
    long t = 0;
    for (int i = 0; i < NREQ; i++) {
        unsigned a = rnd();
        t += 4 + (long)(a % 9);
        arrive[i] = t;
        ev_push(t, EV_ARRIVE, 0, i, 0, 0, 0);
    }
    long sum = 0;
    while (evn > 0) {
        Ev e = ev_pop();
        if (e.type == EV_DONE) { outstanding[e.node]--; continue; }
        unsigned k = rnd();
        unsigned z = rnd();
        int size = (k % 10 == 0) ? 200 + (int)(z % 200) : 10 + (int)(z % 21); /* 10% elephants */
        int b = pick(strat);
        int svc = (size + speed[b] - 1) / speed[b];
        long start = now > free_at[b] ? now : free_at[b];
        long fin = start + svc;
        free_at[b] = fin;
        outstanding[b]++;
        ev_push(fin, EV_DONE, b, e.a, 0, 0, 0);
        lat[e.a] = (int)(fin - arrive[e.a]);
        sum += lat[e.a];
        r.share[b]++;
    }
    r.mean = sum / NREQ;
    qsort(lat, NREQ, sizeof(int), cmp_int);
    r.p50 = lat[NREQ / 2]; r.p99 = lat[NREQ * 99 / 100]; r.pmax = lat[NREQ - 1];
    return r;
}

int main(void) {
    /* smooth weighted round robin: exact proportions over each cycle of sum(weights) picks, and no long bursts */
    memset(wrr_cur, 0, sizeof wrr_cur);
    int cnt[NB] = {0}, run_len = 0, max_run = 0, last = -1;
    for (int i = 0; i < 8 * 5; i++) {
        int b = pick_smooth_wrr();
        cnt[b]++;
        run_len = b == last ? run_len + 1 : 1;
        if (run_len > max_run) max_run = run_len;
        last = b;
        if ((i + 1) % 8 == 0)
            for (int j = 0; j < NB; j++) check(cnt[j] == speed[j] * ((i + 1) / 8), "smooth WRR is exact per cycle");
    }
    printf("smooth WRR over 40 picks: counts %d %d %d %d, longest run %d\n", cnt[0], cnt[1], cnt[2], cnt[3], max_run);
    check(max_run <= 2, "smooth WRR interleaves");
    Result res[NSTRAT];
    printf("%-13s %6s %5s %5s %6s | requests per backend (speeds 1 1 2 4)\n", "strategy", "mean", "p50", "p99", "max");
    for (int s = 0; s < NSTRAT; s++) {
        res[s] = run(s);
        int tot = 0;
        for (int b = 0; b < NB; b++) tot += res[s].share[b];
        check(tot == NREQ, "every request dispatched");
        printf("%-13s %6ld %5d %5d %6d | %5d %5d %5d %5d\n", sname[s], res[s].mean, res[s].p50, res[s].p99, res[s].pmax, res[s].share[0],
               res[s].share[1], res[s].share[2], res[s].share[3]);
    }
    check(res[S_WLC].p99 < res[S_RR].p99, "load-aware balancing beats blind round robin on the tail");
    check(res[S_P2CW].mean < res[S_RANDOM].mean, "two choices beat one");
    check(res[S_WRR].mean < res[S_RR].mean, "weights help when backends differ");
    return 0;
}
