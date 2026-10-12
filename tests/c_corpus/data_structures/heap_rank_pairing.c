/*
 * title: Rank-pairing heap (type 1) with half-ary trees
 * topic: data_structures
 * covers: rank-pairing heap, half-tree binary representation, rank rule repair on cut, decrease-key, delete, one-pass consolidation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 5556667;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 26);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Binary view: left = first child, right = next sibling, p = binary parent.
 * Rank rule for non-roots: rank = r(l)+1 if r(l)==r(r) else max(r(l), r(r)); roots: rank = r(l)+1. */
typedef struct RNode {
    int key, id, rank;
    struct RNode *l, *r, *p, *rnext;
} RNode;

enum { MAXID = 3200, NEG_INF = -1000000 };
static RNode *where[MAXID];
static long links, cuts, rank_fixes;

typedef struct {
    RNode *roots;
    long n;
} RP;

static int rk(const RNode *x) { return x ? x->rank : -1; }

static int rule(const RNode *x) {
    int a = rk(x->l), b = rk(x->r);
    return a == b ? a + 1 : (a > b ? a : b);
}

static void add_root(RP *h, RNode *x) {
    x->p = NULL;
    x->r = NULL;
    x->rank = rk(x->l) + 1;
    x->rnext = h->roots;
    h->roots = x;
}

static void rp_insert(RP *h, int key, int id) {
    RNode *x = calloc(1, sizeof(RNode));
    x->key = key;
    x->id = id;
    where[id] = x;
    add_root(h, x);
    h->n++;
}

static void rp_meld(RP *a, RP *b) {
    if (b->roots) {
        RNode *t = b->roots;
        while (t->rnext)
            t = t->rnext;
        t->rnext = a->roots;
        a->roots = b->roots;
    }
    a->n += b->n;
    b->roots = NULL;
    b->n = 0;
}

static RNode *link_same_rank(RNode *x, RNode *y) {
    if (y->key < x->key) {
        RNode *t = x;
        x = y;
        y = t;
    }
    y->r = x->l;
    if (x->l)
        x->l->p = y;
    x->l = y;
    y->p = x;
    x->rank++;
    links++;
    return x;
}

static void consolidate(RP *h) {
    RNode *bucket[64] = {0};
    RNode *x = h->roots;
    h->roots = NULL;
    while (x) {
        RNode *nx = x->rnext;
        x->rnext = NULL;
        while (bucket[x->rank]) {
            RNode *y = bucket[x->rank];
            bucket[x->rank] = NULL;
            x = link_same_rank(x, y);
        }
        bucket[x->rank] = x;
        x = nx;
    }
    for (int k = 0; k < 64; k++)
        if (bucket[k]) {
            bucket[k]->rnext = h->roots;
            h->roots = bucket[k];
        }
}

static RNode *rp_min(const RP *h) {
    RNode *m = h->roots;
    for (RNode *x = h->roots; x; x = x->rnext)
        if (x->key < m->key)
            m = x;
    return m;
}

static void rp_extract_min(RP *h, int *key, int *id) {
    RNode *m = rp_min(h);
    RNode *rest = NULL;
    for (RNode *x = h->roots; x;) {
        RNode *nx = x->rnext;
        if (x != m) {
            x->rnext = rest;
            rest = x;
        }
        x = nx;
    }
    h->roots = rest;
    for (RNode *c = m->l; c;) {
        RNode *nx = c->r;
        c->rnext = NULL;
        add_root(h, c);
        c = nx;
    }
    *key = m->key;
    *id = m->id;
    where[m->id] = NULL;
    free(m);
    h->n--;
    consolidate(h);
}

static RNode *real_parent(RNode *x) {
    while (x->p && x->p->r == x)
        x = x->p;
    return x->p;
}

