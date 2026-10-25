/*
 * title: Selective repeat ARQ with per-packet timers
 * topic: networking
 * covers: selective repeat, receive buffer, per-packet timers, individual ACKs, window <= modulus/2 rule, reordering
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 8192
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

enum { EV_DATA, EV_ACK, EV_TIMER };
enum { NMSG = 64, MOD = 8, RTO_T = 120 };

typedef struct {
    int window, loss, jitter;
    int base, next;
    int acked[NMSG], gen[NMSG];
    long sent, retx, dropped, buffered;
    int r_base, r_have[MOD], r_msg[MOD];
    int out[NMSG + 32], nout;
    long finish;
} SR;
static SR s;

static void channel(int type, int node, int a, int b) {
    unsigned r1 = rnd(), r2 = rnd();
    if ((int)(r1 % 100) < s.loss) { s.dropped++; return; }
    ev_push(now + 10 + (long)(r2 % (unsigned)(s.jitter + 1)), type, node, a, b, 0, 0);
}
static void tx(int m, int retx) {
    channel(EV_DATA, 1, m % MOD, m);
    s.sent++;
    if (retx) s.retx++;
    ev_push(now + RTO_T, EV_TIMER, 0, m, ++s.gen[m], 0, 0);
}
static void fill(void) {
    while (s.next < s.base + s.window && s.next < NMSG) tx(s.next++, 0);
}
/* is sequence number q inside the receive window [r_base, r_base + window) modulo MOD? */
static int in_window(int q, int lo_abs, int w) {
    int d = (q - lo_abs % MOD + MOD) % MOD;
    return d < w;
}

static int run(int window, int loss, int jitter, int verbose) {
    memset(&s, 0, sizeof s);
    s.window = window; s.loss = loss; s.jitter = jitter;
    rng_state = 31337u + (unsigned)(window * 97 + loss * 13 + jitter);
    evn = 0; now = 0;
    fill();
    while (evn > 0 && s.base < NMSG && s.nout < NMSG + 16) {
        Ev e = ev_pop();
        if (e.type == EV_DATA) {
            int q = e.a, m = e.b;
            if (in_window(q, s.r_base, s.window)) {
                if (!s.r_have[q]) {
                    s.r_have[q] = 1; s.r_msg[q] = m;
                    if (q != s.r_base % MOD) s.buffered++;
                }
                channel(EV_ACK, 0, q, m);
                while (s.r_have[s.r_base % MOD]) {
                    s.out[s.nout++] = s.r_msg[s.r_base % MOD];
                    s.r_have[s.r_base % MOD] = 0;
                    s.r_base++;
                }
            } else if (in_window(q, s.r_base - s.window, s.window)) {
                channel(EV_ACK, 0, q, m); /* old duplicate: re-ack */
            }
        } else if (e.type == EV_ACK) {
            int m = e.b;
            if (m >= s.base && m < s.next) s.acked[m] = 1;
            while (s.base < s.next && s.acked[s.base]) s.base++;
            if (s.base == NMSG) { s.finish = now; break; }
            fill();
        } else if (e.type == EV_TIMER) {
            int m = e.a;
            if (e.b == s.gen[m] && m >= s.base && m < s.next && !s.acked[m]) tx(m, 1);
        }
    }
    int wrong = s.nout != NMSG;
    for (int i = 0; i < s.nout && i < NMSG; i++) if (s.out[i] != i) wrong++;
    if (verbose)
        printf("W=%d loss=%2d%% jitter=%2d: finish@%5ld sent=%3ld retx=%3ld buffered_out_of_order=%3ld wrong=%d\n",
               window, loss, jitter, s.finish, s.sent, s.retx, s.buffered, wrong);
    return wrong;
}

int main(void) {
    int ws[] = {1, 2, 4};
    int ls[] = {0, 10, 30};
    long fin[3][3];
    for (int l = 0; l < 3; l++)
        for (int w = 0; w < 3; w++) {
            check(run(ws[w], ls[l], 15, 1) == 0, "selective repeat with W <= MOD/2 is correct");
            fin[l][w] = s.finish;
        }
    check(fin[0][2] < fin[0][1] && fin[0][1] < fin[0][0], "bigger window is faster");
    /* reordering by jitter needs receive buffering but stays correct */
    check(run(4, 0, 40, 1) == 0, "heavy reordering is handled");
    check(s.buffered > 0, "reordering exercised the receive buffer");
    /* window 5 > MOD/2: the receiver cannot tell a new packet from a retransmitted old one */
    int bad = 0;
    printf("unsafe windows (W > MOD/2):\n");
    for (int loss = 30; loss <= 60; loss += 10) bad += run(6, loss, 30, 1);
    printf("misdelivered in unsafe runs: %d\n", bad);
    check(bad > 0, "window above half the sequence space breaks selective repeat");
    return 0;
}
