/*
 * title: Sliding window transfer with cumulative ACKs
 * topic: networking
 * covers: discrete-event simulation, cumulative ACK, out-of-order buffering, single RTO timer, seeded loss and jitter
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 2048
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

enum { EV_DATA, EV_ACK, EV_RTO };
enum { NSEG = 60, RTO = 70 };

typedef struct {
    int base, next, gen, window;
    int sent, retx, acks_rcvd, done;
    long done_at;
    int rcv_nxt, marked[NSEG], delivered, acks_sent, ooo;
    unsigned long sum;
    int loss_pct;
} Sim;

static Sim sm;

static unsigned payload(int seq) { return (unsigned)seq * 2654435761u + 17u; }

static void xmit(int type, int val) {
    unsigned r = rnd();
    unsigned j = rnd();
    if ((int)(r % 100) < sm.loss_pct) return;
    ev_push(now + 10 + (long)(j % 6), type, type == EV_DATA ? 1 : 0, val, 0, 0, 0);
}
static void arm(void) { ev_push(now + RTO, EV_RTO, 0, ++sm.gen, 0, 0, 0); }
static void fill(void) {
    while (sm.next < sm.base + sm.window && sm.next < NSEG) {
        xmit(EV_DATA, sm.next);
        sm.sent++;
        if (sm.next == sm.base) arm();
        sm.next++;
    }
}

static void run(int window, int loss) {
    memset(&sm, 0, sizeof sm);
    sm.window = window;
    sm.loss_pct = loss;
    rng_state = 0x9e3779b9u ^ (unsigned)(window * 131 + loss);
    evn = 0; now = 0;
    fill();
    while (evn > 0 && !sm.done) {
        Ev e = ev_pop();
        if (e.type == EV_DATA) {
            int seq = e.a;
            if (seq >= sm.rcv_nxt && !sm.marked[seq]) {
                if (seq > sm.rcv_nxt) sm.ooo++;
                sm.marked[seq] = 1;
            }
            while (sm.rcv_nxt < NSEG && sm.marked[sm.rcv_nxt]) {
                sm.sum = sm.sum * 31u + payload(sm.rcv_nxt);
                sm.delivered++;
                sm.rcv_nxt++;
            }
            sm.acks_sent++;
            xmit(EV_ACK, sm.rcv_nxt);
        } else if (e.type == EV_ACK) {
            sm.acks_rcvd++;
            if (e.a > sm.base) {
                sm.base = e.a;
                if (sm.base == NSEG) { sm.done = 1; sm.done_at = now; break; }
                sm.gen++;
                if (sm.base < sm.next) arm();
                fill();
            }
        } else if (e.type == EV_RTO && e.a == sm.gen && sm.base < sm.next) {
            xmit(EV_DATA, sm.base);
            sm.sent++;
            sm.retx++;
            arm();
        }
    }
    check(sm.done, "transfer completes");
    check(sm.delivered == NSEG, "all segments delivered");
    unsigned long want = 0;
    for (int i = 0; i < NSEG; i++) want = want * 31u + payload(i);
    check(want == sm.sum, "in-order payload checksum");
    check(sm.sent == NSEG + sm.retx, "sent = fresh + retransmissions");
    printf("W=%2d loss=%2d%%  done@%4ld sent=%3d retx=%3d acks=%3d/%3d ooo=%3d sum=%08lx\n", window, loss,
           sm.done_at, sm.sent, sm.retx, sm.acks_rcvd, sm.acks_sent, sm.ooo, sm.sum & 0xffffffffu);
}

int main(void) {
    int wins[] = {1, 2, 4, 8, 16};
    int losses[] = {0, 10, 25};
    for (int l = 0; l < 3; l++) {
        for (int w = 0; w < 5; w++) run(wins[w], losses[l]);
        printf("--\n");
    }
    return 0;
}
