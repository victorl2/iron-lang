/*
 * title: NTP-style selection, clock filter and PLL over simulated paths
 * topic: networking
 * covers: four-timestamp offset/delay, 8-sample minimum-delay clock filter, Marzullo intersection with a falseticker, weighted combine, phase/frequency discipline, integer microseconds
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

enum { NS = 4, FILT = 8, POLL_US = 16000000 };

typedef struct { long theta, delta; } Sample;
typedef struct {
    long off;                 /* true offset of the server clock, microseconds */
    long out_base, in_base;   /* one-way path delays */
    Sample win[FILT];
    int n;
} Server;
static Server srv[NS];

static long labs_(long v) { return v < 0 ? -v : v; }
static long isqrt(long v) {
    long lo = 0, hi = 1L << 31;
    while (lo < hi) { long m = (lo + hi + 1) / 2; if (m * m <= v) lo = m; else hi = m - 1; }
    return lo;
}

/* Marzullo: largest set of mutually overlapping intervals. Returns the max overlap count and the intersection bounds. */
static int marzullo(const long *lo, const long *hi, int n, long *best_lo, long *best_hi) {
    long pts[64];
    int typ[64], m = 0;
    for (int i = 0; i < n; i++) { pts[m] = lo[i]; typ[m++] = 0; pts[m] = hi[i]; typ[m++] = 1; }
    for (int i = 1; i < m; i++) { /* sort by point, starts before ends at the same point */
        long p = pts[i]; int t = typ[i], j = i - 1;
        while (j >= 0 && (pts[j] > p || (pts[j] == p && typ[j] > t))) { pts[j + 1] = pts[j]; typ[j + 1] = typ[j]; j--; }
        pts[j + 1] = p; typ[j + 1] = t;
    }
    int cur = 0, best = 0, armed = 0;
    for (int i = 0; i < m; i++) {
        if (typ[i] == 0) {
            cur++;
            if (cur > best) { best = cur; *best_lo = pts[i]; armed = 1; }
        } else {
            if (armed && cur == best) { *best_hi = pts[i]; armed = 0; }
            cur--;
        }
    }
    return best;
}
static int brute_overlap(const long *lo, const long *hi, int n) {
    int best = 0;
    for (int i = 0; i < n; i++) {
        long x = lo[i];
        int c = 0;
        for (int j = 0; j < n; j++) if (lo[j] <= x && x <= hi[j]) c++;
        if (c > best) best = c;
    }
    return best;
}

