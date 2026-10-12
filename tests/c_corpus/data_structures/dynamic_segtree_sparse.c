/*
 * title: Dynamic sparse segment tree over a huge universe
 * topic: data_structures
 * covers: on-demand node creation, permanent lazy tags without push-down, range add, range sum, universe 2^30, interval overlap brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define UNIV 1073741824L /* 2^30 positions */

typedef struct Node {
    struct Node *lc, *rc;
    long sum; /* sum of everything inside this node's interval, including its own tag */
    long tag; /* add applied to every position of the interval, never pushed */
} Node;

static int nodes;

static Node *mk(void) {
    Node *n = calloc(1, sizeof *n);
    check(n != NULL, "alloc");
    nodes++;
    return n;
}

static void add(Node *n, long lo, long hi, long ql, long qr, long v) {
    if (qr < lo || hi < ql)
        return;
    if (ql <= lo && hi <= qr) {
        n->tag += v;
        n->sum += v * (hi - lo + 1);
        return;
    }
    long mid = lo + (hi - lo) / 2;
    if (!n->lc)
        n->lc = mk();
    if (!n->rc)
        n->rc = mk();
    add(n->lc, lo, mid, ql, qr, v);
    add(n->rc, mid + 1, hi, ql, qr, v);
    n->sum = n->lc->sum + n->rc->sum + n->tag * (hi - lo + 1);
}

/* the tag of every node on the way down covers the overlap with the query */
static long query(const Node *n, long lo, long hi, long ql, long qr) {
    if (!n || qr < lo || hi < ql)
        return 0;
    if (ql <= lo && hi <= qr)
        return n->sum;
    long a = ql > lo ? ql : lo;
    long b = qr < hi ? qr : hi;
    long mid = lo + (hi - lo) / 2;
    return n->tag * (b - a + 1) + query(n->lc, lo, mid, ql, qr) + query(n->rc, mid + 1, hi, ql, qr);
}

static void destroy(Node *n) {
    if (!n)
        return;
    destroy(n->lc);
    destroy(n->rc);
    free(n);
}

typedef struct {
    long l, r, v;
} Upd;

static long rand_pos(void) {
    long hi = (long)rnd(32768);
    long lo = (long)rnd(32768);
    return hi * 32768 + lo; /* uniform in [0, 2^30) */
}

int main(void) {
    Node *root = mk();
    static Upd log_[600];
    int nu = 0;
    long total_added = 0;
    long qsum = 0;
    int nq = 0;
    for (int step = 0; step < 1200; step++) {
        long a = rand_pos(), b = rand_pos();
        if (rnd(4) == 0)
            b = a + (long)rnd(50); /* short interval */
        if (a > b) {
            long t = a;
            a = b;
            b = t;
        }
        if (b >= UNIV)
            b = UNIV - 1;
        if (nu < 600 && rnd(2) == 0) {
            long v = (long)rnd(2001) - 1000;
            add(root, 0, UNIV - 1, a, b, v);
            log_[nu].l = a;
            log_[nu].r = b;
            log_[nu].v = v;
            nu++;
            total_added += v * (b - a + 1);
        } else {
            long want = 0;
            for (int i = 0; i < nu; i++) {
                long lo = log_[i].l > a ? log_[i].l : a;
                long hi = log_[i].r < b ? log_[i].r : b;
                if (lo <= hi)
                    want += log_[i].v * (hi - lo + 1);
            }
            long got = query(root, 0, UNIV - 1, a, b);
            check(got == want, "range sum");
            qsum += got % 1000003;
            nq++;
        }
    }
    check(query(root, 0, UNIV - 1, 0, UNIV - 1) == total_added, "grand total");
    printf("updates=%d queries=%d nodes=%d\n", nu, nq, nodes);
    printf("query digest=%ld total added=%ld\n", qsum, total_added);
    check(nodes <= nu * 240 + 1, "node bound");
    /* single-point probes on an untouched region cost no nodes */
    int before = nodes;
    long z = query(root, 0, UNIV - 1, 5, 5);
    check(nodes == before, "queries never allocate");
    printf("probe at 5 = %ld, nodes unchanged=%d\n", z, nodes == before);
    destroy(root);
    return 0;
}
