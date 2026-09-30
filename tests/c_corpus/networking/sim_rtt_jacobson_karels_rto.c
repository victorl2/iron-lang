/*
 * title: Jacobson/Karels RTT estimator with Karn backoff
 * topic: networking
 * covers: SRTT, RTTVAR, RTO clamp, Karn's algorithm, exponential RTO backoff, spurious timeout counting, integer arithmetic
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

enum { MIN_RTO = 200000, MAX_RTO = 60000000 }; /* microseconds */

typedef struct {
    long srtt, rttvar, rto;
    int have;
    int backoff;
} Est;

static long clampl(long v, long lo, long hi) { return v < lo ? lo : v > hi ? hi : v; }

static void jk_sample(Est *e, long r) {
    if (!e->have) {
        e->srtt = r;
        e->rttvar = r / 2;
        e->have = 1;
    } else {
        long err = e->srtt > r ? e->srtt - r : r - e->srtt;
        e->rttvar = (3 * e->rttvar + err) / 4;
        e->srtt = (7 * e->srtt + r) / 8;
    }
    long k = 4 * e->rttvar;
    e->rto = clampl(e->srtt + (k > 1000 ? k : 1000), MIN_RTO, MAX_RTO);
    e->backoff = 0;
}
static void jk_timeout(Est *e) {
    e->rto = clampl(e->rto * 2, MIN_RTO, MAX_RTO);
    e->backoff++;
}

static void karn_ack(Est *e, long r, int was_retransmitted) {
    if (!was_retransmitted) jk_sample(e, r);
}

/* RFC 793 style estimator: SRTT = 0.8 SRTT + 0.2 R, RTO = clamp(2 SRTT) */
typedef struct { long srtt; int have; long rto; } Old;
static void old_sample(Old *o, long r) {
    if (!o->have) { o->srtt = r; o->have = 1; }
    else o->srtt = (8 * o->srtt + 2 * r) / 10;
    o->rto = clampl(2 * o->srtt, MIN_RTO, MAX_RTO);
}

int main(void) {
    rng_state = 20240607u;
    Est e = {0, 0, 1000000, 0, 0};
    Old o = {0, 0, 1000000};
    int spurious_jk = 0, spurious_old = 0, samples = 0;
    long max_rto = 0;
    printf("%3s %8s | %8s %8s %8s | %8s\n", "n", "rtt_us", "srtt", "rttvar", "rto", "old_rto");
    for (int i = 0; i < 48; i++) {
        long base = i < 16 ? 100000 : i < 26 ? 320000 : 110000;
        unsigned jr = rnd();
        long jitter = (long)(jr % 30001) - 15000;
        long r = base + jitter;
        if (i == 33) r = 900000; /* one delay spike */
        long prev_jk = e.rto, prev_old = o.rto;
        if (i > 0) {
            if (r > prev_jk) spurious_jk++;
            if (r > prev_old) spurious_old++;
        }
        jk_sample(&e, r);
        old_sample(&o, r);
        samples++;
        if (e.rto > max_rto) max_rto = e.rto;
        check(e.rto >= e.srtt, "RTO covers SRTT");
        check(e.rto >= MIN_RTO && e.rto <= MAX_RTO, "RTO clamped");
        if (i < 6 || (i >= 14 && i < 20) || (i >= 31 && i < 38) || i == 47)
            printf("%3d %8ld | %8ld %8ld %8ld | %8ld\n", i, r, e.srtt, e.rttvar, e.rto, o.rto);
    }
    printf("samples=%d spurious timeouts: jacobson-karels=%d rfc793=%d\n", samples, spurious_jk, spurious_old);
    check(spurious_jk <= spurious_old, "variance-aware RTO is no worse than 2*SRTT");

    /* Karn: retransmitted segments give ambiguous samples, so keep the backed-off RTO until a fresh sample */
    printf("loss episode\n");
    long before = e.rto;
    long hist[8];
    int n = 0;
    for (int k = 0; k < 6; k++) {
        jk_timeout(&e);
        hist[n++] = e.rto;
        printf("  timeout %d -> rto=%ld backoff=%d\n", k + 1, e.rto, e.backoff);
    }
    check(hist[0] == 2 * before, "first backoff doubles");
    check(hist[1] == 2 * hist[0] || hist[1] == MAX_RTO, "backoff keeps doubling");
    long stuck = e.rto;
    karn_ack(&e, 777000, 1); /* ACK of a retransmission: must not be sampled */
    check(e.rto == stuck && e.backoff == 6, "ambiguous ACK leaves RTO untouched");
    long r = 120000;
    jk_sample(&e, r);
    printf("  fresh sample %ld -> srtt=%ld rttvar=%ld rto=%ld backoff=%d\n", r, e.srtt, e.rttvar, e.rto, e.backoff);
    check(e.rto < stuck && e.backoff == 0, "fresh sample resets backoff");
    for (int k = 0; k < 8; k++) jk_timeout(&e);
    check(e.rto <= MAX_RTO, "cap");
    printf("after 8 more timeouts rto=%ld (cap %d)\n", e.rto, MAX_RTO);
    printf("max rto during trace: %ld\n", max_rto);
    return 0;
}