int main(void) {
    rng_state = 0x123456u;
    /* Marzullo against brute force on random interval sets */
    for (int trial = 0; trial < 500; trial++) {
        long lo[9], hi[9];
        int n = 3 + (int)(rnd() % 7);
        for (int i = 0; i < n; i++) {
            long c = (long)(rnd() % 1000);
            long w = (long)(rnd() % 200);
            lo[i] = c - w; hi[i] = c + w;
        }
        long bl = 0, bh = 0;
        int got = marzullo(lo, hi, n, &bl, &bh);
        check(got == brute_overlap(lo, hi, n), "Marzullo max overlap equals brute force");
        check(bl <= bh, "intersection is non-empty");
    }

    long true_off[NS] = {0, 900, -400, 180000};       /* server 3 is a falseticker */
    long out_base[NS] = {20000, 25000, 15000, 30000};
    long in_base[NS] = {20000, 24000, 15000, 30000};   /* server 1 is slightly asymmetric */
    for (int i = 0; i < NS; i++) { srv[i].off = true_off[i]; srv[i].out_base = out_base[i]; srv[i].in_base = in_base[i]; }

    long T = 0;                    /* true time */
    long L = 30000;                /* local clock starts 30 ms fast */
    long drift_ppb = 50000;        /* 50 ppm fast */
    printf("%4s %10s %10s %8s %s\n", "poll", "error_us", "drift_ppb", "survivors", "falsetickers");
    long err_final = 0;
    int false_caught = 0, polls_after_warmup = 0;
    for (int poll = 0; poll < 96; poll++) {
        /* advance time and the local clock */
        T += POLL_US;
        L += POLL_US + POLL_US / 1000 * drift_ppb / 1000000;
        long lo[NS], hi[NS], theta_c[NS], lam[NS];
        for (int s = 0; s < NS; s++) {
            unsigned r1 = rnd();
            unsigned r2 = rnd();
            long out = srv[s].out_base + (long)(r1 % 2000);
            long in = srv[s].in_base + (long)(r2 % 2000);
            long t1 = L;
            long t2 = (T + out) + srv[s].off;
            long t3 = t2 + 50;
            long t4 = L + out + 50 + in;
            Sample sm;
            sm.theta = ((t2 - t1) + (t3 - t4)) / 2;
            sm.delta = (t4 - t1) - (t3 - t2);
            /* clock filter: shift window, choose minimum delay */
            Server *x = &srv[s];
            if (x->n < FILT) x->win[x->n++] = sm;
            else { memmove(&x->win[0], &x->win[1], sizeof(Sample) * (FILT - 1)); x->win[FILT - 1] = sm; }
            int best = 0;
            for (int i = 1; i < x->n; i++) if (x->win[i].delta < x->win[best].delta) best = i;
            long jit2 = 0;
            for (int i = 0; i < x->n; i++) { long d = x->win[i].theta - x->win[best].theta; jit2 += d * d; }
            long jitter = isqrt(jit2 / x->n);
            theta_c[s] = x->win[best].theta;
            lam[s] = x->win[best].delta / 2 + jitter + 1000;
            lo[s] = theta_c[s] - lam[s]; hi[s] = theta_c[s] + lam[s];
        }
        long bl = 0, bh = 0;
        int cnt = marzullo(lo, hi, NS, &bl, &bh);
        check(cnt * 2 > NS, "a majority of servers agree");
        int surv[NS], ns = 0;
        char flmask[NS + 1];
        for (int s = 0; s < NS; s++) {
            if (hi[s] >= bl && lo[s] <= bh) { surv[ns++] = s; flmask[s] = '.'; }
            else flmask[s] = 'X';
        }
        flmask[NS] = 0;
        /* combine survivors weighted by 1/lambda */
        long wsum = 0, acc = 0;
        for (int i = 0; i < ns; i++) { long w = 1000000000L / lam[surv[i]]; wsum += w; acc += w * theta_c[surv[i]] / 1000; }
        long theta = acc * 1000 / wsum;
        /* PLL: slew half the phase error each poll and integrate a small fraction into the frequency estimate.
           theta microseconds over one 16 s poll interval is theta*62.5 ppb; use one thirty-second of it, clamped. */
        L += theta / 2;
        long dfreq = theta * 1000000 / (POLL_US / 1000) / 32;
        if (dfreq > 5000) dfreq = 5000;
        if (dfreq < -5000) dfreq = -5000;
        drift_ppb += dfreq;
        long err = L - T;
        if (poll >= 40) {
            polls_after_warmup++;
            if (flmask[3] == 'X' && flmask[0] == '.' && flmask[1] == '.' && flmask[2] == '.') false_caught++;
        }
        err_final = err;
        if (poll < 4 || poll % 12 == 11) printf("%4d %10ld %10ld %8d %s\n", poll, err, drift_ppb, ns, flmask);
    }
    printf("falseticker (server 3, +180 ms) rejected in %d of %d polls after warmup\n", false_caught, polls_after_warmup);
    printf("final clock error %ld us\n", err_final);
    check(false_caught == polls_after_warmup, "falseticker always rejected");
    check(labs_(err_final) < 3000, "clock disciplined to within 3 ms");
    check(labs_(drift_ppb) < 20000, "frequency error reduced from 50 ppm");
    return 0;
}
