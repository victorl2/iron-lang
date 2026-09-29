/*
 * title: Meldable heaps compared on identical operation scripts
 * topic: data_structures
 * covers: leftist heap, skew heap, pairing heap, binomial heap, function-pointer vtable, comparison counting, meld workloads
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0xABCDEF12345ull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;
static int lt(int a, int b) {
    cmps++;
    return a < b;
}

typedef struct {
    const char *name;
    void *(*create)(void);
    void (*insert)(void *, int);
    void (*meld)(void *, void *); /* second heap is consumed and freed */
    int (*pop)(void *);
    int (*size)(void *);
    void (*destroy)(void *);
} Ops;

/* ---------- leftist ---------- */
typedef struct LN {
    int key, npl;
    struct LN *l, *r;
} LN;
typedef struct {
    LN *root;
    int n;
} LH;

static int lnpl(LN *x) { return x ? x->npl : -1; }
static LN *lmeld(LN *a, LN *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (lt(b->key, a->key)) {
        LN *t = a;
        a = b;
        b = t;
    }
    a->r = lmeld(a->r, b);
    if (lnpl(a->l) < lnpl(a->r)) {
        LN *t = a->l;
        a->l = a->r;
        a->r = t;
    }
    a->npl = lnpl(a->r) + 1;
    return a;
}
static void *l_create(void) { return calloc(1, sizeof(LH)); }
static void l_insert(void *h, int k) {
    LH *x = h;
    LN *n = calloc(1, sizeof(LN));
    n->key = k;
    x->root = lmeld(x->root, n);
    x->n++;
}
static void l_meld(void *a, void *b) {
    LH *x = a, *y = b;
    x->root = lmeld(x->root, y->root);
    x->n += y->n;
    free(y);
}
static int l_pop(void *h) {
    LH *x = h;
    LN *r = x->root;
    int k = r->key;
    x->root = lmeld(r->l, r->r);
    free(r);
    x->n--;
    return k;
}
static int l_size(void *h) { return ((LH *)h)->n; }
static void lfree(LN *x) {
    if (x) {
        lfree(x->l);
        lfree(x->r);
        free(x);
    }
}
static void l_destroy(void *h) {
    lfree(((LH *)h)->root);
    free(h);
}

/* ---------- skew ---------- */
typedef struct SN {
    int key;
    struct SN *l, *r;
} SN;
typedef struct {
    SN *root;
    int n;
} SH;
static SN *smeld(SN *a, SN *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (lt(b->key, a->key)) {
        SN *t = a;
        a = b;
        b = t;
    }
    SN *t = a->l;
    a->l = smeld(a->r, b);
    a->r = t;
    return a;
}
static void *s_create(void) { return calloc(1, sizeof(SH)); }
static void s_insert(void *h, int k) {
    SH *x = h;
    SN *n = calloc(1, sizeof(SN));
    n->key = k;
    x->root = smeld(x->root, n);
    x->n++;
}
static void s_meld(void *a, void *b) {
    SH *x = a, *y = b;
    x->root = smeld(x->root, y->root);
    x->n += y->n;
    free(y);
}
static int s_pop(void *h) {
    SH *x = h;
    SN *r = x->root;
    int k = r->key;
    x->root = smeld(r->l, r->r);
    free(r);
    x->n--;
    return k;
}
static int s_size(void *h) { return ((SH *)h)->n; }
static void sfree(SN *x) {
    if (x) {
        sfree(x->l);
        sfree(x->r);
        free(x);
    }
}
static void s_destroy(void *h) {
    sfree(((SH *)h)->root);
    free(h);
}

