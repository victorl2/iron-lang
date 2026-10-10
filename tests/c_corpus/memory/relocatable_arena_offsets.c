/*
 * title: Position-independent arena using 32-bit offsets, copied and grown
 * topic: memory
 * covers: offset instead of pointer references, self-describing arena image, memcpy relocation, growth by copying into a larger malloc'd block, string interning, BST inside the arena
 * deps: libc
 */
#define SEED 0x2E10CA7EULL
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

/* arena image layout: header at offset 0, everything else referenced by u32 offsets from the image start */
#define H_USED 0
#define H_ROOT 4
#define H_BUCKETS 8
#define H_NSTR 12
#define H_NNODE 16
#define HDR_SIZE 32u
#define NBUCKET 64u

typedef struct { unsigned char *base; size_t cap; int owned; } Arena;
static int growths;

static uint32_t rd32(const Arena *a, uint32_t off) { uint32_t v; memcpy(&v, a->base + off, 4); return v; }
static void wr32(Arena *a, uint32_t off, uint32_t v) { memcpy(a->base + off, &v, 4); }

static uint32_t arena_alloc(Arena *a, uint32_t n) {
    n = (n + 3u) & ~3u;
    uint32_t used = rd32(a, H_USED);
    if (used + n > a->cap) {
        size_t ncap = a->cap * 2;
        while (used + n > ncap) ncap *= 2;
        unsigned char *nb = malloc(ncap);
        CHECK(nb);
        memcpy(nb, a->base, used);           /* position independence: no fix-ups needed */
        if (a->owned) free(a->base);
        a->base = nb; a->cap = ncap; a->owned = 1;
        growths++;
    }
    wr32(a, H_USED, used + n);
    memset(a->base + used, 0, n);
    return used;
}

static void arena_init(Arena *a, unsigned char *buf, size_t cap) {
    a->base = buf; a->cap = cap; a->owned = 0;
    memset(buf, 0, HDR_SIZE);
    wr32(a, H_USED, HDR_SIZE);
    uint32_t b = arena_alloc(a, NBUCKET * 4);
    wr32(a, H_BUCKETS, b);
}

static uint32_t fnv(const char *s, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* string entry: [next][hash][len][chars\0] */
static uint32_t intern(Arena *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), h = fnv(s, len);
    uint32_t slot = rd32(a, H_BUCKETS) + (h % NBUCKET) * 4;
    for (uint32_t e = rd32(a, slot); e; e = rd32(a, e))
        if (rd32(a, e + 4) == h && rd32(a, e + 8) == len && memcmp(a->base + e + 12, s, len) == 0) return e;
    uint32_t e = arena_alloc(a, 12 + len + 1);
    slot = rd32(a, H_BUCKETS) + (h % NBUCKET) * 4;     /* base may have moved: recompute from offsets */
    wr32(a, e, rd32(a, slot)); wr32(a, e + 4, h); wr32(a, e + 8, len);
    memcpy(a->base + e + 12, s, len);
    wr32(a, slot, e);
    wr32(a, H_NSTR, rd32(a, H_NSTR) + 1);
    return e;
}

/* node: [key][left][right][name][blob]; blob: [len][bytes] filled with a pattern keyed by the node key */
static int tree_insert(Arena *a, uint32_t key, const char *name) {
    uint32_t nm = intern(a, name);
    uint32_t cur = rd32(a, H_ROOT), parent = 0;
    int went_left = 0;
    while (cur) {
        uint32_t k = rd32(a, cur);
        if (key == k) return 0;
        parent = cur;
        went_left = key < k;
        cur = rd32(a, cur + (went_left ? 4 : 8));
    }
    uint32_t blen = 8 + key % 24;
    uint32_t blob = arena_alloc(a, 4 + blen);
    wr32(a, blob, blen);
    pat_fill(a->base + blob + 4, blen, key);
    uint32_t n = arena_alloc(a, 20);
    wr32(a, n, key); wr32(a, n + 12, nm); wr32(a, n + 16, blob);
    if (!parent) wr32(a, H_ROOT, n); else wr32(a, parent + (went_left ? 4 : 8), n);
    wr32(a, H_NNODE, rd32(a, H_NNODE) + 1);
    return 1;
}

static const char *find(const Arena *a, uint32_t key) {
    uint32_t cur = rd32(a, H_ROOT);
    while (cur) {
        uint32_t k = rd32(a, cur);
        if (k == key) return (const char *)a->base + rd32(a, cur + 12) + 12;
        cur = rd32(a, cur + (key < k ? 4 : 8));
    }
    return NULL;
}

