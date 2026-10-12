/*
 * title: Token bucket, GCRA, policer vs shaper and trTCM
 * topic: networking
 * covers: token bucket, GCRA virtual scheduling equivalence, policing vs shaping, conformance brute-force check, two-rate three-color marker
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

/* time in microseconds; rate 125000 bytes/s = 8 us per byte */
enum { US_PER_BYTE = 8, NPKT = 300 };

typedef struct { long t; int size; } Pkt;

/* token bucket in whole bytes plus a microsecond remainder */
typedef struct { long burst, tokens, rem, last; } TB;
static void tb_init(TB *b, long burst) { b->burst = burst; b->tokens = burst; b->rem = 0; b->last = 0; }
static void tb_refill(TB *b, long t) {
    long dt = t - b->last + b->rem;
    b->last = t;
    b->tokens += dt / US_PER_BYTE;
    b->rem = dt % US_PER_BYTE;
    if (b->tokens >= b->burst) { b->tokens = b->burst; b->rem = 0; }
}
static int tb_conform(TB *b, long t, int size) {
    tb_refill(b, t);
    if (b->tokens >= size) { b->tokens -= size; return 1; }
    return 0;
}

/* GCRA as a leaky-bucket meter (virtual scheduling) */
typedef struct { long burst, tat; } Gcra;
static int gcra_conform(Gcra *g, long t, int size) {
    long tau = (g->burst - size) * US_PER_BYTE;
    if (g->tat - t > tau) return 0;
    if (g->tat < t) g->tat = t;
    g->tat += (long)size * US_PER_BYTE;
    return 1;
}

static void gen_traffic(Pkt *p, int n, unsigned seed) {
    rng_state = seed;
    long t = 0;
    for (int i = 0; i < n; i++) {
        unsigned a = rnd();
        unsigned b = rnd();
        /* on/off source: bursts of closely spaced packets, then gaps */
        t += (a % 7 == 0) ? 20000 + (long)(b % 30000) : 200 + (long)(b % 900);
        p[i].t = t;
        p[i].size = 64 + (int)((a >> 8) % 1200);
    }
}

/* shaper: delay a packet until the bucket holds enough tokens, FIFO, bounded queue in bytes */
static int shape(const Pkt *in, int n, long burst, long qlimit, Pkt *out, long *maxdelay, long *totdelay, int *dropped) {
    TB b;
    tb_init(&b, burst);
    long free_at = 0; /* time the previous packet left */
    int m = 0;
    *maxdelay = 0; *totdelay = 0; *dropped = 0;
    for (int i = 0; i < n; i++) {
        long t = in[i].t > free_at ? in[i].t : free_at;
        /* backlog: bytes still waiting at arrival = packets whose departure is after in[i].t */
        long backlog = 0;
        for (int j = 0; j < m; j++) if (out[j].t > in[i].t) backlog += out[j].size;
        if (backlog + in[i].size > qlimit) { (*dropped)++; continue; }
        tb_refill(&b, t);
        if (b.tokens < in[i].size) {
            long need = in[i].size - b.tokens;
            t += need * US_PER_BYTE - b.rem;
        }
        int ok = tb_conform(&b, t, in[i].size);
        check(ok, "shaper releases only conforming packets");
        out[m].t = t; out[m].size = in[i].size;
        long d = t - in[i].t;
        if (d > *maxdelay) *maxdelay = d;
        *totdelay += d;
        free_at = t;
        m++;
    }
    return m;
}
static int conforms_bruteforce(const Pkt *p, int n, long burst) {
    for (int i = 0; i < n; i++) {
        long bytes = 0;
        for (int j = i; j < n; j++) {
            bytes += p[j].size;
            if (bytes * US_PER_BYTE > burst * US_PER_BYTE + (p[j].t - p[i].t)) return 0;
        }
    }
    return 1;
}

/* trTCM (RFC 2698, color-blind): PIR bucket P and CIR bucket C */
typedef struct { TB c, p; } TrTcm;
static int trtcm(TrTcm *m, long t, int size) {
    tb_refill(&m->p, t * 2); /* the peak bucket refills twice as fast: PIR = 2 * CIR */
    tb_refill(&m->c, t);
    if (m->p.tokens < size) return 2; /* red */
    m->p.tokens -= size;
    if (m->c.tokens < size) return 1; /* yellow */
    m->c.tokens -= size;
    return 0; /* green */
}

int main(void) {
    static Pkt in[NPKT], out[NPKT];
    gen_traffic(in, NPKT, 0x51ceu);
    long total_bytes = 0;
    for (int i = 0; i < NPKT; i++) total_bytes += in[i].size;
    printf("offered: %d packets, %ld bytes over %ld us\n", NPKT, total_bytes, in[NPKT - 1].t);

    /* policer: token bucket and GCRA must make identical decisions for several burst sizes */
    long bursts[] = {1500, 3000, 6000, 12000};
    for (int k = 0; k < 4; k++) {
        TB b; Gcra g = {bursts[k], 0};
        tb_init(&b, bursts[k]);
        int conf = 0, diff = 0;
        long conf_bytes = 0;
        Pkt kept[NPKT];
        int nk = 0;
        for (int i = 0; i < NPKT; i++) {
            int a = tb_conform(&b, in[i].t, in[i].size);
            int c = gcra_conform(&g, in[i].t, in[i].size);
            if (a != c) diff++;
            if (a) { conf++; conf_bytes += in[i].size; kept[nk++] = in[i]; }
        }
        check(diff == 0, "token bucket and GCRA agree on every packet");
        check(conforms_bruteforce(kept, nk, bursts[k]), "policed output obeys the burst+rate envelope");
        printf("policer burst=%5ld: conforming %3d/%d packets (%ld bytes), token bucket == GCRA\n", bursts[k], conf, NPKT, conf_bytes);
    }

    /* shaper: nothing is dropped if the queue is deep; latency grows instead */
    long qlims[] = {100000, 6000, 3000};
    for (int k = 0; k < 3; k++) {
        long maxd, totd;
        int dropped;
        int m = shape(in, NPKT, 3000, qlims[k], out, &maxd, &totd, &dropped);
        check(m + dropped == NPKT, "every packet was shaped or dropped");
        check(conforms_bruteforce(out, m, 3000), "shaped output obeys the envelope");
        for (int i = 1; i < m; i++) check(out[i].t >= out[i - 1].t, "FIFO order preserved");
        printf("shaper queue<=%6ld: sent %3d dropped %3d max delay %6ld us mean delay %5ld us\n", qlims[k], m, dropped, maxd,
               m ? totd / m : 0);
    }

    /* trTCM colour marking: CIR 125000 B/s (8 us/B), PIR 250000 B/s (4 us/B) implemented with two buckets */
    TrTcm m;
    tb_init(&m.c, 3000);
    tb_init(&m.p, 6000);
    long counts[3] = {0, 0, 0}, bytes[3] = {0, 0, 0};
    for (int i = 0; i < NPKT; i++) {
        int color = trtcm(&m, in[i].t, in[i].size);
        counts[color]++; bytes[color] += in[i].size;
    }
    printf("trTCM: green=%ld (%ld B) yellow=%ld (%ld B) red=%ld (%ld B)\n", counts[0], bytes[0], counts[1], bytes[1], counts[2], bytes[2]);
    check(counts[0] + counts[1] + counts[2] == NPKT, "every packet coloured");
    check(counts[0] > 0 && counts[1] > 0, "both green and yellow occur");
    return 0;
}
