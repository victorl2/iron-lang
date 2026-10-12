/*
 * title: Go-Back-N ARQ with modulo sequence numbers
 * topic: networking
 * covers: Go-Back-N, 3-bit sequence space, window size limit modulus-1, cumulative ACK, whole-window retransmission
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 4096
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

enum { EV_DATA, EV_ACK, EV_TIMEOUT };
enum { NMSG = 50, MOD = 8, DELAY = 10, RTO_T = 45 };

typedef struct {
    int window, loss, ack_loss;
    int base, next, gen;
    long sent, resent, timeouts;
    int r_count, delivered[NMSG + 16], ndel;
    long dropped;
    long finish;
} G;
static G g;

static void channel(int type, int node, int a, int b) {
    unsigned r = rnd();
    int pct = type == EV_ACK ? g.ack_loss : g.loss;
    if ((int)(r % 100) < pct) { g.dropped++; return; }
    ev_push(now + DELAY, type, node, a, b, 0, 0);
}
static void arm(void) { ev_push(now + RTO_T, EV_TIMEOUT, 0, ++g.gen, 0, 0, 0); }
static void send_msg(int m) {
    channel(EV_DATA, 1, m % MOD, m);
    g.sent++;
}
static void fill(void) {
    while (g.next < g.base + g.window && g.next < NMSG) {
        if (g.next == g.base) arm();
        send_msg(g.next++);
    }
}

static int run(int window, int loss, int ack_loss, int verbose) {
    memset(&g, 0, sizeof g);
    g.window = window; g.loss = loss; g.ack_loss = ack_loss;
    rng_state = 99u + (unsigned)(window * 1000 + loss + ack_loss * 7);
    evn = 0; now = 0;
    fill();
    while (evn > 0 && g.ndel < NMSG + 8) {
        Ev e = ev_pop();
        if (e.type == EV_DATA) {
            if (e.a == g.r_count % MOD) {
                g.delivered[g.ndel++] = e.b;
                g.r_count++;
            }
            channel(EV_ACK, 0, g.r_count % MOD, 0);
        } else if (e.type == EV_ACK) {
            /* next expected sequence number modulo MOD; distance from base gives how many are acked */
            int adv = (e.a - g.base % MOD + MOD) % MOD;
            if (adv > 0 && adv <= g.next - g.base) {
                g.base += adv;
                g.gen++;
                if (g.base == NMSG) { g.finish = now; break; }
                if (g.base < g.next) arm();
                fill();
            }
        } else if (e.type == EV_TIMEOUT && e.a == g.gen && g.base < g.next) {
            g.timeouts++;
            arm();
            for (int m = g.base; m < g.next; m++) { send_msg(m); g.resent++; }
        }
    }
    int wrong = 0;
    if (g.ndel != NMSG) wrong++;
    for (int i = 0; i < g.ndel && i < NMSG; i++) if (g.delivered[i] != i) wrong++;
    if (verbose)
        printf("W=%d loss=%2d%% ackloss=%2d%%: finish@%5ld sent=%3ld resent=%3ld timeouts=%2ld delivered=%2d wrong=%d\n", window,
               loss, ack_loss, g.finish, g.sent, g.resent, g.timeouts, g.ndel, wrong);
    return wrong;
}

int main(void) {
    int ws[] = {1, 3, 5, 7};
    int ls[] = {0, 10, 25};
    for (int l = 0; l < 3; l++) {
        for (int w = 0; w < 4; w++) check(run(ws[w], ls[l], ls[l], 1) == 0, "GBN with window <= MOD-1 is correct");
    }
    /* window == MOD makes a fully acknowledged window indistinguishable from a fully lost one */
    int bad = 0;
    printf("unsafe window W=8:\n");
    for (int al = 50; al <= 90; al += 10) bad += run(8, 0, al, 1);
    check(bad > 0, "window equal to the modulus corrupts the stream under ACK loss");
    printf("W=8 misdelivered %d messages in total\n", bad);
    return 0;
}
