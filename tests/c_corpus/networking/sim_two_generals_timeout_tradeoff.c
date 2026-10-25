/*
 * title: Two generals and the timeout trade-off
 * topic: networking
 * covers: unreliable channel agreement, retransmission budget, disagreement probability, closed-form check, timeout false positives vs detection latency
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

enum { TRIALS = 60000 };

/* A sends up to K copies of "attack at dawn"; B acks every copy it receives. Loss is p_num/100 per message.
   B attacks iff it received a copy. A attacks iff it received an ack. Disagreement = exactly one attacks. */
typedef struct { long both, none, only_b, only_a; } Outcome;

static Outcome play(int k, int p_pct) {
    Outcome o = {0, 0, 0, 0};
    for (int t = 0; t < TRIALS; t++) {
        int b_got = 0, a_got = 0;
        for (int i = 0; i < k; i++) {
            unsigned r1 = rnd();
            unsigned r2 = rnd();
            int delivered = (int)(r1 % 100) >= p_pct;
            if (!delivered) continue;
            b_got = 1;
            if ((int)(r2 % 100) >= p_pct) a_got = 1;
        }
        if (a_got && b_got) o.both++;
        else if (!a_got && !b_got) o.none++;
        else if (b_got) o.only_b++;
        else o.only_a++;
    }
    return o;
}

static double disagree_theory(int k, double p) {
    /* P(B received) - P(A got an ack) = (1 - p^k) - (1 - q^k) with q = 1 - (1-p)^2 the chance a round trip fails */
    double q = 1.0 - (1.0 - p) * (1.0 - p);
    return pow(q, k) - pow(p, k);
}

int main(void) {
    int p_pct = 30;
    printf("channel loss %d%%, %d trials per row\n", p_pct, TRIALS);
    printf("%2s %8s %8s %8s %8s | %9s %9s\n", "K", "both", "none", "only_B", "only_A", "measured", "theory");
    for (int k = 1; k <= 8; k++) {
        rng_state = 0x2a2a0000u + (unsigned)k;
        Outcome o = play(k, p_pct);
        check(o.only_a == 0, "A can only attack after B received a copy");
        check(o.both + o.none + o.only_b == TRIALS, "outcomes partition the trials");
        double meas = (double)o.only_b / TRIALS;
        double th = disagree_theory(k, p_pct / 100.0);
        printf("%2d %8ld %8ld %8ld %8ld | %9.4f %9.4f\n", k, o.both, o.none, o.only_b, o.only_a, meas, th);
        check(fabs(meas - th) < 0.012, "simulation matches the closed form");
        if (k > 1) check(th > 0.0, "disagreement never reaches zero");
    }
    /* more retransmissions only push the disagreement down; it is a probability, never a guarantee */
    double t20 = disagree_theory(20, 0.30);
    printf("theory K=20: %.6f, K=50: %.9f (still > 0)\n", t20, disagree_theory(50, 0.30));
    check(disagree_theory(50, 0.30) > 0.0, "still nonzero at K=50");

    /* Timeout trade-off: heartbeat delay = 10 + 5 * (number of consecutive coin flips with 40% continue). */
    printf("heartbeat delay timeouts (delay = 10 + 5*Geom(0.4)), 200000 heartbeats\n");
    printf("%8s %14s %12s\n", "timeout", "false suspects", "detect after crash");
    long prev_fp = 1 << 30;
    for (int to = 10; to <= 60; to += 10) {
        rng_state = 0x7777u;
        long fp = 0;
        for (int i = 0; i < 200000; i++) {
            long d = 10;
            for (;;) {
                unsigned r = rnd();
                if ((int)(r % 100) < 40) d += 5; else break;
            }
            if (d > to) fp++;
        }
        /* a crashed peer is noticed one timeout after the last heartbeat it managed to send (interval 10) */
        printf("%8d %7ld (%4ld.%02ld%%) %12d\n", to, fp, fp / 2000, (fp / 20) % 100, to + 10);
        check(fp <= prev_fp, "longer timeouts suspect less often");
        prev_fp = fp;
    }
    check(prev_fp < 200, "a generous timeout almost never gives a false alarm");
    return 0;
}
