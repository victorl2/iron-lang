/*
 * title: Deficit round robin vs self-clocked fair queueing vs FIFO
 * topic: networking
 * covers: DRR quantum and deficit counters, SCFQ virtual finish tags, weighted bandwidth shares, interactive latency under load
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

enum { NF = 4, MAXQ = 2048, SCALE = 12, BASEQ = 1500 };
enum { K_FIFO, K_DRR, K_SCFQ };
static const char *kname[] = {"FIFO", "DRR", "SCFQ"};
static const int weight[NF] = {1, 2, 3, 4};

typedef struct { int flow, size; long arrive; long tag; long seq; } P;
typedef struct {
    P q[NF][MAXQ];
    int head[NF], tail[NF];
    long deficit[NF];
    int cur, turn_started;
    long vtime, last_tag[NF];
    long seq;
} Sched;
static Sched S;

static int qn(int f) { return S.tail[f] - S.head[f]; }
static P *front(int f) { return &S.q[f][S.head[f]]; }

static void enqueue(int kind, int flow, int size, long t) {
    if (S.tail[flow] >= MAXQ) fail("queue overflow");
    P *p = &S.q[flow][S.tail[flow]++];
    p->flow = flow; p->size = size; p->arrive = t; p->seq = S.seq++;
    if (kind == K_SCFQ) {
        long start = S.vtime > S.last_tag[flow] ? S.vtime : S.last_tag[flow];
        p->tag = start + (long)size * SCALE / weight[flow];
        S.last_tag[flow] = p->tag;
    }
}
static int pick(int kind) {
    int total = 0;
    for (int f = 0; f < NF; f++) total += qn(f);
    if (!total) return -1;
    if (kind == K_FIFO) {
        int best = -1;
        for (int f = 0; f < NF; f++)
            if (qn(f) && (best < 0 || front(f)->seq < front(best)->seq)) best = f;
        return best;
    }
    if (kind == K_SCFQ) {
        int best = -1;
        for (int f = 0; f < NF; f++)
            if (qn(f) && (best < 0 || front(f)->tag < front(best)->tag)) best = f;
        S.vtime = front(best)->tag;
        return best;
    }
    for (int guard = 0; guard < 100000; guard++) {
        int f = S.cur;
        if (!qn(f)) { S.deficit[f] = 0; S.turn_started = 0; S.cur = (f + 1) % NF; continue; }
        if (!S.turn_started) { S.deficit[f] += (long)weight[f] * BASEQ / 2; S.turn_started = 1; }
        if (front(f)->size <= S.deficit[f]) { S.deficit[f] -= front(f)->size; return f; }
        S.turn_started = 0;
        S.cur = (f + 1) % NF;
    }
    fail("DRR made no progress");
    return -1;
}

static int fsize(int f, unsigned r) {
    switch (f) {
    case 0: return 1500;
    case 1: return 500;
    case 2: return 64 + (int)(r % 1437);
    default: return 100;
    }
}

static void backlogged(int kind) {
    memset(&S, 0, sizeof S);
    rng_state = 4711u;
    /* arrivals interleave one packet per flow per round, so FIFO serves equal packet counts */
    for (int i = 0; i < 1400; i++)
        for (int f = 0; f < NF; f++) { unsigned r = rnd(); enqueue(kind, f, fsize(f, r), 0); }
    long served[NF] = {0}, total = 0;
    while (total < 250000) {
        int f = pick(kind);
        check(f >= 0, "backlog lasts");
        P *p = front(f);
        served[f] += p->size; total += p->size;
        S.head[f]++;
    }
    printf("%-4s shares of %ld bytes:", kname[kind], total);
    long wsum = 0;
    for (int f = 0; f < NF; f++) wsum += weight[f];
    long maxdev = 0;
    for (int f = 0; f < NF; f++) {
        long ideal = total * weight[f] / wsum;
        long dev = served[f] > ideal ? served[f] - ideal : ideal - served[f];
        if (dev > maxdev) maxdev = dev;
        printf(" f%d=%2ld%%", f, served[f] * 100 / total);
    }
    printf("  max deviation from weighted share=%ld B\n", maxdev);
    if (kind != K_FIFO) check(maxdev <= 4 * 1500, "weighted fairness within a few packets");
    else check(maxdev > 4 * 1500, "FIFO is not weighted-fair");
}

/* flow 0 sends a small packet every 1200 ticks while flows 1..3 keep the link saturated; link moves 1 byte per tick */
static void interactive(int kind, long *avg, long *worst) {
    memset(&S, 0, sizeof S);
    rng_state = 99u;
    long t = 0, next_i = 0;
    int sent_i = 0, nint = 0;
    long sumd = 0, maxd = 0;
    int nbig[NF] = {0};
    long next_arr = 0;
    while (nint < 40) {
        /* admit arrivals up to time t */
        while (next_arr <= t) {
            for (int f = 1; f < NF; f++) {
                if (qn(f) < 6) { unsigned r = rnd(); enqueue(kind, f, f == 1 ? 1500 : 500 + (int)(r % 1000), next_arr); nbig[f]++; }
            }
            next_arr += 300;
        }
        while (next_i <= t && sent_i < 40) { enqueue(kind, 0, 64, next_i); sent_i++; next_i += 1200; }
        int f = pick(kind);
        if (f < 0) { t++; continue; }
        P p = *front(f);
        S.head[f]++;
        long d = t - p.arrive;
        t += p.size;
        if (f == 0) { nint++; sumd += d; if (d > maxd) maxd = d; }
    }
    *avg = sumd / nint; *worst = maxd;
}

int main(void) {
    for (int k = 0; k < 3; k++) backlogged(k);
    long avg[3], worst[3];
    for (int k = 0; k < 3; k++) {
        interactive(k, &avg[k], &worst[k]);
        printf("%-4s interactive 64B flow queueing delay: mean=%ld ticks worst=%ld ticks\n", kname[k], avg[k], worst[k]);
    }
    check(avg[K_SCFQ] < avg[K_FIFO], "SCFQ isolates the small flow better than FIFO");
    check(avg[K_DRR] <= avg[K_FIFO] + 1500, "DRR does not starve the small flow");
    return 0;
}
