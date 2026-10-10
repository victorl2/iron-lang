/*
 * title: Allocator interface with swappable implementations and decorators
 * topic: memory
 * covers: vtable of function pointers, bump/pool/size-class implementations, fallback composition, tracking decorator with canaries, identical workload replayed per allocator
 * deps: libc
 */
#define SEED 0xA110C7AB1EULL
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

typedef struct Allocator Allocator;
struct Allocator {
    const char *name;
    void *(*alloc)(Allocator *, size_t);
    void (*release)(Allocator *, void *);
    void (*reset)(Allocator *);          /* NULL if unsupported */
    size_t (*footprint)(Allocator *);    /* bytes of backing consumed at high-water mark */
    void *ctx;
};

/* ---------- bump ---------- */
typedef struct { unsigned char *mem; size_t cap, used, peak; } Bump;
static void *bump_alloc(Allocator *a, size_t n) {
    Bump *b = a->ctx;
    size_t s = (b->used + 15u) & ~(size_t)15;
    if (s + n > b->cap) return NULL;
    b->used = s + n;
    if (b->used > b->peak) b->peak = b->used;
    return b->mem + s;
}
static void bump_release(Allocator *a, void *p) { (void)a; (void)p; }
static void bump_reset(Allocator *a) { Bump *b = a->ctx; b->used = 0; }
static size_t bump_foot(Allocator *a) { return ((Bump *)a->ctx)->peak; }

/* ---------- fixed pool of 64-byte slots ---------- */
#define PSLOT 64
#define PN 40
typedef struct { unsigned char mem[PSLOT * PN]; int free_stack[PN]; int nfree, min_free; } Pool;
static void pool_reset(Allocator *a) {
    Pool *p = a->ctx;
    for (int i = 0; i < PN; i++) p->free_stack[i] = PN - 1 - i;
    p->nfree = PN;
}
static void *pool_alloc(Allocator *a, size_t n) {
    Pool *p = a->ctx;
    if (n > PSLOT || p->nfree == 0) return NULL;
    int i = p->free_stack[--p->nfree];
    if (p->nfree < p->min_free) p->min_free = p->nfree;
    return p->mem + (size_t)i * PSLOT;
}
static void pool_release(Allocator *a, void *ptr) {
    Pool *p = a->ctx;
    size_t off = (size_t)((unsigned char *)ptr - p->mem);
    CHECK(off < sizeof p->mem && off % PSLOT == 0);
    p->free_stack[p->nfree++] = (int)(off / PSLOT);
}
static size_t pool_foot(Allocator *a) { Pool *p = a->ctx; return (size_t)(PN - p->min_free) * PSLOT; }

/* ---------- power-of-two size-class recycler over a bump region ---------- */
#define NCLS 6   /* 16 .. 512 payload */
typedef struct { unsigned char *mem; size_t cap, used, peak; uint32_t head[NCLS]; } Pow2;
#define P2NIL 0xFFFFFFFFu
static void pow2_reset(Allocator *a) {
    Pow2 *p = a->ctx;
    p->used = 0;
    for (int c = 0; c < NCLS; c++) p->head[c] = P2NIL;
}
static void *pow2_alloc(Allocator *a, size_t n) {
    Pow2 *p = a->ctx;
    int c = 0;
    while (((size_t)16 << c) < n) { c++; if (c == NCLS) return NULL; }
    if (p->head[c] != P2NIL) {
        uint32_t off = p->head[c], nx;
        memcpy(&nx, p->mem + off, 4);
        p->head[c] = nx;
        return p->mem + off;
    }
    size_t need = 16 + ((size_t)16 << c);
    if (p->used + need > p->cap) return NULL;
    unsigned char *blk = p->mem + p->used;
    p->used += need;
    if (p->used > p->peak) p->peak = p->used;
    uint32_t cc = (uint32_t)c;
    memcpy(blk, &cc, 4);           /* class header, payload starts 16 bytes in */
    return blk + 16;
}
static void pow2_release(Allocator *a, void *ptr) {
    Pow2 *p = a->ctx;
    unsigned char *pl = ptr;
    uint32_t c;
    memcpy(&c, pl - 16, 4);
    CHECK(c < NCLS);
    uint32_t off = (uint32_t)(pl - p->mem);
    memcpy(pl, &p->head[c], 4);
    p->head[c] = off;
}
static size_t pow2_foot(Allocator *a) { return ((Pow2 *)a->ctx)->peak; }