static uint32_t walk(const Arena *a, uint32_t n, uint32_t *sum, uint32_t *count, uint32_t *last, int *depth_max, int depth) {
    if (!n) return 0;
    if (depth > *depth_max) *depth_max = depth;
    walk(a, rd32(a, n + 4), sum, count, last, depth_max, depth + 1);
    uint32_t k = rd32(a, n);
    CHECK(*count == 0 || k > *last);
    *last = k;
    const char *nm = (const char *)a->base + rd32(a, n + 12) + 12;
    uint32_t blob = rd32(a, n + 16);
    CHECK(pat_ok(a->base + blob + 4, rd32(a, blob), k));
    *sum = *sum * 33u + k + fnv(nm, (uint32_t)strlen(nm));
    (*count)++;
    walk(a, rd32(a, n + 8), sum, count, last, depth_max, depth + 1);
    return 1;
}

static void summarize(const Arena *a, uint32_t *sum, uint32_t *count, int *depth) {
    uint32_t last = 0;
    *sum = 5381; *count = 0; *depth = 0;
    walk(a, rd32(a, H_ROOT), sum, count, &last, depth, 1);
}

#define KEYS 4000
static int ref_name[KEYS];       /* 0: absent, else vocabulary index + 1 */
static char vocab[200][12];

static void insert_random(Arena *a, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t key = rnd() % KEYS;
        unsigned v = rnd() % 200;
        int added = tree_insert(a, key, vocab[v]);
        CHECK(added == (ref_name[key] == 0));
        if (added) ref_name[key] = (int)v + 1;
    }
}

static void verify_against_reference(const Arena *a) {
    uint32_t cnt = 0;
    for (int k = 0; k < KEYS; k++) {
        const char *s = find(a, (uint32_t)k);
        if (ref_name[k]) { CHECK(s && strcmp(s, vocab[ref_name[k] - 1]) == 0); cnt++; }
        else CHECK(s == NULL);
    }
    CHECK(rd32(a, H_NNODE) == cnt);
}

int main(void) {
    for (int i = 0; i < 200; i++) snprintf(vocab[i], sizeof vocab[i], "w%c%d", (char)('a' + i % 26), i);
    static _Alignas(16) unsigned char buf_a[8192], buf_b[8192];
    Arena a;
    arena_init(&a, buf_a, sizeof buf_a);
    insert_random(&a, 100);
    verify_against_reference(&a);
    uint32_t sum1, cnt1, sum2, cnt2; int d1, d2;
    summarize(&a, &sum1, &cnt1, &d1);
    printf("stage 1: nodes=%u strings=%u used=%u depth=%d checksum=%u\n", rd32(&a, H_NNODE), rd32(&a, H_NSTR), rd32(&a, H_USED), d1, sum1);

    /* relocation 1: byte copy into a different static buffer */
    Arena b;
    b.base = buf_b; b.cap = sizeof buf_b; b.owned = 0;
    memcpy(buf_b, buf_a, rd32(&a, H_USED));
    summarize(&b, &sum2, &cnt2, &d2);
    CHECK(sum1 == sum2 && cnt1 == cnt2 && d1 == d2);
    verify_against_reference(&b);
    printf("stage 2: copied image identical, checksum=%u\n", sum2);

    /* relocation 2: copy into malloc'd memory and keep growing there (forces several moves) */
    Arena c;
    c.cap = 8192; c.owned = 1;
    c.base = malloc(c.cap);
    CHECK(c.base);
    memcpy(c.base, buf_b, rd32(&b, H_USED));
    int g0 = growths;
    for (int round = 0; round < 6; round++) insert_random(&c, 300);
    int moves = growths - g0;
    verify_against_reference(&c);
    summarize(&c, &sum1, &cnt1, &d1);
    printf("stage 3: nodes=%u strings=%u used=%u depth=%d checksum=%u\n", rd32(&c, H_NNODE), rd32(&c, H_NSTR), rd32(&c, H_USED), d1, sum1);
    CHECK(moves >= 1);
    printf("growth copies while inserting: %d\n", moves);

    /* relocation 3: shrink-wrap copy of exactly the used bytes, queries still work */
    Arena d;
    d.cap = rd32(&c, H_USED); d.owned = 1;
    d.base = malloc(d.cap);
    CHECK(d.base);
    memcpy(d.base, c.base, d.cap);
    summarize(&d, &sum2, &cnt2, &d2);
    CHECK(sum1 == sum2 && cnt1 == cnt2);
    verify_against_reference(&d);
    printf("stage 4: shrink-wrapped copy of %zu bytes verified\n", d.cap);
    free(c.base);
    free(d.base);
    return 0;
}
