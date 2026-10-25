/*
 * title: Causal broadcast with vector clocks and a hold-back queue
 * topic: networking
 * covers: vector clock timestamps, causal delivery condition, hold-back queue, duplicate suppression, comparison with arrival-order and per-sender FIFO delivery
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

enum { NP = 5, NMSG = 60, HOLD = 64 };
enum { EV_SEND, EV_ARRIVE };
enum { D_ARRIVAL, D_FIFO, D_CAUSAL };
static const char *dname[] = {"arrival order", "per-sender FIFO", "causal (vector clock)"};

typedef struct { int sender; int vc[NP]; unsigned long long deps; long sent_at; } Msg;
static Msg msgs[NMSG];
static int nmsgs;

typedef struct {
    int vc[NP];                       /* delivered counts per sender */
    unsigned long long delivered;     /* bitmask of message ids */
    int hold[HOLD], nhold;
    int fifo_next[NP];
    int seen[NMSG];
    long violations, delivered_count, max_hold, total_delay;
    int order[NMSG], norder;
} Proc;
static Proc pr[NP];

static int deliverable(const Proc *p, const Msg *m, int mode) {
    if (mode == D_ARRIVAL) return 1;
    if (mode == D_FIFO) return m->vc[m->sender] == p->fifo_next[m->sender] + 1;
    /* causal: next message from that sender, and everything the sender had seen from others is already delivered here */
    if (m->vc[m->sender] != p->vc[m->sender] + 1) return 0;
    for (int k = 0; k < NP; k++) if (k != m->sender && m->vc[k] > p->vc[k]) return 0;
    return 1;
}
static void do_deliver(int who, int id, int mode) {
    Proc *p = &pr[who];
    Msg *m = &msgs[id];
    if (m->deps & ~p->delivered & ~(1ull << id)) p->violations++;
    p->delivered |= 1ull << id;
    p->vc[m->sender]++;
    p->fifo_next[m->sender]++;
    p->delivered_count++;
    p->total_delay += now - m->sent_at;
    p->order[p->norder++] = id;
    (void)mode;
}

static void run(int mode, int verbose) {
    memset(pr, 0, sizeof pr);
    nmsgs = 0; evn = 0; now = 0;
    rng_state = 0xca05a1u;
    long t = 0;
    for (int i = 0; i < NMSG; i++) {
        unsigned a = rnd(), b = rnd();
        t += 1 + (long)(a % 9);
        ev_push(t, EV_SEND, (int)(b % NP), 0, 0, 0, 0);
    }
    while (evn > 0) {
        Ev e = ev_pop();
        if (e.type == EV_SEND) {
            int s = e.node;
            int id = nmsgs++;
            Msg *m = &msgs[id];
            m->sender = s;
            memcpy(m->vc, pr[s].vc, sizeof m->vc);
            m->vc[s]++;                       /* own send counts as delivered locally */
            m->deps = pr[s].delivered;
            m->sent_at = now;
            do_deliver(s, id, mode);          /* a sender delivers its own message immediately */
            for (int q = 0; q < NP; q++) {
                if (q == s) continue;
                unsigned r = rnd();
                unsigned dup = rnd();
                long d = 1 + (long)(r % 40);
                ev_push(now + d, EV_ARRIVE, q, id, 0, 0, 0);
                if (dup % 10 == 0) ev_push(now + d + 1 + (long)(dup % 30), EV_ARRIVE, q, id, 0, 0, 0); /* network duplicate */
            }
        } else {
            Proc *p = &pr[e.node];
            int id = e.a;
            if (p->seen[id]) continue; /* duplicate suppression by message id */
            p->seen[id] = 1;
            if (p->nhold >= HOLD) fail("hold-back queue overflow");
            p->hold[p->nhold++] = id;
            if (p->nhold > p->max_hold) p->max_hold = p->nhold;
            int progress = 1;
            while (progress) {
                progress = 0;
                for (int i = 0; i < p->nhold; i++) {
                    if (deliverable(p, &msgs[p->hold[i]], mode)) {
                        int did = p->hold[i];
                        p->hold[i] = p->hold[--p->nhold];
                        do_deliver(e.node, did, mode);
                        progress = 1;
                        break;
                    }
                }
            }
        }
    }
    long viol = 0, mh = 0, td = 0, dc = 0;
    for (int i = 0; i < NP; i++) {
        viol += pr[i].violations; if (pr[i].max_hold > mh) mh = pr[i].max_hold;
        td += pr[i].total_delay; dc += pr[i].delivered_count;
    }
    if (verbose)
        printf("%-22s delivered=%3ld causal violations=%3ld max hold-back=%2ld mean delivery delay=%ld ticks\n", dname[mode], dc, viol, mh, td / dc);
    for (int i = 0; i < NP; i++) {
        check(pr[i].nhold == 0, "nothing left in the hold-back queue");
        check(pr[i].delivered_count == NMSG, "every process delivers every message exactly once");
    }
    if (mode == D_CAUSAL) check(viol == 0, "causal delivery never violates happened-before");
    else check(viol > 0, "weaker orderings do violate causality here");
}

int main(void) {
    for (int mode = 0; mode < 3; mode++) run(mode, 1);
    /* under causal delivery all processes may interleave concurrent messages differently, but causally related ones agree */
    run(D_CAUSAL, 0);
    int diff_pairs = 0, agree_pairs = 0;
    for (int a = 0; a < NMSG; a++)
        for (int b = a + 1; b < NMSG; b++) {
            int related = (msgs[b].deps >> a) & 1;
            int before[NP];
            for (int p = 0; p < NP; p++) {
                int pa = -1, pb = -1;
                for (int i = 0; i < NMSG; i++) { if (pr[p].order[i] == a) pa = i; if (pr[p].order[i] == b) pb = i; }
                before[p] = pa < pb;
            }
            int same = 1;
            for (int p = 1; p < NP; p++) if (before[p] != before[0]) same = 0;
            if (related) { check(same && before[0], "causally related messages are delivered in causal order everywhere"); agree_pairs++; }
            else if (!same) diff_pairs++;
        }
    printf("causally related pairs (same order at every process): %d; concurrent pairs ordered differently somewhere: %d\n", agree_pairs, diff_pairs);
    check(diff_pairs > 0, "concurrent messages are allowed to interleave differently");
    return 0;
}
