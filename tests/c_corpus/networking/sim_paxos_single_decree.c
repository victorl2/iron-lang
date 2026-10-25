/*
 * title: Single-decree Paxos with dueling proposers
 * topic: networking
 * covers: prepare/promise, accept/accepted, ballot numbers, value adoption, majority quorum, message loss, safety across many seeds
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

enum { NP = 3, NA = 5, MAJ = 3, MAXROUND = 8, MAXMSG = 20000 };
enum { M_P1A, M_P1B, M_P2A, M_P2B };
enum { EV_MSG, EV_TIMER, EV_START };
enum { TO_PROPOSER, TO_ACCEPTOR, TO_LEARNER };

typedef struct { int type, from, ballot, acc_ballot, acc_val, val; } Msg;
typedef struct {
    int round, phase, promises, best_ballot, best_val, my_val, ballot, accepts, gen, done;
    int adopted;
} Prop;
typedef struct { int promised, acc_ballot, acc_val; } Acc;

static Prop pr[NP];
static Acc ac[NA];
static Msg pool[MAXMSG];
static int npool;
static int seen[512][NA];      /* learner: acceptor a accepted ballot b */
static int seen_val[512];
static int chosen_val, chosen_at, loss_pct;
static long msgs, drops, adoptions;
static int ballots_started;

static int ballot_of(int round, int p) { return (round + 1) * 10 + p; }

static void send(int kind, int dst, Msg m) {
    msgs++;
    unsigned r = rnd(), d = rnd();
    if ((int)(r % 100) < loss_pct) { drops++; return; }
    if (npool >= MAXMSG) fail("message pool full");
    pool[npool] = m;
    ev_push(now + 3 + (long)(d % 12), EV_MSG, dst, npool++, kind, 0, 0);
}
static void start_round(int p) {
    Prop *x = &pr[p];
    if (x->done || x->round >= MAXROUND) return;
    x->ballot = ballot_of(x->round++, p);
    x->phase = 1; x->promises = 0; x->accepts = 0; x->best_ballot = 0; x->best_val = 0;
    ballots_started++;
    for (int a = 0; a < NA; a++) {
        Msg m = {M_P1A, p, x->ballot, 0, 0, 0};
        send(TO_ACCEPTOR, a, m);
    }
    unsigned j = rnd();
    ev_push(now + 60 + (long)(j % 40) + p * 7, EV_TIMER, p, ++x->gen, 0, 0, 0);
}
static void on_learner(int a, int ballot, int val) {
    seen[ballot][a] = 1;
    seen_val[ballot] = val;
    int c = 0;
    for (int i = 0; i < NA; i++) c += seen[ballot][i];
    if (c >= MAJ) {
        if (chosen_val == 0) { chosen_val = val; chosen_at = (int)now; }
        check(chosen_val == val, "SAFETY: two different values chosen");
    }
}
static void deliver(const Ev *e) {
    Msg m = pool[e->a];
    if (e->b == TO_ACCEPTOR) {
        Acc *a = &ac[e->node];
        if (m.type == M_P1A) {
            if (m.ballot > a->promised) {
                a->promised = m.ballot;
                Msg r = {M_P1B, e->node, m.ballot, a->acc_ballot, a->acc_val, 0};
                send(TO_PROPOSER, m.from, r);
            }
        } else if (m.type == M_P2A) {
            if (m.ballot >= a->promised) {
                a->promised = m.ballot; a->acc_ballot = m.ballot; a->acc_val = m.val;
                Msg r = {M_P2B, e->node, m.ballot, 0, 0, m.val};
                send(TO_LEARNER, 0, r);
                send(TO_PROPOSER, m.from, r);
            }
        }
    } else if (e->b == TO_PROPOSER) {
        Prop *x = &pr[e->node];
        if (m.ballot != x->ballot || x->done) return;
        if (m.type == M_P1B && x->phase == 1) {
            x->promises++;
            if (m.acc_ballot > x->best_ballot) { x->best_ballot = m.acc_ballot; x->best_val = m.acc_val; }
            if (x->promises == MAJ) {
                int v = x->my_val;
                if (x->best_ballot > 0) { v = x->best_val; x->adopted++; adoptions++; } /* must adopt the highest accepted value */
                x->phase = 2;
                for (int a = 0; a < NA; a++) {
                    Msg r = {M_P2A, e->node, x->ballot, 0, 0, v};
                    send(TO_ACCEPTOR, a, r);
                }
            }
        } else if (m.type == M_P2B && x->phase == 2) {
            if (++x->accepts == MAJ) { x->phase = 3; x->done = 1; }
        }
    } else {
        on_learner(m.from, m.ballot, m.val);
    }
}

static void run(unsigned seed, int loss, int verbose) {
    memset(pr, 0, sizeof pr); memset(ac, 0, sizeof ac); memset(seen, 0, sizeof seen); memset(seen_val, 0, sizeof seen_val);
    npool = 0; chosen_val = 0; chosen_at = -1; loss_pct = loss; ballots_started = 0; evn = 0; now = 0;
    rng_state = seed;
    for (int p = 0; p < NP; p++) {
        pr[p].my_val = 100 + p;
        unsigned s = rnd();
        ev_push((long)(s % 30), EV_START, p, 0, 0, 0, 0);
    }
    while (evn > 0 && now < 3000) {
        Ev e = ev_pop();
        if (e.type == EV_START) start_round(e.node);
        else if (e.type == EV_TIMER) { if (e.a == pr[e.node].gen && !pr[e.node].done) start_round(e.node); }
        else deliver(&e);
    }
    /* every accepted (ballot,value) pair on acceptors must agree with the chosen value once chosen */
    if (chosen_val) {
        int hi = 0;
        for (int b = 0; b < 512; b++) {
            int c = 0;
            for (int a = 0; a < NA; a++) c += seen[b][a];
            if (c >= MAJ) hi = b;
        }
        for (int b = hi; b < 512; b++)
            for (int a = 0; a < NA; a++)
                if (seen[b][a]) check(seen_val[b] == chosen_val, "any later accepted value equals the chosen value");
    }
    if (verbose)
        printf("seed %3u loss %2d%%: chosen=%d at t=%d ballots started=%d adoptions so far=%ld\n", seed, loss, chosen_val, chosen_at,
               ballots_started, adoptions);
}

int main(void) {
    long decided = 0, runs = 0, total_ballots = 0;
    long by_value[3] = {0, 0, 0};
    int losses[3] = {0, 20, 40};
    for (int li = 0; li < 3; li++) {
        for (unsigned s = 1; s <= 60; s++) {
            run(s * 7919u + (unsigned)li, losses[li], s <= 3);
            runs++;
            total_ballots += ballots_started;
            if (chosen_val) { decided++; by_value[chosen_val - 100]++; }
        }
    }
    printf("runs=%ld decided=%ld total ballots=%ld adoptions=%ld\n", runs, decided, total_ballots, adoptions);
    printf("chosen value by proposer: p0=%ld p1=%ld p2=%ld\n", by_value[0], by_value[1], by_value[2]);
    check(decided > runs * 8 / 10, "most runs decide despite loss");
    check(adoptions > 0, "some proposers were forced to adopt an earlier accepted value");
    return 0;
}
