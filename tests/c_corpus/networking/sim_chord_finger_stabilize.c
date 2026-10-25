/*
 * title: Chord ring: fingers, joins, stabilization and failures
 * topic: networking
 * covers: consistent identifier ring, finger tables, O(log N) lookup hops, join/stabilize/notify, successor lists, failure repair
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

enum { M = 8, SPACE = 256, MAXN = 32, R = 3 };

typedef struct {
    int id, alive;
    int succ[R];        /* successor list of node indexes, -1 = none */
    int pred;
    int finger[M];
} Node;
static Node nd[MAXN];
static int nn;

static int in_half(int x, int a, int b) { /* x in (a, b] on the ring */
    if (a < b) return x > a && x <= b;
    if (a > b) return x > a || x <= b;
    return 1; /* a == b: the whole ring */
}
static int in_open(int x, int a, int b) { /* x in (a, b) */
    if (a < b) return x > a && x < b;
    if (a > b) return x > a || x < b;
    return x != a;
}
/* ground truth over live nodes: first node id >= key going clockwise */
static int ideal_owner(int key) {
    int best = -1, bd = SPACE + 1;
    for (int i = 0; i < nn; i++) {
        if (!nd[i].alive) continue;
        int d = (nd[i].id - key + SPACE) % SPACE;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static int first_alive_succ(int n) {
    for (int k = 0; k < R; k++) if (nd[n].succ[k] >= 0 && nd[nd[n].succ[k]].alive) return nd[n].succ[k];
    return n;
}
static int closest_preceding(int n, int key) {
    for (int i = M - 1; i >= 0; i--) {
        int f = nd[n].finger[i];
        if (f >= 0 && nd[f].alive && in_open(nd[f].id, nd[n].id, key)) return f;
    }
    return n;
}
static int find_successor(int n, int key, int *hops) {
    *hops = 0;
    for (int guard = 0; guard < 64; guard++) {
        int s = first_alive_succ(n);
        if (in_half(key, nd[n].id, nd[s].id)) return s;
        int c = closest_preceding(n, key);
        if (c == n) return s;
        n = c;
        (*hops)++;
    }
    return -1;
}
static void set_ideal_tables(void) {
    for (int i = 0; i < nn; i++) {
        if (!nd[i].alive) continue;
        for (int f = 0; f < M; f++) nd[i].finger[f] = ideal_owner((nd[i].id + (1 << f)) % SPACE);
        int cur = i;
        for (int k = 0; k < R; k++) {
            cur = ideal_owner((nd[cur].id + 1) % SPACE);
            nd[i].succ[k] = cur;
        }
        nd[i].pred = -1;
    }
}
static int add_node(int id) {
    nd[nn].id = id; nd[nn].alive = 1; nd[nn].pred = -1;
    for (int k = 0; k < R; k++) nd[nn].succ[k] = -1;
    for (int f = 0; f < M; f++) nd[nn].finger[f] = -1;
    return nn++;
}
static void join(int n, int boot) {
    int h;
    int s = find_successor(boot, nd[n].id, &h);
    nd[n].succ[0] = s;
    nd[n].pred = -1;
}
static void notify(int s, int n) {
    if (nd[s].pred < 0 || !nd[nd[s].pred].alive || in_open(nd[n].id, nd[nd[s].pred].id, nd[s].id)) nd[s].pred = n;
}
static void stabilize(int n) {
    int s = first_alive_succ(n);
    int x = nd[s].pred;
    if (x >= 0 && nd[x].alive && x != n && in_open(nd[x].id, nd[n].id, nd[s].id)) s = x;
    nd[n].succ[0] = s;
    /* refresh the rest of the successor list from s's list */
    for (int k = 1; k < R; k++) nd[n].succ[k] = nd[s].succ[k - 1] == n ? -1 : nd[s].succ[k - 1];
    if (s != n) notify(s, n);
    else if (nd[n].pred < 0) nd[n].pred = n;
}
static void fix_fingers(int n) {
    int h;
    for (int f = 0; f < M; f++) nd[n].finger[f] = find_successor(n, (nd[n].id + (1 << f)) % SPACE, &h);
}
static int ring_ok(void) {
    for (int i = 0; i < nn; i++) {
        if (!nd[i].alive) continue;
        if (nd[i].succ[0] < 0 || nd[i].succ[0] != ideal_owner((nd[i].id + 1) % SPACE)) return 0;
    }
    return 1;
}
static int fingers_ok(void) {
    for (int i = 0; i < nn; i++) {
        if (!nd[i].alive) continue;
        for (int f = 0; f < M; f++) if (nd[i].finger[f] != ideal_owner((nd[i].id + (1 << f)) % SPACE)) return 0;
    }
    return 1;
}
static int lookup_errors(void) {
    int bad = 0, h;
    for (int i = 0; i < nn; i++) {
        if (!nd[i].alive) continue;
        for (int key = 0; key < SPACE; key += 5) {
            int r = find_successor(i, key, &h);
            if (r != ideal_owner(key)) bad++;
        }
    }
    return bad;
}
static int rounds_until_stable(int max) {
    int r = 0;
    while (r < max && !(ring_ok() && fingers_ok())) {
        for (int i = 0; i < nn; i++) if (nd[i].alive) stabilize(i);
        for (int i = 0; i < nn; i++) if (nd[i].alive) fix_fingers(i);
        r++;
    }
    return r;
}

int main(void) {
    rng_state = 0xC40Du;
    int used[SPACE] = {0};
    int ids[24], cnt = 0;
    while (cnt < 24) {
        unsigned r = rnd();
        int id = (int)(r % SPACE);
        if (!used[id]) { used[id] = 1; ids[cnt++] = id; }
    }
    /* phase A: ideal tables, hop counts over every start node and key */
    for (int i = 0; i < 16; i++) add_node(ids[i]);
    set_ideal_tables();
    long hist[10] = {0}, total = 0, count = 0;
    int maxh = 0;
    for (int i = 0; i < nn; i++)
        for (int key = 0; key < SPACE; key++) {
            int h, r = find_successor(i, key, &h);
            check(r == ideal_owner(key), "finger lookup returns the true successor");
            hist[h < 9 ? h : 9]++; total += h; count++;
            if (h > maxh) maxh = h;
        }
    printf("16 nodes, %ld lookups: mean hops=%ld.%02ld max=%d (log2 N = 4)\n", count, total / count, total * 100 / count % 100, maxh);
    printf("hop histogram:");
    for (int h = 0; h < 6; h++) printf(" %d:%ld", h, hist[h]);
    printf("\n");
    check(maxh <= M, "at most m hops");
    check(total * 100 / count < 300, "about half of log2 N hops on average");

    /* phase B: build the same ring by joins */
    nn = 0;
    memset(nd, 0, sizeof nd);
    add_node(ids[0]);
    nd[0].succ[0] = 0; nd[0].pred = 0;
    for (int f = 0; f < M; f++) nd[0].finger[f] = 0;
    for (int i = 1; i < 16; i++) {
        int n = add_node(ids[i]);
        join(n, 0);
        /* one round of periodic maintenance after each join */
        for (int k = 0; k < nn; k++) stabilize(k);
    }
    int stale = lookup_errors();
    int r = rounds_until_stable(60);
    printf("joined 16 nodes: %d lookups wrong before finger repair, stable after %d more rounds, wrong after=%d\n", stale, r, lookup_errors());
    check(ring_ok() && fingers_ok(), "joins converge to the ideal ring");
    check(lookup_errors() == 0, "lookups correct after convergence");

    /* phase C: 8 more nodes join at once, then 4 fail (two adjacent) */
    for (int i = 16; i < 24; i++) { int n = add_node(ids[i]); join(n, 0); }
    r = rounds_until_stable(60);
    printf("8 concurrent joins: stable after %d rounds\n", r);
    check(ring_ok() && fingers_ok(), "concurrent joins converge");
    int order[MAXN], no = 0;
    for (int i = 0; i < nn; i++) order[no++] = i;
    /* sort by id (insertion sort) to pick adjacent victims */
    for (int i = 1; i < no; i++) { int v = order[i], j = i - 1; while (j >= 0 && nd[order[j]].id > nd[v].id) { order[j + 1] = order[j]; j--; } order[j + 1] = v; }
    int victims[4] = {order[3], order[4], order[11], order[19]}; /* 3 and 4 are adjacent on the ring */
    for (int k = 0; k < 4; k++) nd[victims[k]].alive = 0;
    printf("failed node ids: %d %d %d %d\n", nd[victims[0]].id, nd[victims[1]].id, nd[victims[2]].id, nd[victims[3]].id);
    int err = lookup_errors();
    r = rounds_until_stable(60);
    printf("after 4 failures: %d wrong lookups before repair, ring repaired in %d rounds, wrong after=%d\n", err, r, lookup_errors());
    check(ring_ok() && fingers_ok() && lookup_errors() == 0, "ring heals after failures thanks to successor lists");
    int alive = 0;
    for (int i = 0; i < nn; i++) alive += nd[i].alive;
    printf("alive nodes: %d\n", alive);
    return 0;
}
