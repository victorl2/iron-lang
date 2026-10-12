/*
 * title: Van Emde Boas tree as an integer priority queue
 * topic: data_structures
 * covers: van Emde Boas tree, recursive universe splitting, summary structure, lazily stored minimum, successor and predecessor, multiset counts
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x7EB0ull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

#define NIL (-1)

/* universe is 2^k; min is stored only here, never inside a cluster */
typedef struct Veb {
    int k, lo_bits;
    int min, max;
    struct Veb *summary, **cluster;
} Veb;

static long node_count;

static Veb *veb_new(int k) {
    Veb *v = calloc(1, sizeof(Veb));
    node_count++;
    v->k = k;
    v->min = v->max = NIL;
    if (k > 1) {
        v->lo_bits = k / 2;
        int hi_bits = k - v->lo_bits;
        v->summary = veb_new(hi_bits);
        v->cluster = malloc(sizeof(Veb *) * ((size_t)1 << hi_bits));
        for (int i = 0; i < (1 << hi_bits); i++)
            v->cluster[i] = veb_new(v->lo_bits);
    }
    return v;
}

static void veb_free(Veb *v) {
    if (v->k > 1) {
        for (int i = 0; i < (1 << (v->k - v->lo_bits)); i++)
            veb_free(v->cluster[i]);
        free(v->cluster);
        veb_free(v->summary);
    }
    free(v);
}

static int hi(const Veb *v, int x) { return x >> v->lo_bits; }
static int lo(const Veb *v, int x) { return x & ((1 << v->lo_bits) - 1); }
static int idx(const Veb *v, int h, int l) { return (h << v->lo_bits) | l; }

static void veb_insert(Veb *v, int x) {
    if (v->min == NIL) {
        v->min = v->max = x;
        return;
    }
    if (x == v->min || x == v->max)
        return;
    if (x < v->min) {
        int t = x;
        x = v->min;
        v->min = t;
    }
    if (v->k > 1) {
        Veb *c = v->cluster[hi(v, x)];
        if (c->min == NIL)
            veb_insert(v->summary, hi(v, x));
        veb_insert(c, lo(v, x));
    }
    if (x > v->max)
        v->max = x;
}

static int veb_member(const Veb *v, int x) {
    if (x == v->min || x == v->max)
        return 1;
    if (v->k == 1 || v->min == NIL)
        return 0;
    return veb_member(v->cluster[hi(v, x)], lo(v, x));
}

static void veb_delete(Veb *v, int x) {
    if (v->min == v->max) {
        v->min = v->max = NIL;
        return;
    }
    if (v->k == 1) {
        v->min = x == 0 ? 1 : 0;
        v->max = v->min;
        return;
    }
    if (x == v->min) {
        int first = v->summary->min;
        x = idx(v, first, v->cluster[first]->min);
        v->min = x;
    }
    Veb *c = v->cluster[hi(v, x)];
    veb_delete(c, lo(v, x));
    if (c->min == NIL) {
        veb_delete(v->summary, hi(v, x));
        if (x == v->max) {
            int sm = v->summary->max;
            v->max = sm == NIL ? v->min : idx(v, sm, v->cluster[sm]->max);
        }
    } else if (x == v->max)
        v->max = idx(v, hi(v, x), c->max);
}

static int veb_succ(const Veb *v, int x) {
    if (v->k == 1)
        return (x == 0 && v->max == 1) ? 1 : NIL;
    if (v->min != NIL && x < v->min)
        return v->min;
    const Veb *c = v->cluster[hi(v, x)];
    if (c->max != NIL && lo(v, x) < c->max)
        return idx(v, hi(v, x), veb_succ(c, lo(v, x)));
    int sc = veb_succ(v->summary, hi(v, x));
    if (sc == NIL)
        return NIL;
    return idx(v, sc, v->cluster[sc]->min);
}

