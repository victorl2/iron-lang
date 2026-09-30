/*
 * title: BGP-like path vector with Gao-Rexford policy
 * topic: networking
 * covers: AS_PATH loop prevention, local preference by relationship, valley-free export, withdrawals, event-driven convergence
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 16384
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

enum { N = 8, MAXP = 9 };
enum { R_NONE, R_CUST, R_PEER, R_PROV }; /* rel[i][j]: what j is to i */
enum { EV_UPDATE };

typedef struct { int len; int as[MAXP]; } Path;
static int rel[N][N];
static int link_up[N][N];
static Path rib_in[N][N][N];   /* [router][neighbor][prefix] */
static Path best[N][N];        /* [router][prefix] */
static int best_from[N][N];    /* neighbor best was learned from, -1 local */
static Path msgpool[16384];
static int nmsg;
static long updates, withdraws, loop_rejects, decisions;
static int policy;

static void set_rel(int provider, int customer) { rel[provider][customer] = R_CUST; rel[customer][provider] = R_PROV; }
static void set_peer(int a, int b) { rel[a][b] = rel[b][a] = R_PEER; }

static int contains(const Path *p, int as) {
    for (int i = 0; i < p->len; i++) if (p->as[i] == as) return 1;
    return 0;
}
static int path_eq(const Path *a, const Path *b) { return a->len == b->len && memcmp(a->as, b->as, sizeof(int) * (size_t)a->len) == 0; }

static void send_update(int from, int to, int prefix, const Path *p) {
    if (nmsg >= 16384) fail("message pool full");
    msgpool[nmsg] = *p;
    if (p->len == 0) withdraws++; else updates++;
    ev_push(now + 1 + (from + to) % 3, EV_UPDATE, to, prefix, from, nmsg++, 0);
}
static int pref_class(int r, int from) { /* higher is better */
    if (!policy) return 0;
    int rr = rel[r][from];
    return rr == R_CUST ? 3 : rr == R_PEER ? 2 : 1;
}
static int may_export(int r, int prefix, int to) {
    if (!policy || best_from[r][prefix] < 0) return 1;
    int learned = rel[r][best_from[r][prefix]];
    return learned == R_CUST || rel[r][to] == R_CUST;
}

static void decide(int r, int prefix) {
    decisions++;
    Path nb;
    int nf = -2;
    memset(&nb, 0, sizeof nb);
    if (r == prefix) { nb.len = 1; nb.as[0] = r; nf = -1; }
    else {
        int bc = -1;
        for (int n = 0; n < N; n++) {
            const Path *c = &rib_in[r][n][prefix];
            if (!link_up[r][n] || c->len == 0) continue;
            int cls = pref_class(r, n);
            int better = 0;
            if (nf == -2) better = 1;
            else if (cls != bc) better = cls > bc;
            else if (c->len != nb.len - 1) better = c->len < nb.len - 1;
            else better = n < nf;
            if (better) {
                nb.len = c->len + 1;
                nb.as[0] = r;
                memcpy(nb.as + 1, c->as, sizeof(int) * (size_t)c->len);
                nf = n; bc = cls;
            }
        }
    }
    if (nf == -2) { nb.len = 0; nf = -3; }
    if (path_eq(&nb, &best[r][prefix]) && nf == best_from[r][prefix]) return;
    best[r][prefix] = nb;
    best_from[r][prefix] = nf;
    for (int n = 0; n < N; n++) {
        if (!link_up[r][n] || n == nf) continue;
        if (nb.len && may_export(r, prefix, n)) send_update(r, n, prefix, &nb);
        else { Path empty; memset(&empty, 0, sizeof empty); send_update(r, n, prefix, &empty); }
    }
}

static void run_events(void) {
    while (evn > 0) {
        Ev e = ev_pop();
        int r = e.node, prefix = e.a, from = e.b;
        if (!link_up[r][from]) continue;
        Path p = msgpool[e.c];
        if (p.len && contains(&p, r)) { loop_rejects++; p.len = 0; }
        if (path_eq(&p, &rib_in[r][from][prefix])) continue;
        rib_in[r][from][prefix] = p;
        decide(r, prefix);
    }
}

