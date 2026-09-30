/*
 * title: Raft leader election and log replication over a lossy network
 * topic: networking
 * covers: Raft terms, RequestVote, AppendEntries, log matching, commit by majority, leader crash and recovery, safety invariants
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

enum { NN = 5, MAXLOG = 256, MAXMSG = 60000, LOSS_PCT = 10, HB = 50 };
enum { FOLLOWER, CANDIDATE, LEADER };
static const char *rolename[] = {"follower", "candidate", "leader"};
enum { M_RV, M_RVR, M_AE, M_AER };
enum { EV_MSG, EV_TIMER, EV_CLIENT, EV_CRASH, EV_RESTART };

typedef struct { int term, cmd; } Entry;
typedef struct {
    int type, from, term;
    int last_idx, last_term;          /* RV: candidate log tail */
    int prev_idx, prev_term, nent;    /* AE */
    Entry ent[2];
    int commit;
    int ok, match;                    /* replies */
} Msg;
typedef struct {
    int up, role, term, voted_for, votes;
    Entry log[MAXLOG + 1];
    int loglen, commit, applied;
    int next_idx[NN], match_idx[NN];
    int tgen;                         /* election timer generation */
    int sm[MAXLOG], nsm;              /* applied client commands */
} Node;

static Node nd[NN];
static Msg pool[MAXMSG];
static int npool;
static int leader_of_term[512];
static long sent, lost, elections, leaders_elected, proposed;
static int next_cmd = 1;

static void send_msg(int from, int to, Msg m) {
    m.from = from;
    sent++;
    unsigned r = rnd();
    unsigned d = rnd();
    if ((int)(r % 100) < LOSS_PCT) { lost++; return; }
    if (npool >= MAXMSG) fail("message pool exhausted");
    pool[npool] = m;
    ev_push(now + 5 + (long)(d % 10), EV_MSG, to, npool++, 0, 0, 0);
}
static void reset_election_timer(int n) {
    unsigned r = rnd();
    ev_push(now + 150 + (long)(r % 150), EV_TIMER, n, 0, ++nd[n].tgen, 0, 0);
}
static int last_term(const Node *x) { return x->loglen ? x->log[x->loglen].term : 0; }

static void become_follower(int n, int term) {
    Node *x = &nd[n];
    if (term > x->term) { x->term = term; x->voted_for = -1; }
    x->role = FOLLOWER;
}
static void send_append(int l, int f) {
    Node *x = &nd[l];
    Msg m;
    memset(&m, 0, sizeof m);
    m.type = M_AE; m.term = x->term;
    m.prev_idx = x->next_idx[f] - 1;
    m.prev_term = m.prev_idx > 0 ? x->log[m.prev_idx].term : 0;
    for (int i = x->next_idx[f]; i <= x->loglen && m.nent < 2; i++) m.ent[m.nent++] = x->log[i];
    m.commit = x->commit;
    send_msg(l, f, m);
}
static void broadcast_append(int l) {
    for (int f = 0; f < NN; f++) if (f != l) send_append(l, f);
}
static void become_leader(int n) {
    Node *x = &nd[n];
    x->role = LEADER;
    check(leader_of_term[x->term] == 0, "election safety: one leader per term");
    leader_of_term[x->term] = n + 1;
    leaders_elected++;
    printf("t=%5ld node %d becomes leader of term %d (log length %d, commit %d)\n", now, n, x->term, x->loglen, x->commit);
    if (x->loglen >= MAXLOG) fail("log full");
    x->log[++x->loglen] = (Entry){x->term, 0}; /* no-op lets the leader commit earlier terms */
    for (int f = 0; f < NN; f++) { x->next_idx[f] = x->loglen; x->match_idx[f] = 0; }
    x->match_idx[n] = x->loglen;
    broadcast_append(n);
    ev_push(now + HB, EV_TIMER, n, 1, x->term, 0, 0);
}
static void start_election(int n) {
    Node *x = &nd[n];
    x->role = CANDIDATE; x->term++; x->voted_for = n; x->votes = 1;
    elections++;
    for (int p = 0; p < NN; p++) {
        if (p == n) continue;
        Msg m;
        memset(&m, 0, sizeof m);
        m.type = M_RV; m.term = x->term; m.last_idx = x->loglen; m.last_term = last_term(x);
        send_msg(n, p, m);
    }
    reset_election_timer(n);
}
static void apply_committed(int n) {
    Node *x = &nd[n];
    while (x->applied < x->commit) {
        x->applied++;
        if (x->log[x->applied].cmd != 0) x->sm[x->nsm++] = x->log[x->applied].cmd;
    }
}
static void advance_commit(int l) {
    Node *x = &nd[l];
    for (int idx = x->loglen; idx > x->commit; idx--) {
        int c = 0;
        for (int p = 0; p < NN; p++) if (x->match_idx[p] >= idx) c++;
        if (c * 2 > NN && x->log[idx].term == x->term) { x->commit = idx; break; }
    }
    apply_committed(l);
}

