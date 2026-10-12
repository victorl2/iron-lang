/*
 * title: Handle-based compacting heap with pinning
 * topic: memory
 * covers: handles instead of pointers, sliding compaction, handle table update, pinned blocks as obstacles, dead-space accounting, fragmentation before and after
 * deps: libc
 */
#define SEED 0x4A4D1E5ULL
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

#define HEAP 8192u
#define NH 256
#define DEAD 0xFFFFu
#define HDR 8u

static _Alignas(16) unsigned char heap[HEAP];
static unsigned top;

typedef struct { unsigned off; unsigned size; int used; int pin; } Entry;
static Entry tab[NH];
static unsigned dead_bytes, live_bytes;
static unsigned long compactions, moved_bytes, moved_blocks, pinned_stops, alloc_ok, alloc_fail;

/* block: [u16 handle or DEAD][u16 pad][u32 total size] payload ... ; total size includes header, 8-aligned */
static void put_hdr(unsigned off, unsigned h, unsigned total) {
    uint16_t hh = (uint16_t)h, z = 0;
    memcpy(heap + off, &hh, 2); memcpy(heap + off + 2, &z, 2); memcpy(heap + off + 4, &total, 4);
}
static void get_hdr(unsigned off, unsigned *h, unsigned *total) {
    uint16_t hh; memcpy(&hh, heap + off, 2); memcpy(total, heap + off + 4, 4); *h = hh;
}

static void *deref(int h) { CHECK(tab[h].used); return heap + tab[h].off + HDR; }

static void compact(void) {
    compactions++;
    /* pinned blocks split the heap into regions; movable blocks are packed into the earliest region with room */
    unsigned rstart[16], rend[16], fill[16];
    int nr = 0;
    unsigned start = 0, p = 0;
    while (p < top) {
        unsigned h, t;
        get_hdr(p, &h, &t);
        if (h != DEAD && tab[h].pin) {
            rstart[nr] = start; rend[nr] = p; fill[nr] = start; nr++;
            start = p + t;
            pinned_stops++;
        }
        p += t;
    }
    rstart[nr] = start; rend[nr] = top; fill[nr] = start; nr++;
    unsigned src = 0;
    while (src < top) {
        unsigned h, total;
        get_hdr(src, &h, &total);
        unsigned next = src + total;
        if (h != DEAD && !tab[h].pin) {
            int r = 0;
            while (fill[r] + total > rend[r]) r++;
            CHECK(r < nr && fill[r] >= rstart[r] && fill[r] <= src);
            if (fill[r] != src) { memmove(heap + fill[r], heap + src, total); tab[h].off = fill[r]; moved_bytes += total; moved_blocks++; }
            fill[r] += total;
        }
        src = next;
    }
    for (int r = 0; r + 1 < nr; r++) if (fill[r] < rend[r]) put_hdr(fill[r], DEAD, rend[r] - fill[r]);
    top = fill[nr - 1];
    dead_bytes = 0;
    p = 0;
    while (p < top) { unsigned h, t; get_hdr(p, &h, &t); if (h == DEAD) dead_bytes += t; p += t; }
}

static int h_alloc(unsigned n) {
    unsigned total = (n + HDR + 7) & ~7u;
    if (top + total > HEAP) {
        if (dead_bytes == 0) { alloc_fail++; return -1; }
        compact();
        if (top + total > HEAP) { alloc_fail++; return -1; }
    }
    int h = -1;
    for (int i = 0; i < NH; i++) if (!tab[i].used) { h = i; break; }
    if (h < 0) { alloc_fail++; return -1; }
    put_hdr(top, (unsigned)h, total);
    tab[h].off = top; tab[h].size = n; tab[h].used = 1; tab[h].pin = 0;
    top += total;
    live_bytes += total;
    alloc_ok++;
    return h;
}

static void h_free(int h) {
    CHECK(tab[h].used && !tab[h].pin);
    unsigned hh, total;
    get_hdr(tab[h].off, &hh, &total);
    put_hdr(tab[h].off, DEAD, total);
    dead_bytes += total; live_bytes -= total;
    tab[h].used = 0;
}

static void walk_check(void) {
    unsigned p = 0, live = 0, dead = 0;
    int seen[NH] = {0};
    while (p < top) {
        unsigned h, t;
        get_hdr(p, &h, &t);
        CHECK(t >= HDR && (t & 7) == 0 && p + t <= top);
        if (h == DEAD) dead += t;
        else { CHECK(h < NH && tab[h].used && tab[h].off == p && !seen[h]); seen[h] = 1; live += t; }
        p += t;
    }
    CHECK(p == top && live == live_bytes && dead == dead_bytes);
    for (int i = 0; i < NH; i++) CHECK(!tab[i].used || seen[i]);
}

typedef struct { int h; unsigned n; unsigned tag; } Rec;

int main(void) {
    Rec live[NH];
    int nlive = 0, npinned = 0;
    unsigned tag = 1;
    for (int step = 0; step < 15000; step++) {
        unsigned op = rnd() % 100;
        if (nlive == 0 || (op < 50 && nlive < 70)) {
            unsigned n = 1 + rnd() % 150;
            unsigned before_compactions = (unsigned)compactions;
            int h = h_alloc(n);
            if (h >= 0) {
                pat_fill(deref(h), n, tag);
                live[nlive].h = h; live[nlive].n = n; live[nlive].tag = tag++; nlive++;
            }
            if (compactions != before_compactions) {
                /* handles keep working across a move */
                for (int i = 0; i < nlive; i++) CHECK(pat_ok(deref(live[i].h), live[i].n, live[i].tag));
                walk_check();
            }
        } else if (op < 85) {
            int i = (int)(rnd() % (unsigned)nlive);
            if (tab[live[i].h].pin) { tab[live[i].h].pin = 0; npinned--; }
            CHECK(pat_ok(deref(live[i].h), live[i].n, live[i].tag));
            h_free(live[i].h);
            live[i] = live[--nlive];
        } else if (op < 93) {
            int i = (int)(rnd() % (unsigned)nlive);
            if (!tab[live[i].h].pin && npinned < 6) { tab[live[i].h].pin = 1; npinned++; }
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            if (tab[live[i].h].pin) { tab[live[i].h].pin = 0; npinned--; }
        }
        if (step % 250 == 0) {
            walk_check();
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(deref(live[i].h), live[i].n, live[i].tag));
        }
    }
    walk_check();
    printf("allocs ok=%lu failed=%lu live blocks=%d\n", alloc_ok, alloc_fail, nlive);
    printf("compactions=%lu blocks moved=%lu bytes moved=%lu pinned obstacles=%lu\n", compactions, moved_blocks, moved_bytes, pinned_stops);
    printf("top=%u live bytes=%u dead bytes=%u pinned now=%d\n", top, live_bytes, dead_bytes, npinned);
    for (int i = 0; i < nlive; i++) { tab[live[i].h].pin = 0; h_free(live[i].h); }
    compact();
    CHECK(top == 0 && dead_bytes == 0 && live_bytes == 0);
    printf("after freeing everything and compacting: top=%u\n", top);
    return 0;
}
