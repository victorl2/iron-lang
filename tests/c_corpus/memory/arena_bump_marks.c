/*
 * title: Bump arena with marks, rollback and reset
 * topic: memory
 * covers: bump allocator, alignment padding, marks, rollback, reset, peak usage, fill-pattern validation
 * deps: libc
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x243F6A8885A308D3ULL;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}

#define CAP 8192
static _Alignas(16) unsigned char backing[CAP];

typedef struct {
    unsigned char *base;
    size_t cap, used, peak, waste, fails, allocs;
} Arena;

static void arena_init(Arena *a, unsigned char *buf, size_t cap) {
    memset(a, 0, sizeof *a);
    a->base = buf;
    a->cap = cap;
}

static void *arena_alloc(Arena *a, size_t n, size_t align) {
    CHECK(align && (align & (align - 1)) == 0);
    size_t start = (a->used + align - 1) & ~(align - 1);
    if (start + n > a->cap) {
        a->fails++;
        return NULL;
    }
    a->waste += start - a->used;
    a->used = start + n;
    if (a->used > a->peak) a->peak = a->used;
    a->allocs++;
    return a->base + start;
}

static size_t arena_mark(const Arena *a) { return a->used; }

static void arena_rollback(Arena *a, size_t mark) {
    CHECK(mark <= a->used);
    memset(a->base + mark, 0xDD, a->used - mark); /* poison released space */
    a->used = mark;
}

typedef struct { unsigned char *p; size_t n; unsigned tag; } Rec;

static void fill(unsigned char *p, size_t n, unsigned tag) {
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag * 37u + i * 11u + 5u);
}
static int verify(const Rec *r) {
    for (size_t i = 0; i < r->n; i++)
        if (r->p[i] != (unsigned char)(r->tag * 37u + i * 11u + 5u)) return 0;
    return 1;
}

int main(void) {
    Arena a;
    arena_init(&a, backing, CAP);
    Rec live[512];
    int nlive = 0;
    size_t marks[16];
    int mark_live[16];
    int nmarks = 0;
    unsigned tag = 1;
    unsigned rollbacks = 0, resets = 0;
    size_t total_req = 0;

    for (int step = 0; step < 4000; step++) {
        unsigned op = rnd() % 100;
        if (op < 70) {
            size_t n = 1 + rnd() % 120;
            size_t align = (size_t)1 << (rnd() % 5);
            unsigned char *p = arena_alloc(&a, n, align);
            if (p) {
                CHECK(((size_t)(p - backing) & (align - 1)) == 0);
                fill(p, n, tag);
                live[nlive].p = p; live[nlive].n = n; live[nlive].tag = tag++;
                nlive++;
                total_req += n;
                CHECK(nlive < 512);
            }
        } else if (op < 82) {
            if (nmarks < 16) {
                marks[nmarks] = arena_mark(&a);
                mark_live[nmarks] = nlive;
                nmarks++;
            }
        } else if (op < 95) {
            if (nmarks > 0) {
                nmarks--;
                arena_rollback(&a, marks[nmarks]);
                nlive = mark_live[nmarks];
                rollbacks++;
            }
        } else {
            arena_rollback(&a, 0);
            nlive = 0; nmarks = 0;
            resets++;
        }
        if (step % 50 == 0) {
            for (int i = 0; i < nlive; i++) CHECK(verify(&live[i]));
            /* live blocks are disjoint and ascending */
            for (int i = 1; i < nlive; i++) CHECK(live[i - 1].p + live[i - 1].n <= live[i].p);
            if (nlive) CHECK((size_t)(live[nlive - 1].p + live[nlive - 1].n - backing) <= a.used);
        }
    }
    for (int i = 0; i < nlive; i++) CHECK(verify(&live[i]));

    printf("allocs=%zu fails=%zu\n", a.allocs, a.fails);
    printf("rollbacks=%u resets=%u\n", rollbacks, resets);
    printf("peak=%zu used=%zu live=%d\n", a.peak, a.used, nlive);
    printf("alignment waste=%zu requested=%zu\n", a.waste, total_req);

    /* second phase: exact-fit test of capacity */
    arena_rollback(&a, 0);
    size_t got = 0;
    while (arena_alloc(&a, 100, 4)) got++;
    printf("100-byte blocks at align 4: %zu (used %zu of %d)\n", got, a.used, CAP);
    CHECK(got == CAP / 100);
    arena_rollback(&a, 0);
    CHECK(arena_alloc(&a, CAP, 1) != NULL);
    CHECK(arena_alloc(&a, 1, 1) == NULL);
    printf("full-arena block ok, extra alloc refused\n");
    return 0;
}