static void handle(int n, const Msg *m) {
    Node *x = &nd[n];
    if (m->term > x->term) become_follower(n, m->term);
    Msg r;
    memset(&r, 0, sizeof r);
    switch (m->type) {
    case M_RV: {
        int up_to_date = m->last_term > last_term(x) || (m->last_term == last_term(x) && m->last_idx >= x->loglen);
        r.type = M_RVR; r.term = x->term;
        if (m->term == x->term && (x->voted_for < 0 || x->voted_for == m->from) && up_to_date) {
            x->voted_for = m->from; r.ok = 1;
            reset_election_timer(n);
        }
        send_msg(n, m->from, r);
        break;
    }
    case M_RVR:
        if (x->role == CANDIDATE && m->term == x->term && m->ok && ++x->votes * 2 > NN) become_leader(n);
        break;
    case M_AE:
        r.type = M_AER; r.term = x->term;
        if (m->term < x->term) { send_msg(n, m->from, r); break; }
        become_follower(n, m->term);
        reset_election_timer(n);
        if (m->prev_idx > x->loglen || (m->prev_idx > 0 && x->log[m->prev_idx].term != m->prev_term)) {
            r.match = x->loglen < m->prev_idx ? x->loglen : m->prev_idx - 1;
            send_msg(n, m->from, r);
            break;
        }
        for (int i = 0; i < m->nent; i++) {
            int idx = m->prev_idx + 1 + i;
            if (idx <= x->loglen && x->log[idx].term != m->ent[i].term) x->loglen = idx - 1; /* truncate conflict */
            if (idx > x->loglen) { if (x->loglen >= MAXLOG) fail("log full"); x->log[++x->loglen] = m->ent[i]; }
        }
        {
            int last_new = m->prev_idx + m->nent;
            if (m->commit > x->commit) { x->commit = m->commit < last_new ? m->commit : last_new; apply_committed(n); }
            r.ok = 1; r.match = last_new;
        }
        send_msg(n, m->from, r);
        break;
    case M_AER:
        if (x->role != LEADER || m->term != x->term) break;
        if (m->ok) {
            if (m->match > x->match_idx[m->from]) x->match_idx[m->from] = m->match;
            x->next_idx[m->from] = x->match_idx[m->from] + 1;
            advance_commit(n);
        } else {
            int nx = m->match + 1;
            x->next_idx[m->from] = nx < x->next_idx[m->from] ? (nx > 1 ? nx : 1) : x->next_idx[m->from];
        }
        break;
    }
}

static void verify(void) {
    /* log matching: same index and term implies identical prefix */
    for (int a = 0; a < NN; a++)
        for (int b = a + 1; b < NN; b++) {
            int lim = nd[a].loglen < nd[b].loglen ? nd[a].loglen : nd[b].loglen;
            int match_from = 0;
            for (int i = lim; i >= 1; i--) {
                if (nd[a].log[i].term == nd[b].log[i].term) { match_from = i; break; }
            }
            for (int i = 1; i <= match_from; i++)
                check(nd[a].log[i].term == nd[b].log[i].term && nd[a].log[i].cmd == nd[b].log[i].cmd, "log matching property");
        }
    /* state machine safety: applied sequences are prefixes of one another */
    for (int a = 0; a < NN; a++)
        for (int b = a + 1; b < NN; b++) {
            int lim = nd[a].nsm < nd[b].nsm ? nd[a].nsm : nd[b].nsm;
            for (int i = 0; i < lim; i++) check(nd[a].sm[i] == nd[b].sm[i], "state machine safety");
        }
}