/* ---------- fallback: try A, then B; release routes by address ownership ---------- */
typedef struct { Allocator *first, *second; const unsigned char *lo, *hi; } Fallback;
static void *fb_alloc(Allocator *a, size_t n) {
    Fallback *f = a->ctx;
    void *p = f->first->alloc(f->first, n);
    return p ? p : f->second->alloc(f->second, n);
}
static void fb_release(Allocator *a, void *ptr) {
    Fallback *f = a->ctx;
    uintptr_t u = (uintptr_t)ptr;
    if (u >= (uintptr_t)f->lo && u < (uintptr_t)f->hi) f->first->release(f->first, ptr);
    else f->second->release(f->second, ptr);
}
static void fb_reset(Allocator *a) { Fallback *f = a->ctx; f->first->reset(f->first); f->second->reset(f->second); }
static size_t fb_foot(Allocator *a) { Fallback *f = a->ctx; return f->first->footprint(f->first) + f->second->footprint(f->second); }

/* ---------- decorator: header and trailer canaries, byte accounting ---------- */
typedef struct { Allocator *inner; size_t live_bytes, peak_bytes, live_blocks, total_blocks; } Tracker;
#define CAN 0xC4A9EA55u
static void *tr_alloc(Allocator *a, size_t n) {
    Tracker *t = a->ctx;
    unsigned char *raw = t->inner->alloc(t->inner, n + 16 + 4);
    if (!raw) return NULL;
    uint32_t sz = (uint32_t)n, can = CAN;
    memcpy(raw, &sz, 4); memcpy(raw + 4, &can, 4);
    memcpy(raw + 16 + n, &can, 4);
    t->live_bytes += n; t->live_blocks++; t->total_blocks++;
    if (t->live_bytes > t->peak_bytes) t->peak_bytes = t->live_bytes;
    return raw + 16;
}
static void tr_release(Allocator *a, void *ptr) {
    Tracker *t = a->ctx;
    unsigned char *raw = (unsigned char *)ptr - 16;
    uint32_t sz, c1, c2;
    memcpy(&sz, raw, 4); memcpy(&c1, raw + 4, 4); memcpy(&c2, raw + 16 + sz, 4);
    CHECK(c1 == CAN && c2 == CAN);
    uint32_t dead = 0xDEADBEEFu;
    memcpy(raw + 4, &dead, 4);
    t->live_bytes -= sz; t->live_blocks--;
    t->inner->release(t->inner, raw);
}
static void tr_reset(Allocator *a) { Tracker *t = a->ctx; t->live_bytes = 0; t->live_blocks = 0; t->inner->reset(t->inner); }
static size_t tr_foot(Allocator *a) { Tracker *t = a->ctx; return t->inner->footprint(t->inner); }

/* ---------- workload ---------- */
typedef struct { int is_alloc; unsigned size; int target; } Op;
#define NOPS 8000
static Op ops[NOPS];
typedef struct { void *p; unsigned n; unsigned tag; int ok; } Slot;
static Slot slots[NOPS];

static void gen_trace(void) {
    int live[200], nl = 0;
    for (int i = 0; i < NOPS; i++) {
        if (nl == 0 || (rnd() % 100 < 52 && nl < 200)) {
            unsigned k = rnd() % 10;
            ops[i].is_alloc = 1; ops[i].size = k < 7 ? 1 + rnd() % 60 : k < 9 ? 61 + rnd() % 140 : 201 + rnd() % 300;
            ops[i].target = -1;
            live[nl++] = i;
        } else {
            int j = (int)(rnd() % (unsigned)nl);
            ops[i].is_alloc = 0; ops[i].target = live[j];
            live[j] = live[--nl];
        }
    }
}

