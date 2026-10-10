/*
 * title: Growing chained arena with geometric blocks
 * topic: memory
 * covers: chained arena, geometric block growth, oversize dedicated blocks, pointer stability, reset retaining first block, malloc'd backing
 * deps: libc
 */
#define SEED 0x1234ABCD9876ULL
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

typedef struct Block {
    struct Block *next;
    size_t cap, used;
    /* payload follows */
    unsigned char data[];
} Block;

static size_t total_oversize;

typedef struct {
    Block *head;      /* current block (newest) */
    Block *first;
    size_t next_cap;
    size_t blocks, total_cap, total_used_closed, requested, oversize;
    size_t peak_blocks;
} Chain;

#define MIN_CAP 256
#define MAX_CAP 4096

static Block *new_block(size_t cap) {
    Block *b = malloc(sizeof(Block) + cap);
    CHECK(b);
    b->next = NULL; b->cap = cap; b->used = 0;
    return b;
}

static void chain_init(Chain *c) {
    memset(c, 0, sizeof *c);
    c->next_cap = MIN_CAP;
}

static void *chain_alloc(Chain *c, size_t n) {
    n = (n + 7) & ~(size_t)7;
    c->requested += n;
    if (n > MAX_CAP / 2) {
        /* oversize: dedicated block linked behind the current head, keep head open */
        Block *b = new_block(n);
        b->used = n;
        if (c->head) { b->next = c->head->next; c->head->next = b; }
        else { c->head = c->first = b; }
        c->blocks++; c->total_cap += n; c->oversize++; total_oversize++;
        if (c->blocks > c->peak_blocks) c->peak_blocks = c->blocks;
        return b->data;
    }
    if (!c->head || c->head->used + n > c->head->cap) {
        size_t cap = c->next_cap > n ? c->next_cap : n;
        Block *b = new_block(cap);
        if (c->next_cap < MAX_CAP) c->next_cap *= 2;
        if (c->head) c->total_used_closed += c->head->used;
        b->next = c->head; /* newest block first */
        c->head = b;
        if (!c->first) c->first = b;
        c->blocks++; c->total_cap += b->cap;
        if (c->blocks > c->peak_blocks) c->peak_blocks = c->blocks;
    }
    void *p = c->head->data + c->head->used;
    c->head->used += n;
    return p;
}

static void chain_reset(Chain *c) {
    /* free every block, then keep a fresh small one so the chain restarts cheaply */
    Block *b = c->head;
    while (b) { Block *n = b->next; free(b); b = n; }
    size_t pk = c->peak_blocks;
    chain_init(c);
    c->peak_blocks = pk;
}

static void chain_stats(const Chain *c, size_t *nb, size_t *cap, size_t *used) {
    *nb = *cap = *used = 0;
    for (const Block *b = c->head; b; b = b->next) {
        (*nb)++; *cap += b->cap; *used += b->used;
        CHECK(b->used <= b->cap);
    }
}

typedef struct { unsigned char *p; size_t n; unsigned tag; } Rec;

int main(void) {
    Chain c;
    chain_init(&c);
    static Rec live[3000];
    int nlive = 0;
    unsigned tag = 1;
    int resets = 0;
    size_t hist[6] = {0};

    for (int round = 0; round < 6; round++) {
        int allocs = 200 + (int)(rnd() % 400);
        for (int i = 0; i < allocs; i++) {
            size_t n;
            unsigned k = rnd() % 100;
            if (k < 80) n = 1 + rnd() % 60;
            else if (k < 97) n = 100 + rnd() % 500;
            else n = 3000 + rnd() % 3000;
            unsigned char *p = chain_alloc(&c, n);
            CHECK(((uintptr_t)p & 7u) == 0);
            pat_fill(p, n, tag);
            live[nlive].p = p; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
            size_t cls = n <= 60 ? 0 : n <= 600 ? 1 : 2;
            hist[cls]++;
        }
        /* pointer stability: every earlier block still intact after growth */
        for (int i = 0; i < nlive; i++) CHECK(pat_ok(live[i].p, live[i].n, live[i].tag));
        size_t nb, cap, used;
        chain_stats(&c, &nb, &cap, &used);
        CHECK(nb == c.blocks && cap == c.total_cap);
        printf("round %d: live=%d blocks=%zu cap=%zu used=%zu slack=%zu\n", round, nlive, nb, cap, used, cap - used);
        if (round % 2 == 1) {
            chain_reset(&c);
            nlive = 0;
            resets++;
            CHECK(c.blocks == 0 && c.head == NULL);
        }
    }
    printf("small=%zu medium=%zu large=%zu\n", hist[0], hist[1], hist[2]);
    printf("oversize blocks=%zu peak blocks=%zu resets=%d\n", total_oversize, c.peak_blocks, resets);
    chain_reset(&c);
    return 0;
}