static void rp_decrease(RP *h, RNode *x, int nk) {
    check(nk <= x->key, "only decrease");
    x->key = nk;
    if (!x->p)
        return;
    RNode *rp = real_parent(x);
    if (x->key >= rp->key)
        return;
    /* cut x (without its right siblings), which stay where x was */
    RNode *u = x->p, *s = x->r;
    if (u->l == x)
        u->l = s;
    else
        u->r = s;
    if (s)
        s->p = u;
    cuts++;
    add_root(h, x);
    /* repair ranks upward until nothing changes */
    while (u->p) {
        int nr = rule(u);
        if (nr == u->rank)
            break;
        u->rank = nr;
        rank_fixes++;
        u = u->p;
    }
    if (!u->p)
        u->rank = rk(u->l) + 1;
}

static long chk(const RNode *x, const RNode *binp, const RNode *realp) {
    if (!x)
        return 0;
    check(x->p == binp, "binary parent pointer");
    check(!realp || x->key >= realp->key, "heap order against real parent");
    check(x->rank == rule(x), "rank rule");
    check(where[x->id] == x, "handle");
    return 1 + chk(x->l, x, x) + chk(x->r, x, realp);
}

static long rp_check(const RP *h) {
    long n = 0;
    for (const RNode *x = h->roots; x; x = x->rnext) {
        check(!x->p && !x->r, "root has no parent and no right child");
        check(x->rank == rk(x->l) + 1, "root rank");
        check(where[x->id] == x, "root handle");
        n += 1 + chk(x->l, x, x);
    }
    return n;
}

static void destroy(RNode *x) {
    if (!x)
        return;
    destroy(x->l);
    destroy(x->r);
    free(x);
}

int main(void) {
    RP H[2] = {{NULL, 0}, {NULL, 0}};
    int state[MAXID] = {0}, key[MAXID] = {0}, next_id = 0;
    int pops = 0, decs = 0, dels = 0, melds = 0;
    long popsum = 0;
    for (int op = 0; op < 3600; op++) {
        int r = (int)(rng() % 100), s = (int)(rng() % 2);
        if ((r < 50 || H[s].n == 0) && next_id < MAXID) {
            int k = (int)(rng() % 40000);
            key[next_id] = k;
            state[next_id] = s + 1;
            rp_insert(&H[s], k, next_id++);
        } else if (r < 63 && H[s].n) {
            int mk = 1 << 30;
            for (int i = 0; i < next_id; i++)
                if (state[i] == s + 1 && key[i] < mk)
                    mk = key[i];
            int k, id;
            rp_extract_min(&H[s], &k, &id);
            check(k == mk && state[id] == s + 1, "extract-min equals model");
            state[id] = 0;
            pops++;
            popsum += k;
        } else if (r < 86 && H[s].n) {
            int id = (int)(rng() % (unsigned)next_id);
            while (state[id] != s + 1)
                id = (id + 1) % next_id;
            key[id] -= (int)(rng() % 6000);
            rp_decrease(&H[s], where[id], key[id]);
            decs++;
        } else if (r < 93 && H[s].n) {
            int id = (int)(rng() % (unsigned)next_id);
            while (state[id] != s + 1)
                id = (id + 1) % next_id;
            rp_decrease(&H[s], where[id], NEG_INF);
            int k, got;
            rp_extract_min(&H[s], &k, &got);
            check(got == id && k == NEG_INF, "delete removes chosen id");
            state[id] = 0;
            dels++;
        } else if (r >= 97) {
            rp_meld(&H[0], &H[1]);
            for (int i = 0; i < next_id; i++)
                if (state[i] == 2)
                    state[i] = 1;
            melds++;
        }
        for (int h = 0; h < 2; h++)
            check(rp_check(&H[h]) == H[h].n, "size");
    }
    printf("inserted=%d pops=%d decrease=%d deletes=%d melds=%d\n", next_id, pops, decs, dels, melds);
    printf("popsum=%ld live=%ld links=%ld cuts=%ld rank_fixes=%ld\n", popsum, H[0].n + H[1].n, links, cuts, rank_fixes);
    rp_meld(&H[0], &H[1]);
    int prev = -(1 << 30), k, id;
    long n = 0;
    while (H[0].n > 0) {
        rp_extract_min(&H[0], &k, &id);
        check(k >= prev, "drain ascending");
        prev = k;
        n++;
    }
    printf("drained=%ld\n", n);
    for (RNode *x = H[0].roots; x; x = x->rnext)
        destroy(x);
    return 0;
}
