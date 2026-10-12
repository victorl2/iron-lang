/*
 * title: RED active queue management vs drop-tail
 * topic: networking
 * covers: RED average queue EWMA, count-based drop probability, fixed-point arithmetic, global synchronization, AIMD sources
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

enum { NSRC = 8, DRAIN = 40, QCAP = 60, ROUNDS = 3000 };
static int minth = 12, maxth = 36, wdiv = 32;
enum { P_DROPTAIL, P_RED };
enum { ONE = 65536, MAXP = 6554 /* 0.1 */ };

typedef struct {
    int qlen;
    long avg_fp;      /* average queue, scaled by 65536 */
    int count;        /* packets since last drop while avg is between thresholds */
    long forced, early, tail;
} Q;

static int red_drop(Q *q) {
    q->avg_fp += ((long)q->qlen * ONE - q->avg_fp) / wdiv;
    long min_fp = (long)minth * ONE, max_fp = (long)maxth * ONE;
    if (q->avg_fp < min_fp) { q->count = -1; return 0; }
    if (q->avg_fp >= max_fp) { q->count = 0; q->forced++; return 1; }
    q->count++;
    long pb = (long)MAXP * (q->avg_fp - min_fp) / (max_fp - min_fp);
    long denom = ONE - (long)q->count * pb;
    long pa = denom <= 0 ? ONE : pb * ONE / denom;
    unsigned r = rnd() % ONE;
    if ((long)r < pa) { q->count = 0; q->early++; return 1; }
    return 0;
}

static void run(int policy, long *avgq_out, long *util_out) {
    Q q;
    memset(&q, 0, sizeof q);
    q.count = -1;
    int cwnd[NSRC];
    long delivered[NSRC] = {0};
    for (int i = 0; i < NSRC; i++) cwnd[i] = 1 + i % 4;
    rng_state = 0xABCDu;
    long qsum = 0, served = 0, drops = 0, sync_rounds = 0, loss_rounds = 0;
    int owner_q[QCAP];
    int head = 0, tail = 0;
    for (int r = 0; r < ROUNDS; r++) {
        int lost[NSRC] = {0};
        int sent[NSRC];
        memcpy(sent, cwnd, sizeof sent);
        int remaining = 1;
        /* packets interleave across sources one at a time */
        while (remaining) {
            remaining = 0;
            for (int s = 0; s < NSRC; s++) {
                if (sent[s] <= 0) continue;
                sent[s]--;
                if (sent[s] > 0) remaining = 1;
                int drop = 0;
                if (policy == P_RED && red_drop(&q)) drop = 1;
                if (!drop && q.qlen >= QCAP) { drop = 1; q.tail++; }
                if (drop) { lost[s] = 1; drops++; continue; }
                owner_q[tail % QCAP] = s; tail++; q.qlen++;
            }
        }
        for (int d = 0; d < DRAIN && q.qlen > 0; d++) {
            delivered[owner_q[head % QCAP]]++;
            head++; q.qlen--; served++;
        }
        if (q.qlen == 0 && policy == P_RED) q.avg_fp = q.avg_fp * 3 / 4; /* idle decay */
        qsum += q.qlen;
        int nl = 0;
        for (int s = 0; s < NSRC; s++) {
            if (lost[s]) { nl++; cwnd[s] = cwnd[s] / 2 > 1 ? cwnd[s] / 2 : 1; }
            else cwnd[s]++;
        }
        if (nl) loss_rounds++;
        if (nl >= NSRC * 3 / 4) sync_rounds++;
    }
    long sum = 0, sq = 0;
    for (int s = 0; s < NSRC; s++) { sum += delivered[s]; sq += delivered[s] * delivered[s]; }
    long jain = sum * sum * 1000 / ((long)NSRC * sq);
    printf("%-9s util=%3ld%% avg queue=%2ld.%02ld drops=%5ld (early %4ld forced %4ld tail %4ld) loss rounds=%4ld synchronized=%4ld fairness=%ld/1000\n",
           policy == P_RED ? (wdiv == 32 ? "RED fast" : "RED slow") : "droptail", served * 100 / ((long)DRAIN * ROUNDS), qsum / ROUNDS,
           (qsum * 100 / ROUNDS) % 100, drops, q.early, q.forced, q.tail, loss_rounds, sync_rounds, jain);
    *avgq_out = qsum * 100 / ROUNDS;
    *util_out = served * 100 / ((long)DRAIN * ROUNDS);
    check(policy == P_RED || q.early == 0, "droptail never drops early");
    check(policy == P_DROPTAIL || wdiv != 32 || q.early > 10 * (q.forced + q.tail), "RED drops early, before the buffer is full");
    check(served <= (long)DRAIN * ROUNDS, "cannot exceed link capacity");
}

int main(void) {
    long aq[3], ut[3];
    run(P_DROPTAIL, &aq[0], &ut[0]);
    run(P_RED, &aq[1], &ut[1]);
    minth = 20; maxth = 50; wdiv = 256;
    run(P_RED, &aq[2], &ut[2]);
    check(aq[1] < aq[0] && aq[2] < aq[0], "RED keeps the standing queue shorter than drop-tail");
    check(ut[0] >= ut[1], "drop-tail keeps the link at least as busy");
    return 0;
}