static void boot(void) {
    memset(rib_in, 0, sizeof rib_in);
    memset(best, 0, sizeof best);
    for (int i = 0; i < N; i++) for (int p = 0; p < N; p++) best_from[i][p] = -3;
    updates = withdraws = loop_rejects = decisions = 0;
    nmsg = 0; evn = 0; now = 0;
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) link_up[i][j] = rel[i][j] != R_NONE;
    for (int i = 0; i < N; i++) decide(i, i);
    run_events();
}

/* valley-free: up* (peer)? down* */
static int valley_free(const Path *p) {
    int phase = 0; /* 0 climbing, 1 after peer or descending */
    for (int i = 0; i + 1 < p->len; i++) {
        int a = p->as[i], b = p->as[i + 1];
        int r = rel[a][b]; /* what b is to a */
        if (r == R_PROV) { if (phase) return 0; }
        else if (r == R_PEER) { if (phase) return 0; phase = 1; }
        else if (r == R_CUST) phase = 1;
        else return 0;
    }
    return 1;
}

static void fail_link(int a, int b) {
    link_up[a][b] = link_up[b][a] = 0;
    for (int p = 0; p < N; p++) { rib_in[a][b][p].len = 0; rib_in[b][a][p].len = 0; }
    for (int p = 0; p < N; p++) { decide(a, p); decide(b, p); }
    run_events();
}

static void show(int r, int prefix) {
    const Path *p = &best[r][prefix];
    printf("  AS%d -> prefix %d: ", r, prefix);
    if (!p->len) { printf("unreachable\n"); return; }
    for (int i = 0; i < p->len; i++) printf("%s%d", i ? " " : "", p->as[i]);
    printf("\n");
}

static void audit(const char *tag, int expect_full) {
    int reach = 0, pairs = 0;
    for (int r = 0; r < N; r++)
        for (int p = 0; p < N; p++) {
            const Path *b = &best[r][p];
            if (b->len == 0) { pairs++; continue; }
            reach++; pairs++;
            check(b->as[0] == r && b->as[b->len - 1] == p, "path endpoints");
            for (int i = 0; i < b->len; i++)
                for (int j = i + 1; j < b->len; j++) check(b->as[i] != b->as[j], "loop free");
            for (int i = 0; i + 1 < b->len; i++) check(link_up[b->as[i]][b->as[i + 1]], "path uses live links");
            if (policy) check(valley_free(b), "valley-free");
        }
    if (expect_full) check(reach == pairs, "full reachability");
    printf("%s: reachable %d/%d, updates=%ld withdrawals=%ld loop-rejects=%ld decisions=%ld\n", tag, reach, pairs, updates,
           withdraws, loop_rejects, decisions);
}

int main(void) {
    /* tier-1: 0,1 peer; tier-2: 2,3,4; stubs 5,6,7 */
    set_peer(0, 1);
    set_rel(0, 2); set_rel(0, 3); set_rel(1, 3); set_rel(1, 4);
    set_peer(2, 3);
    set_rel(2, 5); set_rel(3, 6); set_rel(3, 7); set_rel(4, 7);
    policy = 1;
    printf("policy routing (customer > peer > provider, valley-free export)\n");
    boot();
    audit("converged", 1);
    show(5, 7); show(5, 6); show(2, 4); show(6, 5);
    check(best[5][7].len == 4, "AS5 reaches AS7 via 2 3 7");
    /* valley-free means the peer link 2-3 can not carry transit for 0's routes */
    printf("failure of link 3-7\n");
    long u0 = updates + withdraws;
    fail_link(3, 7);
    audit("after 3-7 down", 1);
    show(5, 7); show(6, 7);
    printf("  churn: %ld messages\n", updates + withdraws - u0);
    check(best[6][7].len > 0, "AS6 still reaches AS7 through the tier-1 providers");
    check(best[3][7].len > 0 && best[3][7].as[1] != 7, "AS3 detours");

    policy = 0;
    printf("no policy (shortest path, export everything)\n");
    boot();
    audit("converged", 1);
    show(5, 7); show(2, 4);
    fail_link(3, 7);
    audit("after 3-7 down", 1);
    show(5, 7); show(6, 7);
    check(best[6][7].len == 5, "AS6 detours through 1 and 4");
    return 0;
}