static void replay(Allocator *a) {
    memset(slots, 0, sizeof slots);
    a->reset(a);
    unsigned ok = 0, refused = 0, epochs = 0;
    size_t live_bytes = 0, peak_live = 0;
    for (int i = 0; i < NOPS; i++) {
        if (ops[i].is_alloc) {
            void *p = a->alloc(a, ops[i].size);
            if (!p) refused++;
            else {
                slots[i].p = p; slots[i].n = ops[i].size; slots[i].tag = (unsigned)i + 1; slots[i].ok = 1;
                pat_fill(p, slots[i].n, slots[i].tag);
                ok++; live_bytes += slots[i].n;
                if (live_bytes > peak_live) peak_live = live_bytes;
            }
        } else if (slots[ops[i].target].ok) {
            Slot *s = &slots[ops[i].target];
            CHECK(pat_ok(s->p, s->n, s->tag));
            a->release(a, s->p);
            live_bytes -= s->n; s->ok = 0;
        }
        if ((i + 1) % 1000 == 0) {            /* epoch end: everything still live is verified, then dropped */
            for (int j = 0; j <= i; j++) if (slots[j].ok) { CHECK(pat_ok(slots[j].p, slots[j].n, slots[j].tag)); slots[j].ok = 0; }
            live_bytes = 0;
            a->reset(a);
            epochs++;
        }
    }
    printf("%-20s ok=%4u refused=%4u peak live=%5zu footprint=%5zu epochs=%u\n", a->name, ok, refused, peak_live, a->footprint(a), epochs);
}

int main(void) {
    static _Alignas(16) unsigned char bump_mem[16384], pow2_mem[6144];
    static Pool pool;
    Bump bump = { bump_mem, sizeof bump_mem, 0, 0 };
    Pow2 p2 = { pow2_mem, sizeof pow2_mem, 0, 0, {0} };
    pool.min_free = PN;
    Allocator a_bump = { "bump", bump_alloc, bump_release, bump_reset, bump_foot, &bump };
    Allocator a_pool = { "pool64", pool_alloc, pool_release, pool_reset, pool_foot, &pool };
    Allocator a_pow2 = { "pow2-recycler", pow2_alloc, pow2_release, pow2_reset, pow2_foot, &p2 };
    Fallback fbk = { &a_pool, &a_pow2, pool.mem, pool.mem + sizeof pool.mem };
    Allocator a_fb = { "pool64+pow2", fb_alloc, fb_release, fb_reset, fb_foot, &fbk };
    static _Alignas(16) unsigned char pow2b_mem[8192];
    Pow2 p2b = { pow2b_mem, sizeof pow2b_mem, 0, 0, {0} };
    Allocator a_pow2b = { "pow2-inner", pow2_alloc, pow2_release, pow2_reset, pow2_foot, &p2b };
    Tracker trk = { &a_pow2b, 0, 0, 0, 0 };
    Allocator a_trk = { "tracked(pow2)", tr_alloc, tr_release, tr_reset, tr_foot, &trk };

    gen_trace();
    int nalloc = 0;
    for (int i = 0; i < NOPS; i++) nalloc += ops[i].is_alloc;
    printf("workload: %d ops (%d allocs), 8 epochs\n", NOPS, nalloc);
    Allocator *all[] = { &a_bump, &a_pool, &a_pow2, &a_fb, &a_trk };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) replay(all[i]);
    printf("tracker: blocks handed out=%zu peak requested bytes=%zu live at end=%zu\n", trk.total_blocks, trk.peak_bytes, trk.live_blocks);
    CHECK(trk.live_blocks == 0);
    return 0;
}