static int veb_pred(const Veb *v, int x) {
    if (v->k == 1)
        return (x == 1 && v->min == 0) ? 0 : NIL;
    if (v->max != NIL && x > v->max)
        return v->max;
    const Veb *c = v->cluster[hi(v, x)];
    if (c->min != NIL && lo(v, x) > c->min)
        return idx(v, hi(v, x), veb_pred(c, lo(v, x)));
    int pc = veb_pred(v->summary, hi(v, x));
    if (pc == NIL)
        return (v->min != NIL && x > v->min) ? v->min : NIL;
    return idx(v, pc, v->cluster[pc]->max);
}

/* multiset priority queue on top: the tree holds the distinct keys, cnt holds multiplicities */
typedef struct {
    Veb *t;
    int *cnt;
    long size;
} VPQ;

static void vpq_push(VPQ *q, int x) {
    if (q->cnt[x]++ == 0)
        veb_insert(q->t, x);
    q->size++;
}

static int vpq_pop(VPQ *q) {
    int x = q->t->min;
    if (--q->cnt[x] == 0)
        veb_delete(q->t, x);
    q->size--;
    return x;
}

int main(void) {
    /* phase 1: universe 2^12 against a counting-array model, checking every query type */
    enum { K1 = 12, U1 = 1 << K1 };
    VPQ q = {veb_new(K1), calloc(U1, sizeof(int)), 0};
    static int model[U1];
    long pushes = 0, pops = 0, succ_checks = 0, pred_checks = 0, popsum = 0;
    for (int op = 0; op < 8000; op++) {
        int r = (int)(rng() % 100);
        if (r < 55) {
            int x = (int)(rng() % U1);
            if (rng() % 4 == 0)
                x = (int)(rng() % 64) * 60; /* clustered keys give duplicates */
            model[x]++;
            vpq_push(&q, x);
            pushes++;
        } else if (r < 75 && q.size > 0) {
            int want = 0;
            while (!model[want])
                want++;
            int got = vpq_pop(&q);
            check(got == want, "pop equals smallest key");
            model[want]--;
            popsum += got;
            pops++;
        } else if (r < 88) {
            int x = (int)(rng() % U1), want = NIL;
            for (int y = x + 1; y < U1; y++)
                if (model[y]) {
                    want = y;
                    break;
                }
            check(veb_succ(q.t, x) == want, "successor");
            succ_checks++;
        } else {
            int x = (int)(rng() % U1), want = NIL;
            for (int y = x - 1; y >= 0; y--)
                if (model[y]) {
                    want = y;
                    break;
                }
            check(veb_pred(q.t, x) == want, "predecessor");
            pred_checks++;
        }
        if (op % 500 == 0) {
            int x = (int)(rng() % U1);
            check(veb_member(q.t, x) == (model[x] > 0), "membership");
        }
    }
    printf("universe 2^%d: pushes=%ld pops=%ld succ=%ld pred=%ld popsum=%ld size=%ld\n", K1, pushes, pops, succ_checks, pred_checks, popsum, q.size);
    while (q.size > 0)
        vpq_pop(&q);
    check(q.t->min == NIL && q.t->max == NIL, "empty after drain");
    veb_free(q.t);
    free(q.cnt);

    /* phase 2: universe 2^16, sparse keys; enumerate with successor and compare to sorted order */
    node_count = 0;
    Veb *t = veb_new(16);
    long nodes = node_count;
    enum { NKEYS = 3000 };
    static unsigned char present[1 << 16];
    for (int i = 0; i < NKEYS; i++) {
        int x = (int)(rng() % (1 << 16));
        if (!present[x]) {
            present[x] = 1;
            veb_insert(t, x);
        }
    }
    int expect = 0, distinct = 0, x = t->min;
    long sum = 0;
    while (x != NIL) {
        while (!present[expect])
            expect++;
        check(x == expect, "successor chain enumerates keys in order");
        expect++;
        sum += x;
        distinct++;
        x = veb_succ(t, x);
    }
    int y = t->max, back = 0;
    while (y != NIL) {
        back++;
        y = veb_pred(t, y);
    }
    check(back == distinct, "predecessor chain has the same length");
    printf("universe 2^16: nodes=%ld distinct=%d min=%d max=%d key_sum=%ld\n", nodes, distinct, t->min, t->max, sum);
    veb_free(t);
    return 0;
}