int main(void) {
    rng_state = 0x5afe1234u;
    for (int i = 0; i < NN; i++) { nd[i].up = 1; nd[i].voted_for = -1; reset_election_timer(i); }
    for (long t = 400; t < 6000; t += 90) ev_push(t, EV_CLIENT, 0, 0, 0, 0, 0);
    ev_push(2000, EV_CRASH, -1, 0, 0, 0, 0);   /* crash whoever leads at t=2000 */
    ev_push(3800, EV_RESTART, -1, 0, 0, 0, 0);
    ev_push(4200, EV_CRASH, -1, 0, 0, 0, 0);   /* crash the leader again */
    ev_push(5200, EV_RESTART, -1, 0, 0, 0, 0);
    int crashed[4] = {-1, -1, -1, -1}, ncr = 0;
    while (evn > 0) {
        Ev e = ev_pop();
        if (now > 9000) break;
        if (e.type == EV_CLIENT) {
            int l = -1;
            for (int i = 0; i < NN; i++) if (nd[i].up && nd[i].role == LEADER && (l < 0 || nd[i].term > nd[l].term)) l = i;
            if (l >= 0 && nd[l].loglen < MAXLOG - 1) {
                nd[l].log[++nd[l].loglen] = (Entry){nd[l].term, next_cmd++};
                nd[l].match_idx[l] = nd[l].loglen;
                proposed++;
                broadcast_append(l);
            }
            continue;
        }
        if (e.type == EV_CRASH) {
            int l = -1;
            for (int i = 0; i < NN; i++) if (nd[i].up && nd[i].role == LEADER && (l < 0 || nd[i].term > nd[l].term)) l = i;
            if (l >= 0) { nd[l].up = 0; crashed[ncr++] = l; printf("t=%5ld node %d (leader) crashes\n", now, l); }
            continue;
        }
        if (e.type == EV_RESTART) {
            for (int k = 0; k < ncr; k++) {
                int n = crashed[k];
                if (n >= 0 && !nd[n].up) {
                    nd[n].up = 1; nd[n].role = FOLLOWER; nd[n].commit = 0; nd[n].applied = 0; nd[n].nsm = 0;
                    reset_election_timer(n);
                    printf("t=%5ld node %d restarts (persistent log length %d, term %d)\n", now, n, nd[n].loglen, nd[n].term);
                }
            }
            continue;
        }
        int n = e.node;
        if (!nd[n].up) continue;
        if (e.type == EV_MSG) handle(n, &pool[e.a]);
        else if (e.type == EV_TIMER) {
            if (e.a == 0 && e.b == nd[n].tgen && nd[n].role != LEADER) start_election(n);
            else if (e.a == 1 && nd[n].role == LEADER && nd[n].term == e.b) {
                broadcast_append(n);
                ev_push(now + HB, EV_TIMER, n, 1, nd[n].term, 0, 0);
            }
        }
        verify();
    }
    printf("proposed=%ld messages=%ld lost=%ld elections started=%ld leaders elected=%ld\n", proposed, sent, lost, elections, leaders_elected);
    int maxc = 0;
    for (int i = 0; i < NN; i++) if (nd[i].commit > maxc) maxc = nd[i].commit;
    for (int i = 0; i < NN; i++) {
        unsigned h = 0;
        for (int k = 0; k < nd[i].nsm; k++) h = h * 131u + (unsigned)nd[i].sm[k];
        printf("node %d: term=%d %-9s log=%d commit=%d applied=%d state hash=%08x\n", i, nd[i].term, rolename[nd[i].role], nd[i].loglen,
               nd[i].commit, nd[i].nsm, h);
    }
    /* after the quiet period every node has caught up to the same state machine */
    for (int i = 1; i < NN; i++) check(nd[i].nsm == nd[0].nsm, "all replicas applied the same number of commands");
    check(nd[0].nsm > 30, "commands were committed");
    for (int k = 0; k < nd[0].nsm; k++) check(nd[0].sm[k] >= 1 && nd[0].sm[k] < next_cmd, "only proposed commands are applied");
    for (int k = 1; k < nd[0].nsm; k++) check(nd[0].sm[k] > nd[0].sm[k - 1], "commands applied in proposal order without duplicates");
    return 0;
}
