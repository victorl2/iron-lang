/*
 * title: Fast retransmit on three duplicate ACKs
 * topic: networking
 * covers: duplicate ACK counting, fast retransmit, partial ACK (NewReno style), RTO fallback, tail loss
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 1024
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
enum { NSEG = 40, WIN = 12, RTO_T = 100, DELAY = 10 };
enum { M_RTO_ONLY, M_FAST, M_NEWRENO };
static const char *mname[] = {"rto-only", "fast-retx", "newreno"};

typedef struct {
    int mode;
    int base, next, gen, dup, in_rec, recover;
    int fast, rto_fires, partial, sent;
    int rcv_nxt, marked[NSEG];
    int first_tx_dropped[NSEG];
    int sent_once[NSEG];
    long done_at;
} S;
static S s;

static void tx(int seq) {
    s.sent++;
    int first = !s.sent_once[seq];
    s.sent_once[seq] = 1;
    if (first && s.first_tx_dropped[seq]) return;
    ev_push(now + DELAY, EV_DATA, 1, seq, 0, 0, 0);
}
static void arm(void) { ev_push(now + RTO_T, EV_RTO, 0, ++s.gen, 0, 0, 0); }
static void fill(void) {
    while (s.next < s.base + WIN && s.next < NSEG) {
        tx(s.next);
        if (s.next == s.base) arm();
        s.next++;
    }
}

static void run(const char *label, int mode, const int *drops, int nd) {
    memset(&s, 0, sizeof s);
    s.mode = mode;
    for (int i = 0; i < nd; i++) s.first_tx_dropped[drops[i]] = 1;
    evn = 0; now = 0;
    fill();
    while (evn > 0) {
        Ev e = ev_pop();
        if (e.type == EV_DATA) {
            s.marked[e.a] = 1;
            while (s.rcv_nxt < NSEG && s.marked[s.rcv_nxt]) s.rcv_nxt++;
            ev_push(now + DELAY, EV_ACK, 0, s.rcv_nxt, 0, 0, 0);
        } else if (e.type == EV_ACK) {
            int a = e.a;
            if (a > s.base) {
                int was_partial = s.in_rec && a < s.recover;
                s.base = a;
                s.dup = 0;
                if (s.base == NSEG) { s.done_at = now; break; }
                if (s.in_rec && a >= s.recover) s.in_rec = 0;
                if (was_partial && s.mode == M_NEWRENO) { tx(s.base); s.partial++; }
                s.gen++;
                if (s.base < s.next) arm();
                fill();
            } else if (a == s.base && s.base < s.next) {
                s.dup++;
                if (s.dup == 3 && s.mode != M_RTO_ONLY && !s.in_rec) {
                    tx(s.base);
                    s.fast++;
                    s.in_rec = 1;
                    s.recover = s.next;
                }
            }
        } else if (e.type == EV_RTO && e.a == s.gen && s.base < s.next) {
            tx(s.base);
            s.rto_fires++;
            s.dup = 0;
            s.in_rec = 0;
            arm();
        }
    }
    check(s.rcv_nxt == NSEG, "everything delivered");
    printf("  %-10s %-16s done@%4ld sent=%2d fast=%d partial=%d rto=%d\n", mname[mode], label, s.done_at, s.sent, s.fast,
           s.partial, s.rto_fires);
}

int main(void) {
    static const int one[] = {5};
    static const int two[] = {5, 7};
    static const int three[] = {5, 6, 7};
    static const int tail[] = {39};
    static const int mixed[] = {3, 20, 21, 37, 38};
    struct { const char *n; const int *d; int k; } cases[] = {
        {"single loss", one, 1}, {"two losses", two, 2}, {"burst of three", three, 3},
        {"tail loss", tail, 1}, {"mixed", mixed, 5},
    };
    long t[5][3];
    for (int c = 0; c < 5; c++) {
        printf("%s\n", cases[c].n);
        for (int m = 0; m < 3; m++) {
            run(cases[c].n, m, cases[c].d, cases[c].k);
            t[c][m] = s.done_at;
        }
    }
    check(t[0][M_FAST] < t[0][M_RTO_ONLY], "fast retransmit beats RTO for a single loss");
    check(t[1][M_NEWRENO] <= t[1][M_FAST], "partial ACKs help with two losses");
    check(t[3][M_FAST] == t[3][M_RTO_ONLY], "tail loss needs the RTO regardless");
    printf("single-loss speedup: %ld ticks\n", t[0][M_RTO_ONLY] - t[0][M_FAST]);
    return 0;
}
