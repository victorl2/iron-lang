/*
 * title: Pure vs slotted ALOHA throughput
 * topic: networking
 * covers: ALOHA vulnerable period, G*exp(-2G) and G*exp(-G) curves, Monte Carlo with seeded PRNG, backlogged finite population, retransmission probability
 * deps: libc, libm
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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

enum { L = 10, M = 100, MAXSTART = 200000 };
static long starts[MAXSTART];

/* Poisson-like source: each of M stations starts a frame in each tick with probability G/(L*M).
   Returns the number of successful frames; a frame succeeds iff no other start lies within L ticks of it. */
static long pure_aloha(long g_milli, long ticks) {
    unsigned p = (unsigned)((unsigned long)g_milli * 1048576ul / (1000ul * L * M));
    long n = 0;
    for (long t = 0; t < ticks; t++) {
        for (int i = 0; i < M; i++) {
            if ((rnd() & 0xfffffu) < p) {
                if (n >= MAXSTART) fail("too many starts");
                starts[n++] = t;
            }
        }
    }
    long ok = 0;
    for (long i = 0; i < n; i++) {
        int left = i == 0 || starts[i] - starts[i - 1] >= L;
        int right = i == n - 1 || starts[i + 1] - starts[i] >= L;
        if (left && right) ok++;
    }
    return ok;
}
static long slotted_aloha(long g_milli, long slots, long *collisions, long *empty) {
    unsigned q = (unsigned)((unsigned long)g_milli * 1048576ul / (1000ul * M));
    long ok = 0;
    *collisions = 0; *empty = 0;
    for (long s = 0; s < slots; s++) {
        int k = 0;
        for (int i = 0; i < M; i++) if ((rnd() & 0xfffffu) < q) k++;
        if (k == 1) ok++; else if (k == 0) (*empty)++; else (*collisions)++;
    }
    return ok;
}

/* finite population with backlog: idle stations get a frame with prob a per slot and send at once;
   backlogged stations retransmit with prob q per slot. */
static void backlogged(int n, int a_permille, int q_permille, long slots, long *thru, long *avg_backlog) {
    int backlog[64] = {0};
    long ok = 0, bsum = 0;
    for (long s = 0; s < slots; s++) {
        int k = 0, who = -1;
        for (int i = 0; i < n; i++) {
            unsigned r = rnd();
            int tx;
            if (backlog[i]) tx = (int)(r % 1000) < q_permille;
            else if ((int)(r % 1000) < a_permille) { tx = 1; backlog[i] = 1; }
            else tx = 0;
            if (tx) { k++; who = i; }
        }
        if (k == 1) { ok++; backlog[who] = 0; }
        for (int i = 0; i < n; i++) bsum += backlog[i];
    }
    *thru = ok;
    *avg_backlog = bsum * 100 / slots;
}

int main(void) {
    long gs[] = {100, 250, 500, 1000, 1500, 2000};
    printf("%6s | %10s %10s | %10s %10s\n", "G", "pure S", "theory", "slotted S", "theory");
    long best_pure = 0, best_slot = 0;
    for (int i = 0; i < 6; i++) {
        double g = (double)gs[i] / 1000.0;
        rng_state = 0x1234567u + (unsigned)i;
        long ticks = 30000;
        long ok = pure_aloha(gs[i], ticks);
        double s_pure = (double)ok * L / (double)ticks;
        long coll, empty;
        long ok2 = slotted_aloha(gs[i], 30000, &coll, &empty);
        double s_slot = (double)ok2 / 30000.0;
        double th_pure = g * exp(-2.0 * g), th_slot = g * exp(-g);
        printf("%6.2f | %10.3f %10.3f | %10.3f %10.3f\n", g, s_pure, th_pure, s_slot, th_slot);
        check(fabs(s_pure - th_pure) < 0.03, "pure ALOHA matches G e^{-2G}");
        check(fabs(s_slot - th_slot) < 0.03, "slotted ALOHA matches G e^{-G}");
        if (ok > best_pure) best_pure = ok;
        if (ok2 > best_slot) best_slot = ok2;
        check(coll + empty + ok2 == 30000, "slot accounting");
    }
    /* peak throughputs: 1/(2e) = 0.184 and 1/e = 0.368 */
    double bp = (double)best_pure * L / 30000.0, bs = (double)best_slot / 30000.0;
    printf("peak measured: pure=%.3f (1/2e=%.3f) slotted=%.3f (1/e=%.3f)\n", bp, 1.0 / (2.0 * exp(1.0)), bs, 1.0 / exp(1.0));
    check(bs > 1.6 * bp && bs < 2.4 * bp, "slotting roughly doubles the peak");

    /* stability of slotted ALOHA with a finite population and a fixed retransmission probability */
    printf("finite population N=20, arrival prob 15/1000 per idle station per slot:\n");
    int qs[] = {20, 100, 400};
    for (int i = 0; i < 3; i++) {
        long th, bl;
        rng_state = 0xfeedu;
        backlogged(20, 15, qs[i], 20000, &th, &bl);
        printf("  retransmit prob %3d/1000: throughput %.3f backlog %ld.%02ld stations\n", qs[i], (double)th / 20000.0, bl / 100, bl % 100);
    }
    long th1, bl1, th2, bl2;
    rng_state = 77u; backlogged(20, 15, 100, 20000, &th1, &bl1);
    rng_state = 77u; backlogged(20, 15, 600, 20000, &th2, &bl2);
    check(bl2 > bl1, "aggressive retransmission piles up a larger backlog");
    return 0;
}
