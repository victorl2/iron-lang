/*
 * title: Kademlia XOR routing with k-buckets and iterative lookup
 * topic: networking
 * covers: XOR metric, k-bucket LRU with live-node preference, iterative FIND_NODE with alpha, join by self-lookup, churn recovery
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

enum { N = 96, BITS = 16, K = 6, ALPHA = 3, MAXSL = 128 };

typedef struct {
    unsigned id;
    int alive;
    int bucket[BITS][K];
    int cnt[BITS];
} Node;
static Node nd[N];
static long rpcs, pings, evictions;

static int bucket_of(unsigned a, unsigned b) {
    unsigned x = a ^ b;
    int i = 0;
    while (x >>= 1) i++;
    return i;
}
static unsigned dist(unsigned a, unsigned b) { return a ^ b; }

static void remove_contact(int n, int c) {
    int b = bucket_of(nd[n].id, nd[c].id);
    for (int i = 0; i < nd[n].cnt[b]; i++)
        if (nd[n].bucket[b][i] == c) {
            memmove(&nd[n].bucket[b][i], &nd[n].bucket[b][i + 1], sizeof(int) * (size_t)(nd[n].cnt[b] - i - 1));
            nd[n].cnt[b]--;
            return;
        }
}
static void update(int n, int c) {
    if (n == c) return;
    int b = bucket_of(nd[n].id, nd[c].id);
    Node *x = &nd[n];
    for (int i = 0; i < x->cnt[b]; i++)
        if (x->bucket[b][i] == c) { /* seen again: move to the most-recent end */
            memmove(&x->bucket[b][i], &x->bucket[b][i + 1], sizeof(int) * (size_t)(x->cnt[b] - i - 1));
            x->bucket[b][x->cnt[b] - 1] = c;
            return;
        }
    if (x->cnt[b] < K) { x->bucket[b][x->cnt[b]++] = c; return; }
    int head = x->bucket[b][0];
    pings++;
    if (!nd[head].alive) { /* dead head is replaced by the newcomer */
        evictions++;
        memmove(&x->bucket[b][0], &x->bucket[b][1], sizeof(int) * (size_t)(K - 1));
        x->bucket[b][K - 1] = c;
    } else { /* live old contact wins: refresh it, drop the newcomer */
        memmove(&x->bucket[b][0], &x->bucket[b][1], sizeof(int) * (size_t)(K - 1));
        x->bucket[b][K - 1] = head;
    }
}
/* the K contacts of node n closest to target (never n itself) */
static int closest_in_table(int n, unsigned target, int *out) {
    int all[BITS * K], na = 0;
    for (int b = 0; b < BITS; b++) for (int i = 0; i < nd[n].cnt[b]; i++) all[na++] = nd[n].bucket[b][i];
    for (int i = 1; i < na; i++) { /* insertion sort by distance, tie by index */
        int v = all[i], j = i - 1;
        while (j >= 0 && dist(nd[all[j]].id, target) > dist(nd[v].id, target)) { all[j + 1] = all[j]; j--; }
        all[j + 1] = v;
    }
    int m = na < K ? na : K;
    for (int i = 0; i < m; i++) out[i] = all[i];
    return m;
}
static int rpc_find_node(int from, int to, unsigned target, int *out) {
    rpcs++;
    if (!nd[to].alive) return -1;
    update(to, from);
    return closest_in_table(to, target, out);
}

