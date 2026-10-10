/*
 * title: Three-level hierarchical bitmap block allocator with run search inside words
 * topic: memory
 * covers: summary bitmaps, count-trailing-zeros without builtins, next-free search with hint and wraparound, shift-and run finding within a word, brute-force cross-check, invariant between levels
 * deps: libc
 */
#define SEED 0x817A2C41ULL
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

#define NBLK 32768u
#define BSZ 8u
#define W0 (NBLK / 64u)      /* 512 words */
#define W1 (W0 / 64u)        /* 8 words */

static _Alignas(16) unsigned char store[NBLK * BSZ];
static uint64_t l0[W0], l1[W1], l2;      /* bit set = free / has free */
static unsigned char model[NBLK];         /* 1 = free, brute force reference */
static unsigned long level_steps, allocs, frees, run_allocs, hint_wraps;
static unsigned used_blocks;

static unsigned ctz64(uint64_t x) {
    unsigned n = 0;
    if (!(x & 0xFFFFFFFFu)) { n += 32; x >>= 32; }
    if (!(x & 0xFFFFu)) { n += 16; x >>= 16; }
    if (!(x & 0xFFu)) { n += 8; x >>= 8; }
    if (!(x & 0xFu)) { n += 4; x >>= 4; }
    if (!(x & 3u)) { n += 2; x >>= 2; }
    if (!(x & 1u)) n += 1;
    return n;
}

static void set_free(unsigned b) {
    l0[b >> 6] |= (uint64_t)1 << (b & 63);
    l1[b >> 12] |= (uint64_t)1 << ((b >> 6) & 63);
    l2 |= (uint64_t)1 << (b >> 12);
    model[b] = 1;
}
static void clear_free(unsigned b) {
    l0[b >> 6] &= ~((uint64_t)1 << (b & 63));
    if (!l0[b >> 6]) {
        l1[b >> 12] &= ~((uint64_t)1 << ((b >> 6) & 63));
        if (!l1[b >> 12]) l2 &= ~((uint64_t)1 << (b >> 12));
    }
    model[b] = 0;
}

/* lowest free block index >= start, or NBLK if none; walks up and down the hierarchy */
static unsigned next_free_from(unsigned start) {
    if (start >= NBLK) return NBLK;
    unsigned w = start >> 6;
    level_steps++;
    uint64_t m = l0[w] & (~(uint64_t)0 << (start & 63));
    if (m) return (w << 6) | ctz64(m);
    /* next word in the same level-1 word */
    unsigned g = w >> 6, wb = (w & 63) + 1;
    level_steps++;
    if (wb < 64) {
        m = l1[g] & (~(uint64_t)0 << wb);
        if (m) { unsigned ww = (g << 6) | ctz64(m); return (ww << 6) | ctz64(l0[ww]); }
    }
    /* next group */
    level_steps++;
    if (g + 1 < 64) {
        m = l2 & (~(uint64_t)0 << (g + 1));
        if (m) {
            unsigned gg = ctz64(m);
            unsigned ww = (gg << 6) | ctz64(l1[gg]);
            return (ww << 6) | ctz64(l0[ww]);
        }
    }
    return NBLK;
}

static unsigned model_next_free(unsigned start) {
    for (unsigned i = start; i < NBLK; i++) if (model[i]) return i;
    return NBLK;
}

static long alloc1(unsigned hint) {
    unsigned b = next_free_from(hint);
    unsigned mb = model_next_free(hint);
    CHECK(b == mb);
    if (b == NBLK) {
        hint_wraps++;
        b = next_free_from(0);
        CHECK(b == model_next_free(0));
        if (b == NBLK) return -1;
    }
    clear_free(b);
    used_blocks++; allocs++;
    return (long)b;
}

