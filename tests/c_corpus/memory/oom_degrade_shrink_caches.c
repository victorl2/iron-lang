/*
 * title: Graceful out-of-memory degradation by shrinking caches
 * topic: memory
 * covers: memory pressure, cache eviction on allocation failure, retry loop, priority of reclaimers, no data loss for pinned state
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* heap with a hard cap */
static size_t cap = 6000, used;
typedef struct { size_t n; } H;
static void *h_alloc(size_t n) {
    if (used + n > cap) return NULL;
    H *h = malloc(sizeof(H) + n);
    if (!h) return NULL;
    h->n = n; used += n;
    return h + 1;
}
static void h_free(void *p) { if (p) { H *h = (H *)p - 1; used -= h->n; free(h); } }

/* a cache of entries; the oldest ones are dropped first; each entry is 100..300 bytes */
typedef struct Ent { int id; size_t n; struct Ent *next; unsigned char *data; } Ent;
typedef struct { const char *name; Ent *head, *tail; int count; size_t bytes; int reclaimed; } Cache;

static int cache_add(Cache *c, int id, size_t n) {
    Ent *e = h_alloc(sizeof *e);
    if (!e) return 0;
    e->data = h_alloc(n);
    if (!e->data) { h_free(e); return 0; }
    memset(e->data, id & 0xFF, n);
    e->id = id; e->n = n; e->next = NULL;
    if (c->tail) c->tail->next = e; else c->head = e;
    c->tail = e; c->count++; c->bytes += n;
    return 1;
}

/* drop up to `want` bytes from the head (oldest); return bytes freed */
static size_t cache_shrink(Cache *c, size_t want) {
    size_t freed = 0;
    while (c->head && freed < want) {
        Ent *e = c->head;
        c->head = e->next;
        if (!c->head) c->tail = NULL;
        freed += e->n + sizeof(Ent);
        c->bytes -= e->n; c->count--; c->reclaimed++;
        h_free(e->data); h_free(e);
    }
    return freed;
}

static Cache caches[3] = {{"parse", 0, 0, 0, 0, 0}, {"glyph", 0, 0, 0, 0, 0}, {"route", 0, 0, 0, 0, 0}};
static const int keep_min[3] = {0, 2, 4}; /* how many entries each cache resists shedding (importance) */
static int retries, hard_fail;

/* Allocation with degradation: on failure, shrink the least important non-empty cache, retry. */
static void *alloc_or_degrade(size_t n) {
    for (;;) {
        void *p = h_alloc(n);
        if (p) return p;
        int victim = -1;
        for (int i = 0; i < 3; i++)
            if (caches[i].count > keep_min[i] && (victim < 0 || keep_min[i] < keep_min[victim] || caches[i].count > caches[victim].count))
                if (victim < 0 || keep_min[i] <= keep_min[victim]) victim = i;
        if (victim < 0) { hard_fail++; return NULL; }
        cache_shrink(&caches[victim], 1);
        retries++;
    }
}

/* the critical state that must never be lost: a 1 KB working buffer and its checksum */
static unsigned sum(const unsigned char *p, size_t n) { unsigned s = 0; for (size_t i = 0; i < n; i++) s = s * 31 + p[i]; return s; }

int main(void) {
    unsigned char *work = h_alloc(1024);
    if (!work) return 1;
    for (int i = 0; i < 1024; i++) work[i] = (unsigned char)(i * 5);
    unsigned want = sum(work, 1024);

    unsigned r = 7;
    int id = 0;
    for (int step = 0; step < 300; step++) {
        r = r * 1664525u + 1013904223u;
        int ci = (int)((r >> 20) % 3);
        size_t n = 100 + (r >> 8) % 200;
        /* the cache needs two blocks; obtain them with degradation, then hand them over */
        void *e = alloc_or_degrade(sizeof(Ent));
        void *d = e ? alloc_or_degrade(n) : NULL;
        if (!e || !d) { h_free(e); h_free(d); continue; }
        Ent *ent = e;
        ent->data = d; ent->id = ++id; ent->n = n; ent->next = NULL;
        memset(d, id & 0xFF, n);
        Cache *c = &caches[ci];
        if (c->tail) c->tail->next = ent; else c->head = ent;
        c->tail = ent; c->count++; c->bytes += n;
        if (used > cap) return 1;
    }
    printf("cap=%zu used=%zu retries=%d hard_fail=%d\n", cap, used, retries, hard_fail);
    for (int i = 0; i < 3; i++)
        printf("  %-6s entries=%3d bytes=%5zu reclaimed=%d\n", caches[i].name, caches[i].count, caches[i].bytes, caches[i].reclaimed);
    if (sum(work, 1024) != want) return 1;
    printf("critical buffer intact: %08x\n", want);

    /* now demand something bigger than caches can provide */
    void *huge = alloc_or_degrade(5900);
    printf("5900-byte request: %s, used=%zu, cache entries left=%d/%d/%d\n", huge ? "served" : "refused", used,
           caches[0].count, caches[1].count, caches[2].count);
    h_free(huge);
    /* a request larger than the cap can never succeed and must terminate */
    void *impossible = alloc_or_degrade(7000);
    printf("7000-byte request: %s hard_fail=%d\n", impossible ? "served" : "refused", hard_fail);

    for (int i = 0; i < 3; i++) cache_shrink(&caches[i], (size_t)-1);
    (void)cache_add;
    h_free(work);
    printf("final used=%zu\n", used);
    return used == 0 ? 0 : 1;
}