typedef struct { int node; int queried; } Cand;
static int lookup(int self, unsigned target, int *result, int trace) {
    Cand sl[MAXSL];
    int ns = 0;
    int tmp[K];
    int m = closest_in_table(self, target, tmp);
    for (int i = 0; i < m; i++) { sl[ns].node = tmp[i]; sl[ns].queried = 0; ns++; }
    for (int round = 0; round < 32; round++) {
        /* sort shortlist by distance */
        for (int i = 1; i < ns; i++) {
            Cand v = sl[i];
            int j = i - 1;
            while (j >= 0 && dist(nd[sl[j].node].id, target) > dist(nd[v.node].id, target)) { sl[j + 1] = sl[j]; j--; }
            sl[j + 1] = v;
        }
        int sent = 0;
        for (int i = 0; i < ns && i < K && sent < ALPHA; i++) {
            if (sl[i].queried) continue;
            sl[i].queried = 1;
            sent++;
            int out[K];
            int got = rpc_find_node(self, sl[i].node, target, out);
            if (got < 0) { remove_contact(self, sl[i].node); sl[i].node = -1; continue; }
            update(self, sl[i].node);
            for (int k = 0; k < got; k++) {
                int c = out[k], dup = 0;
                if (c == self) continue;
                for (int j = 0; j < ns; j++) if (sl[j].node == c) dup = 1;
                if (!dup && ns < MAXSL) { sl[ns].node = c; sl[ns].queried = 0; ns++; }
            }
        }
        /* drop dead entries */
        int w = 0;
        for (int i = 0; i < ns; i++) if (sl[i].node >= 0) sl[w++] = sl[i];
        ns = w;
        if (trace) {
            printf("  round %d: queried %d, best distance %04x\n", round, sent, ns ? dist(nd[sl[0].node].id, target) : 0xffff);
        }
        if (sent == 0) break;
    }
    int rn = 0;
    for (int i = 0; i < ns && rn < K; i++) result[rn++] = sl[i].node;
    return rn;
}
static int brute_closest(int self, unsigned target, int *out) {
    int idx[N], n = 0;
    for (int i = 0; i < N; i++) if (nd[i].alive && i != self) idx[n++] = i;
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && dist(nd[idx[j]].id, target) > dist(nd[v].id, target)) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    int m = n < K ? n : K;
    for (int i = 0; i < m; i++) out[i] = idx[i];
    return m;
}
static void measure(const char *label, int nq, unsigned seed, int *exact_out) {
    rng_state = seed;
    int exact = 0, hits = 0, want = 0;
    long r0 = rpcs;
    for (int q = 0; q < nq; q++) {
        unsigned a = rnd(), b = rnd();
        int self = (int)(a % N);
        while (!nd[self].alive) self = (self + 1) % N;
        unsigned target = b & 0xffffu;
        int res[K], ideal[K];
        int nr = lookup(self, target, res, 0);
        int ni = brute_closest(self, target, ideal);
        int same = nr == ni;
        for (int i = 0; i < ni; i++) {
            int found = 0;
            for (int j = 0; j < nr; j++) if (res[j] == ideal[i]) found = 1;
            hits += found; want++;
            if (!found) same = 0;
        }
        exact += same;
    }
    printf("%s: %d lookups, exact k-closest sets %d, recall %d/%d, avg RPCs per lookup %ld.%02ld\n", label, nq, exact, hits, want,
           (rpcs - r0) / nq, (rpcs - r0) * 100 / nq % 100);
    *exact_out = exact;
}

int main(void) {
    rng_state = 0x4b4d4c31u;
    for (int i = 0; i < N; i++) {
        unsigned id;
        for (;;) {
            unsigned r = rnd();
            id = r & 0xffffu;
            int dup = 0;
            for (int j = 0; j < i; j++) if (nd[j].id == id) dup = 1;
            if (!dup) break;
        }
        nd[i].id = id; nd[i].alive = 1;
    }
    /* XOR metric properties on samples */
    for (int i = 0; i < 300; i++) {
        unsigned a = rnd() & 0xffffu, b = rnd() & 0xffffu, c = rnd() & 0xffffu;
        check(dist(a, b) == dist(b, a), "symmetry");
        check((dist(a, b) == 0) == (a == b), "identity");
        check(dist(a, c) <= dist(a, b) + dist(b, c), "triangle inequality");
        check((dist(a, b) ^ dist(b, c)) == dist(a, c), "xor composes");
        unsigned d = rnd() & 0xffffu;
        if (dist(a, b) == dist(a, d)) check(b == d, "unidirectional: one point at each distance");
    }
    /* join everyone through node 0 */
    for (int i = 1; i < N; i++) {
        update(i, 0); update(0, i);
        int res[K];
        lookup(i, nd[i].id, res, 0);
        for (int b = 0; b < BITS; b += 3) { /* refresh some far buckets */
            unsigned r = rnd();
            unsigned t = nd[i].id ^ (1u << b) ^ (r & ((1u << b) - 1u));
            lookup(i, t, res, 0);
        }
    }
    long total = 0;
    int minc = 1000, maxc = 0;
    for (int i = 0; i < N; i++) {
        int c = 0;
        for (int b = 0; b < BITS; b++) c += nd[i].cnt[b];
        total += c;
        if (c < minc) minc = c;
        if (c > maxc) maxc = c;
    }
    printf("%d nodes, k=%d alpha=%d: routing table size min=%d max=%d avg=%ld\n", N, K, ALPHA, minc, maxc, total / N);
    int ex1, ex2;
    measure("stable network", 200, 42u, &ex1);
    check(ex1 >= 190, "lookups find the true k closest nodes almost always");
    printf("sample lookup trace:\n");
    int res[K];
    lookup(5, 0xbeefu, res, 1);
    printf("  result ids:");
    for (int i = 0; i < K; i++) printf(" %04x", nd[res[i]].id);
    printf("\n");
    /* churn: a third of the nodes die, lookups must route around the dead ones */
    for (int i = 3; i < N; i += 3) nd[i].alive = 0;
    long p0 = pings;
    measure("after 1/3 nodes died", 200, 43u, &ex2);
    check(ex2 >= 150, "routing survives heavy churn");
    printf("dead-contact pings=%ld evictions=%ld\n", pings - p0, evictions);
    return 0;
}
