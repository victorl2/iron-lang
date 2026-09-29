/*
 * title: Object cache with constructors, destructors and reuse of constructed state
 * topic: memory
 * covers: object cache, ctor/dtor callbacks, constructed-state preservation across free, cache reap, function pointers, two cache types
 * deps: libc
 */
#define SEED 0x0B1EC7CAC4EULL
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = SEED;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}
static void pat_fill(void *vp, size_t n, unsigned tag) {
    unsigned char *p = vp;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u);
}
static int pat_ok(const void *vp, size_t n, unsigned tag) {
    const unsigned char *p = vp;
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u)) return 0;
    return 1;
}

typedef void (*CtorFn)(void *obj);
typedef void (*DtorFn)(void *obj);

typedef struct {
    const char *name;
    size_t objsize;
    CtorFn ctor;
    DtorFn dtor;
    unsigned char *mem;
    size_t nslots;
    int32_t *free_stack;   /* indices of constructed, idle objects */
    int nfree_constructed;
    int32_t *virgin;       /* indices never constructed yet */
    int nvirgin;
    unsigned char *state;  /* 0 virgin, 1 idle constructed, 2 in use */
    unsigned ctor_calls, dtor_calls, allocs, frees, reuse_hits;
} Cache;

/* object 1: a node whose constructor establishes invariants that must survive free/alloc */
typedef struct {
    uint32_t magic;
    uint32_t links[2];  /* constructed as empty (0xFFFFFFFF) */
    uint32_t user[9];
} Node;

#define NODE_MAGIC 0xC0DEC0DEu
#define EMPTY_LINK 0xFFFFFFFFu

static void node_ctor(void *o) {
    Node *n = o;
    memset(n, 0, sizeof *n);
    n->magic = NODE_MAGIC;
    n->links[0] = n->links[1] = EMPTY_LINK;
}
static void node_dtor(void *o) {
    Node *n = o;
    CHECK(n->magic == NODE_MAGIC);
    CHECK(n->links[0] == EMPTY_LINK && n->links[1] == EMPTY_LINK);
    n->magic = 0xDEADDEADu;
}

/* object 2: a buffer with a checksum field; ctor zeroes, dtor verifies it was left zeroed by its user */
typedef struct {
    unsigned char guard[4];
    unsigned char data[28];
} Buf;
static void buf_ctor(void *o) { Buf *b = o; memset(b, 0, sizeof *b); memcpy(b->guard, "BUF!", 4); }
static void buf_dtor(void *o) { Buf *b = o; CHECK(memcmp(b->guard, "BUF!", 4) == 0); memset(b, 0xFF, sizeof *b); }

static void cache_init(Cache *c, const char *name, size_t objsize, size_t nslots, CtorFn ctor, DtorFn dtor) {
    memset(c, 0, sizeof *c);
    c->name = name; c->objsize = objsize; c->nslots = nslots; c->ctor = ctor; c->dtor = dtor;
    c->mem = calloc(nslots, objsize);
    c->free_stack = malloc(nslots * sizeof(int32_t));
    c->virgin = malloc(nslots * sizeof(int32_t));
    c->state = calloc(nslots, 1);
    CHECK(c->mem && c->free_stack && c->virgin && c->state);
    for (size_t i = 0; i < nslots; i++) c->virgin[i] = (int32_t)(nslots - 1 - i);
    c->nvirgin = (int)nslots;
}

static void cache_destroy(Cache *c) {
    for (size_t i = 0; i < c->nslots; i++) CHECK(c->state[i] != 2);
    free(c->mem); free(c->free_stack); free(c->virgin); free(c->state);
}

static void *cache_alloc(Cache *c) {
    int32_t i;
    if (c->nfree_constructed > 0) {
        i = c->free_stack[--c->nfree_constructed];
        c->reuse_hits++;
    } else if (c->nvirgin > 0) {
        i = c->virgin[--c->nvirgin];
        c->ctor(c->mem + (size_t)i * c->objsize);
        c->ctor_calls++;
    } else return NULL;
    c->state[i] = 2;
    c->allocs++;
    return c->mem + (size_t)i * c->objsize;
}

