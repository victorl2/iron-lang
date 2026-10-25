/*
 * title: Multi-size pool with size classes and class fallback
 * topic: memory
 * covers: size-class pools, class lookup by address range, upward fallback, internal fragmentation, per-class stats
 * deps: libc
 */
#define SEED 0xC1A55E5ULL
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

#define NCLASS 5
static const unsigned class_size[NCLASS] = { 16, 32, 64, 128, 256 };
static const unsigned class_slots[NCLASS] = { 64, 48, 32, 16, 8 };

static _Alignas(16) unsigned char heap[16 * 64 + 32 * 48 + 64 * 32 + 128 * 16 + 256 * 8];
static size_t class_base[NCLASS];
static int32_t free_head[NCLASS];
static unsigned in_use[NCLASS], peak[NCLASS], served[NCLASS], fallbacks[NCLASS];
static size_t internal_frag, total_requested;

static void pools_init(void) {
    size_t off = 0;
    for (int c = 0; c < NCLASS; c++) {
        class_base[c] = off;
        for (unsigned i = 0; i < class_slots[c]; i++) {
            int32_t nx = i + 1 < class_slots[c] ? (int32_t)(i + 1) : -1;
            memcpy(heap + off + (size_t)i * class_size[c], &nx, sizeof nx);
        }
        free_head[c] = 0;
        off += (size_t)class_size[c] * class_slots[c];
    }
    CHECK(off == sizeof heap);
}

static int class_for(size_t n) {
    for (int c = 0; c < NCLASS; c++) if (n <= class_size[c]) return c;
    return -1;
}

static void *mp_alloc(size_t n) {
    int c = class_for(n);
    if (c < 0) return NULL;
    int start = c;
    while (c < NCLASS && free_head[c] < 0) c++;
    if (c == NCLASS) return NULL;
    if (c != start) fallbacks[start]++;
    int32_t i = free_head[c], nx;
    unsigned char *p = heap + class_base[c] + (size_t)i * class_size[c];
    memcpy(&nx, p, sizeof nx);
    free_head[c] = nx;
    in_use[c]++; served[c]++;
    if (in_use[c] > peak[c]) peak[c] = in_use[c];
    internal_frag += class_size[c] - n;
    total_requested += n;
    return p;
}

/* free needs the original size for the fragmentation accounting only; the class comes from the address */
static void mp_free(void *p, size_t n) {
    size_t off = (size_t)((unsigned char *)p - heap);
    CHECK(off < sizeof heap);
    int c = NCLASS - 1;
    while (off < class_base[c]) c--;
    size_t rel = off - class_base[c];
    CHECK(rel % class_size[c] == 0);
    int32_t i = (int32_t)(rel / class_size[c]);
    memcpy(p, &free_head[c], sizeof(int32_t));
    free_head[c] = i;
    in_use[c]--;
    internal_frag -= class_size[c] - n;
}

static void mp_check(void) {
    for (int c = 0; c < NCLASS; c++) {
        unsigned n = 0;
        for (int32_t i = free_head[c]; i >= 0; ) {
            CHECK((unsigned)i < class_slots[c]);
            n++;
            CHECK(n <= class_slots[c]);
            memcpy(&i, heap + class_base[c] + (size_t)i * class_size[c], sizeof i);
        }
        CHECK(n + in_use[c] == class_slots[c]);
    }
}

typedef struct { unsigned char *p; size_t n; unsigned tag; } Rec;

int main(void) {
    pools_init();
    Rec live[200];
    int nlive = 0, refused = 0;
    unsigned tag = 1;
    for (int step = 0; step < 6000; step++) {
        if (nlive == 0 || (rnd() % 100 < 55 && nlive < 200)) {
            unsigned k = rnd() % 10;
            size_t n = k < 5 ? 1 + rnd() % 32 : k < 8 ? 33 + rnd() % 96 : 129 + rnd() % 130;
            unsigned char *p = mp_alloc(n);
            if (!p) { refused++; continue; }
            if (n > 4) pat_fill(p + 4, n - 4, tag);
            memcpy(p, &tag, n > 4 ? 4 : n);
            live[nlive].p = p; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            Rec *r = &live[i];
            unsigned t;
            size_t n = r->n;
            memcpy(&t, r->p, n > 4 ? 4 : n);
            if (n >= 4) CHECK(t == r->tag);
            if (n > 4) CHECK(pat_ok(r->p + 4, n - 4, r->tag));
            mp_free(r->p, r->n);
            *r = live[--nlive];
        }
        if (step % 100 == 0) mp_check();
    }
    mp_check();
    for (int c = 0; c < NCLASS; c++)
        printf("class %3u: served=%5u peak=%2u/%2u fallbacks-from=%u in-use=%u\n",
               class_size[c], served[c], peak[c], class_slots[c], fallbacks[c], in_use[c]);
    printf("refused=%d live=%d internal fragmentation now=%zu of %zu requested\n", refused, nlive, internal_frag, total_requested);
    for (int i = 0; i < nlive; i++) mp_free(live[i].p, live[i].n);
    mp_check();
    CHECK(internal_frag == 0);
    for (int c = 0; c < NCLASS; c++) CHECK(in_use[c] == 0);
    printf("drained\n");
    return 0;
}