/* ---------- pairing ---------- */
typedef struct PN {
    int key;
    struct PN *child, *sib;
} PN;
typedef struct {
    PN *root;
    int n;
} PH;
static PN *pmeld(PN *a, PN *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (lt(b->key, a->key)) {
        PN *t = a;
        a = b;
        b = t;
    }
    b->sib = a->child;
    a->child = b;
    return a;
}
static void *p_create(void) { return calloc(1, sizeof(PH)); }
static void p_insert(void *h, int k) {
    PH *x = h;
    PN *n = calloc(1, sizeof(PN));
    n->key = k;
    x->root = pmeld(x->root, n);
    x->n++;
}
static void p_meld(void *a, void *b) {
    PH *x = a, *y = b;
    x->root = pmeld(x->root, y->root);
    x->n += y->n;
    free(y);
}
static int p_pop(void *h) {
    PH *x = h;
    PN *r = x->root, *c = r->child, *pairs = NULL;
    int k = r->key;
    while (c) {
        PN *a = c, *b = a->sib;
        c = b ? b->sib : NULL;
        a->sib = NULL;
        if (b)
            b->sib = NULL;
        PN *m = pmeld(a, b);
        m->sib = pairs; /* reversed list of pair winners */
        pairs = m;
    }
    PN *acc = NULL;
    while (pairs) {
        PN *nx = pairs->sib;
        pairs->sib = NULL;
        acc = pmeld(pairs, acc);
        pairs = nx;
    }
    free(r);
    x->root = acc;
    x->n--;
    return k;
}
static int p_size(void *h) { return ((PH *)h)->n; }
static void pfree(PN *x) {
    while (x) {
        PN *n = x->sib;
        pfree(x->child);
        free(x);
        x = n;
    }
}
static void p_destroy(void *h) {
    pfree(((PH *)h)->root);
    free(h);
}

/* ---------- binomial ---------- */
typedef struct BN {
    int key, deg;
    struct BN *child, *sib;
} BN;
typedef struct {
    BN *tr[24];
    int n;
} BH;
static BN *blink(BN *a, BN *b) {
    if (lt(b->key, a->key)) {
        BN *t = a;
        a = b;
        b = t;
    }
    b->sib = a->child;
    a->child = b;
    a->deg++;
    return a;
}
/* add tree t of rank k into heap, carrying */
static void badd(BH *h, BN *t) {
    int k = t->deg;
    while (h->tr[k]) {
        t = blink(h->tr[k], t);
        h->tr[k] = NULL;
        k++;
    }
    h->tr[k] = t;
}
static void *b_create(void) { return calloc(1, sizeof(BH)); }
static void b_insert(void *h, int k) {
    BN *n = calloc(1, sizeof(BN));
    n->key = k;
    badd(h, n);
    ((BH *)h)->n++;
}
static void b_meld(void *a, void *b) {
    BH *x = a, *y = b;
    for (int k = 0; k < 24; k++)
        if (y->tr[k])
            badd(x, y->tr[k]);
    x->n += y->n;
    free(y);
}
static int b_pop(void *h) {
    BH *x = h;
    int bk = -1;
    for (int k = 0; k < 24; k++)
        if (x->tr[k] && (bk < 0 || lt(x->tr[k]->key, x->tr[bk]->key)))
            bk = k;
    BN *r = x->tr[bk];
    x->tr[bk] = NULL;
    int key = r->key;
    for (BN *c = r->child; c;) {
        BN *nx = c->sib;
        c->sib = NULL;
        badd(x, c);
        c = nx;
    }
    free(r);
    x->n--;
    return key;
}
static int b_size(void *h) { return ((BH *)h)->n; }
static void bfree(BN *x) {
    while (x) {
        BN *n = x->sib;
        bfree(x->child);
        free(x);
        x = n;
    }
}
static void b_destroy(void *h) {
    for (int k = 0; k < 24; k++)
        bfree(((BH *)h)->tr[k]);
    free(h);
}

/* ---------- workloads ---------- */
enum { NH = 8 };
typedef struct {
    int kind, a, b, key;
} Step; /* 0 insert a, 1 pop a, 2 meld b into a */

static const Ops OPS[4] = {
    {"leftist", l_create, l_insert, l_meld, l_pop, l_size, l_destroy},
    {"skew", s_create, s_insert, s_meld, s_pop, s_size, s_destroy},
    {"pairing", p_create, p_insert, p_meld, p_pop, p_size, p_destroy},
    {"binomial", b_create, b_insert, b_meld, b_pop, b_size, b_destroy},
};

static unsigned long long fold(unsigned long long h, int v) { return (h ^ (unsigned)v) * 1099511628211ull; }