static void cache_free(Cache *c, void *p) {
    size_t i = (size_t)((unsigned char *)p - c->mem) / c->objsize;
    CHECK(i < c->nslots && c->state[i] == 2);
    c->state[i] = 1;
    c->free_stack[c->nfree_constructed++] = (int32_t)i;
    c->frees++;
}

/* reap: destruct all idle constructed objects, returning them to the virgin pool */
static int cache_reap(Cache *c) {
    int n = 0;
    while (c->nfree_constructed > 0) {
        int32_t i = c->free_stack[--c->nfree_constructed];
        c->dtor(c->mem + (size_t)i * c->objsize);
        c->dtor_calls++;
        c->state[i] = 0;
        c->virgin[c->nvirgin++] = i;
        n++;
    }
    return n;
}

int main(void) {
    Cache nodes, bufs;
    cache_init(&nodes, "node", sizeof(Node), 40, node_ctor, node_dtor);
    cache_init(&bufs, "buf", sizeof(Buf), 24, buf_ctor, buf_dtor);
    void *live_n[40], *live_b[24];
    unsigned tag_n[40], tag_b[24];
    int nn = 0, nb = 0, reaped = 0, refused = 0;
    unsigned tag = 1;

    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        if (op < 30 && nn < 40) {
            Node *n = cache_alloc(&nodes);
            if (!n) { refused++; continue; }
            CHECK(n->magic == NODE_MAGIC && n->links[0] == EMPTY_LINK && n->links[1] == EMPTY_LINK);
            pat_fill(n->user, sizeof n->user, tag);
            live_n[nn] = n; tag_n[nn++] = tag++;
        } else if (op < 55 && nb < 24) {
            Buf *b = cache_alloc(&bufs);
            if (!b) { refused++; continue; }
            CHECK(memcmp(b->guard, "BUF!", 4) == 0);
            pat_fill(b->data, sizeof b->data, tag);
            live_b[nb] = b; tag_b[nb++] = tag++;
        } else if (op < 75 && nn > 0) {
            int i = (int)(rnd() % (unsigned)nn);
            Node *n = live_n[i];
            CHECK(pat_ok(n->user, sizeof n->user, tag_n[i]));
            memset(n->user, 0, sizeof n->user);  /* user restores invariants before free; user area is free to change */
            cache_free(&nodes, n);
            live_n[i] = live_n[--nn]; tag_n[i] = tag_n[nn];
        } else if (op < 95 && nb > 0) {
            int i = (int)(rnd() % (unsigned)nb);
            Buf *b = live_b[i];
            CHECK(pat_ok(b->data, sizeof b->data, tag_b[i]));
            cache_free(&bufs, b);
            live_b[i] = live_b[--nb]; tag_b[i] = tag_b[nb];
        } else if (op >= 98) {
            reaped += cache_reap(&nodes);
            reaped += cache_reap(&bufs);
        }
        if (step % 200 == 0) {
            for (int i = 0; i < nn; i++) CHECK(pat_ok(((Node *)live_n[i])->user, sizeof(((Node *)0)->user), tag_n[i]));
            for (int i = 0; i < nb; i++) CHECK(pat_ok(((Buf *)live_b[i])->data, sizeof(((Buf *)0)->data), tag_b[i]));
        }
    }
    while (nn > 0) { Node *n = live_n[--nn]; CHECK(pat_ok(n->user, sizeof n->user, tag_n[nn])); memset(n->user, 0, sizeof n->user); cache_free(&nodes, n); }
    while (nb > 0) cache_free(&bufs, live_b[--nb]);
    reaped += cache_reap(&nodes);
    reaped += cache_reap(&bufs);
    Cache *cs[2] = { &nodes, &bufs };
    for (int k = 0; k < 2; k++) {
        Cache *c = cs[k];
        CHECK(c->ctor_calls == c->dtor_calls);
        CHECK(c->allocs == c->frees);
        CHECK(c->allocs == c->ctor_calls + c->reuse_hits);
        printf("%-4s objsize=%2zu allocs=%u ctor=%u reuse=%u dtor=%u\n", c->name, c->objsize, c->allocs, c->ctor_calls, c->reuse_hits, c->dtor_calls);
    }
    printf("reaped objects=%d refused=%d\n", reaped, refused);
    cache_destroy(&nodes);
    cache_destroy(&bufs);
    return 0;
}