/* run of n (1..32) free blocks fully inside one 64-bit word, lowest start first */
static long alloc_run(unsigned n) {
    for (unsigned b = next_free_from(0); b < NBLK; ) {
        unsigned w = b >> 6;
        uint64_t x = l0[w], r = x;
        for (unsigned k = 1; k < n; k++) r &= x >> k;   /* bit i set: blocks i..i+n-1 free in this word */
        if (r) {
            unsigned s = (w << 6) | ctz64(r);
            for (unsigned i = 0; i < n; i++) clear_free(s + i);
            used_blocks += n; run_allocs++;
            /* reference: lowest start with n free blocks that do not cross a word boundary */
            unsigned ref = NBLK;
            for (unsigned i = 0; i + n <= NBLK && ref == NBLK; i++) {
                if ((i >> 6) != ((i + n - 1) >> 6)) continue;
                int ok = 1;
                for (unsigned k = 0; k < n; k++) if (!(model[i + k] || (i + k >= s && i + k < s + n))) { ok = 0; break; }
                if (ok) ref = i;
            }
            CHECK(ref == s);
            return (long)s;
        }
        b = next_free_from((w + 1) << 6);
    }
    return -1;
}

static void free_blocks(unsigned b, unsigned n) {
    for (unsigned i = 0; i < n; i++) { CHECK(!model[b + i]); set_free(b + i); }
    used_blocks -= n; frees++;
}

static void check_levels(void) {
    unsigned freec = 0;
    for (unsigned w = 0; w < W0; w++) {
        for (unsigned bit = 0; bit < 64; bit++) {
            int f = (int)((l0[w] >> bit) & 1u);
            CHECK(f == model[(w << 6) | bit]);
            freec += (unsigned)f;
        }
        CHECK((((l1[w >> 6] >> (w & 63)) & 1u) != 0) == (l0[w] != 0));
    }
    for (unsigned g = 0; g < W1; g++) CHECK((((l2 >> g) & 1u) != 0) == (l1[g] != 0));
    CHECK(freec + used_blocks == NBLK);
}

typedef struct { unsigned b, n; unsigned tag; } Rec;

int main(void) {
    for (unsigned i = 0; i < NBLK; i++) set_free(i);
    static Rec live[3000];
    int nlive = 0, refused = 0;
    unsigned tag = 1, hint = 0;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        if (nlive == 0 || (op < 55 && nlive < 3000)) {
            long b; unsigned n;
            if (rnd() % 4 == 0) { n = 2 + rnd() % 30; b = alloc_run(n); }
            else { n = 1; b = alloc1(hint); if (b >= 0) hint = (unsigned)b + 1; if (rnd() % 8 == 0) hint = rnd() % NBLK; }
            if (b < 0) { refused++; continue; }
            pat_fill(store + (size_t)b * BSZ, (size_t)n * BSZ, tag);
            live[nlive].b = (unsigned)b; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(store + (size_t)live[i].b * BSZ, (size_t)live[i].n * BSZ, live[i].tag));
            free_blocks(live[i].b, live[i].n);
            live[i] = live[--nlive];
        }
        if (step % 500 == 0) check_levels();
    }
    check_levels();
    for (int i = 0; i < nlive; i++) CHECK(pat_ok(store + (size_t)live[i].b * BSZ, (size_t)live[i].n * BSZ, live[i].tag));
    printf("single allocs=%lu run allocs=%lu frees=%lu refused=%d\n", allocs, run_allocs, frees, refused);
    printf("hierarchy steps=%lu hint wraps=%lu\n", level_steps, hint_wraps);
    printf("blocks used=%u of %u, live records=%d\n", used_blocks, NBLK, nlive);
    /* fill the whole space one block at a time from hint 0 and confirm exhaustion */
    for (int i = 0; i < nlive; i++) free_blocks(live[i].b, live[i].n);
    unsigned got = 0;
    for (long r; (r = alloc1(got)) >= 0; ) { CHECK((unsigned)r == got); got++; }
    CHECK(got == NBLK && l2 == 0);
    printf("exhausted %u blocks, summary word=%llu\n", got, (unsigned long long)l2);
    /* punch holes near the start, then ask with a hint past them: the search wraps and finds them in order */
    for (unsigned i = 0; i < 20; i++) free_blocks(5 * i + 7, 1);
    unsigned long wraps0 = hint_wraps;
    long first = -1, last = -1;
    for (int i = 0; i < 20; i++) { long r = alloc1(30000); CHECK(r == (long)(5 * (unsigned)i + 7)); if (first < 0) first = r; last = r; }
    CHECK(alloc1(30000) < 0);
    printf("wrapped searches=%lu, holes refilled from %ld to %ld\n", hint_wraps - wraps0 - 1, first, last);
    return 0;
}