/* tournament: 64 heaps of 30 keys, melded pairwise, then drained */
static unsigned long long w_tournament(const Ops *o, const int *keys) {
    void *h[64];
    for (int i = 0; i < 64; i++) {
        h[i] = o->create();
        for (int j = 0; j < 30; j++)
            o->insert(h[i], keys[i * 30 + j]);
    }
    for (int w = 64; w > 1; w /= 2)
        for (int i = 0; i < w / 2; i++)
            o->meld(h[i], h[i + w / 2]);
    unsigned long long acc = 1469598103934665603ull;
    int prev = -1;
    while (o->size(h[0]) > 0) {
        int k = o->pop(h[0]);
        check(k >= prev, "tournament drain sorted");
        prev = k;
        acc = fold(acc, k);
    }
    o->destroy(h[0]);
    return acc;
}

/* chain: one big heap absorbs many small ones, with a pop after each meld */
static unsigned long long w_chain(const Ops *o, const int *keys) {
    void *big = o->create();
    unsigned long long acc = 1469598103934665603ull;
    int ki = 0;
    for (int r = 0; r < 300; r++) {
        void *s = o->create();
        for (int j = 0; j < 5; j++)
            o->insert(s, keys[ki++ % 1920]);
        o->meld(big, s);
        if (r % 3 == 2)
            acc = fold(acc, o->pop(big));
    }
    while (o->size(big) > 0)
        acc = fold(acc, o->pop(big));
    o->destroy(big);
    return acc;
}

static unsigned long long w_script(const Ops *o, const Step *st, int nsteps, int *sizes_out) {
    void *h[NH];
    for (int i = 0; i < NH; i++)
        h[i] = o->create();
    unsigned long long acc = 1469598103934665603ull;
    for (int i = 0; i < nsteps; i++) {
        const Step *s = &st[i];
        if (s->kind == 0)
            o->insert(h[s->a], s->key);
        else if (s->kind == 1)
            acc = fold(acc, o->pop(h[s->a]));
        else {
            /* meld b into a, then recreate b as empty */
            o->meld(h[s->a], h[s->b]);
            h[s->b] = o->create();
        }
    }
    for (int i = 0; i < NH; i++) {
        sizes_out[i] = o->size(h[i]);
        o->destroy(h[i]);
    }
    return acc;
}

int main(void) {
    static int keys[1920];
    for (int i = 0; i < 1920; i++)
        keys[i] = (int)(rng() % 100000);

    /* build the script with a simple model so pops are always legal */
    enum { NSTEPS = 6000 };
    static Step st[NSTEPS];
    int sz[NH] = {0}, ns = 0;
    while (ns < NSTEPS) {
        int r = (int)(rng() % 100), a = (int)(rng() % NH), b = (int)(rng() % NH);
        if (r < 55) {
            st[ns++] = (Step){0, a, 0, (int)(rng() % 50000)};
            sz[a]++;
        } else if (r < 90 && sz[a] > 0) {
            st[ns++] = (Step){1, a, 0, 0};
            sz[a]--;
        } else if (r >= 96 && a != b) {
            st[ns++] = (Step){2, a, b, 0};
            sz[a] += sz[b];
            sz[b] = 0;
        }
    }

    unsigned long long ref[3] = {0, 0, 0};
    int ref_sizes[NH];
    printf("%-9s %-12s %-12s %-12s\n", "heap", "tournament", "chain", "script");
    for (int o = 0; o < 4; o++) {
        long c[3];
        unsigned long long d[3];
        cmps = 0;
        d[0] = w_tournament(&OPS[o], keys);
        c[0] = cmps;
        cmps = 0;
        d[1] = w_chain(&OPS[o], keys);
        c[1] = cmps;
        cmps = 0;
        int sizes[NH];
        d[2] = w_script(&OPS[o], st, ns, sizes);
        c[2] = cmps;
        if (o == 0) {
            for (int i = 0; i < 3; i++)
                ref[i] = d[i];
            memcpy(ref_sizes, sizes, sizeof sizes);
        } else {
            for (int i = 0; i < 3; i++)
                check(d[i] == ref[i], "all heaps produce identical pop streams");
            check(memcmp(sizes, ref_sizes, sizeof sizes) == 0, "identical final sizes");
        }
        for (int i = 0; i < NH; i++)
            check(sizes[i] == sz[i], "final sizes equal the script model");
        printf("%-9s %-12ld %-12ld %-12ld\n", OPS[o].name, c[0], c[1], c[2]);
    }
    printf("pop stream hashes: %llu %llu %llu\n", ref[0], ref[1], ref[2]);
    printf("final sizes:");
    for (int i = 0; i < NH; i++)
        printf(" %d", sz[i]);
    printf("\n");
    return 0;
}
