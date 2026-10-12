/*
 * title: CUBIC window growth in fixed point
 * topic: networking
 * covers: CUBIC W(t), integer cube root, TCP-friendly region, fast convergence, comparison with Reno, 64-bit fixed point
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

/* Units: window in milli-segments, time in milliseconds. C = 0.4, beta = 0.7. */
enum { BETA_M = 700, C_M = 400, RTT_MS = 400 };

static int64_t icbrt(int64_t v) {
    int64_t lo = 0, hi = 2000000;
    while (lo < hi) {
        int64_t mid = (lo + hi + 1) / 2;
        if (mid * mid * mid <= v) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* K in ms: cbrt(Wmax*(1-beta)/C) seconds -> ms^3 = Wmax_segments * (1000-BETA_M)/C_M * 1e9 */
static int64_t cubic_k(int64_t wmax_milli) {
    int64_t cube = wmax_milli / 1000 * (1000 - BETA_M) * 1000000000LL / C_M;
    /* wmax_milli/1000 truncates fractional segments, so add them back proportionally */
    cube += (wmax_milli % 1000) * (1000 - BETA_M) * 1000000LL / C_M;
    return icbrt(cube);
}
static int64_t cubic_w(int64_t wmax_milli, int64_t k_ms, int64_t t_ms) {
    int64_t d = t_ms - k_ms;
    int64_t d3 = d * d * d;
    return wmax_milli + (int64_t)C_M * d3 / 1000000000LL;
}
static int64_t friendly_w(int64_t wmax_milli, int64_t t_ms) {
    /* Reno-equivalent: beta*Wmax + 3(1-beta)/(1+beta) per RTT */
    int64_t per_rtt_milli = 3 * (1000 - BETA_M) * 1000 / (1000 + BETA_M);
    return wmax_milli * BETA_M / 1000 + per_rtt_milli * t_ms / RTT_MS;
}

typedef struct { int64_t wmax, w_last_max; int64_t k, origin; } Epoch;

static void loss(Epoch *e, int64_t cwnd_milli) {
    /* fast convergence: release bandwidth when the new maximum is below the last one */
    if (cwnd_milli < e->w_last_max) e->wmax = cwnd_milli * (1000 + BETA_M) / 2000;
    else e->wmax = cwnd_milli;
    e->w_last_max = cwnd_milli;
    e->k = cubic_k(e->wmax);
}
static int64_t after_loss_cwnd(const Epoch *e) { return e->wmax * BETA_M / 1000; }
static int64_t window(const Epoch *e, int64_t t) {
    int64_t c = cubic_w(e->wmax, e->k, t), f = friendly_w(e->wmax, t);
    return c > f ? c : f;
}

int main(void) {
    /* integer cube root exactness */
    for (int64_t v = 0; v < 20000; v += 7) {
        int64_t r = icbrt(v);
        check(r * r * r <= v && (r + 1) * (r + 1) * (r + 1) > v, "icbrt floor");
    }
    check(icbrt(1000000000000LL) == 10000, "icbrt(1e12)");

    Epoch e = {0, 0, 0, 0};
    loss(&e, 100000);
    printf("loss at cwnd=100.000: wmax=%lld k=%lldms start=%lld\n", (long long)e.wmax, (long long)e.k,
           (long long)after_loss_cwnd(&e));
    check(e.k >= 4216 && e.k <= 4218, "K near 4.217 s");
    int64_t at_k = cubic_w(e.wmax, e.k, e.k);
    check(at_k == e.wmax, "W(K) = Wmax");

    printf("%6s %10s %10s %10s\n", "t_ms", "cubic", "friendly", "reno");
    int64_t prev = 0;
    int64_t reno_hit = -1, cubic_hit = -1;
    for (int64_t t = 0; t <= 16000; t += 100) {
        int64_t w = window(&e, t);
        int64_t reno = after_loss_cwnd(&e) + 1000 * (t / RTT_MS);
        check(w >= prev, "window is non-decreasing");
        prev = w;
        if (reno_hit < 0 && reno >= e.wmax) reno_hit = t;
        if (cubic_hit < 0 && w >= e.wmax) cubic_hit = t;
        if (t % 1000 == 0)
            printf("%6lld %10lld %10lld %10lld\n", (long long)t, (long long)w, (long long)friendly_w(e.wmax, t),
                   (long long)reno);
    }
    printf("regains Wmax: cubic at %lldms, reno at %lldms\n", (long long)cubic_hit, (long long)reno_hit);
    check(cubic_hit > 0 && reno_hit > 0 && cubic_hit < reno_hit, "CUBIC recovers before Reno on a long-RTT path");

    /* plateau: growth per 100 ms near K is far smaller than at the ends of the epoch */
    int64_t g_start = window(&e, 200) - window(&e, 100);
    int64_t g_mid = window(&e, e.k + 50) - window(&e, e.k - 50);
    int64_t g_end = window(&e, 16000) - window(&e, 15900);
    printf("growth/100ms: start=%lld plateau=%lld end=%lld\n", (long long)g_start, (long long)g_mid, (long long)g_end);
    check(g_mid < g_start && g_mid < g_end, "concave then convex");

    /* fast convergence: second loss at a lower window shrinks Wmax further */
    int64_t w2 = window(&e, 2000);
    loss(&e, w2);
    printf("second loss at cwnd=%lld: wmax=%lld (fast convergence) k=%lldms\n", (long long)w2, (long long)e.wmax, (long long)e.k);
    check(e.wmax < w2, "fast convergence lowers Wmax");
    /* growth from 30 different maxima: K scales with cbrt(Wmax) */
    printf("K by Wmax:");
    int64_t last = 0;
    for (int64_t w = 10; w <= 640; w *= 2) {
        int64_t k = cubic_k(w * 1000);
        printf(" %lld->%lld", (long long)w, (long long)k);
        check(k > last, "K increases with Wmax");
        last = k;
    }
    printf("\n");
    /* K ratio for 8x window is 2x (cube root) */
    int64_t k1 = cubic_k(10000), k8 = cubic_k(80000);
    check(k8 >= 2 * k1 - 2 && k8 <= 2 * k1 + 2, "cube-root scaling");
    return 0;
}
