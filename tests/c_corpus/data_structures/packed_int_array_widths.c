/*
 * title: Bit-packed integer arrays for every width plus frame-of-reference blocks
 * topic: data_structures
 * covers: bit packing, word-straddling reads and writes, width selection, frame of reference, block random access, compression ratio
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static uint64_t rs = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd64(void) { /* splitmix64 */
    uint64_t z = (rs += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

typedef struct { uint64_t *w; size_t n; unsigned bits; } Packed;

static uint64_t mask_of(unsigned bits) { return bits >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1); }
static size_t words_for(size_t n, unsigned bits) { return (n * bits + 63) / 64 + 1; }

static Packed p_new(size_t n, unsigned bits) {
    Packed p;
    p.n = n; p.bits = bits;
    p.w = calloc(words_for(n, bits), sizeof(uint64_t));
    CHECK(p.w);
    return p;
}
static uint64_t p_get(const Packed *p, size_t i) {
    size_t bitpos = i * p->bits, word = bitpos >> 6;
    unsigned sh = (unsigned)(bitpos & 63);
    uint64_t v = p->w[word] >> sh;
    if (sh + p->bits > 64) v |= p->w[word + 1] << (64 - sh);
    return v & mask_of(p->bits);
}
static void p_set(Packed *p, size_t i, uint64_t v) {
    uint64_t m = mask_of(p->bits);
    v &= m;
    size_t bitpos = i * p->bits, word = bitpos >> 6;
    unsigned sh = (unsigned)(bitpos & 63);
    p->w[word] = (p->w[word] & ~(m << sh)) | (v << sh);
    if (sh + p->bits > 64) {
        unsigned hi = sh + p->bits - 64;
        uint64_t m2 = mask_of(hi);
        p->w[word + 1] = (p->w[word + 1] & ~m2) | ((v >> (64 - sh)) & m2);
    }
}
/* saturating add, returns 0 on overflow (value is left unchanged) */
static int p_add(Packed *p, size_t i, uint64_t d) {
    uint64_t v = p_get(p, i), m = mask_of(p->bits);
    if (d > m - v) return 0;
    p_set(p, i, v + d);
    return 1;
}
static unsigned bits_needed(uint64_t v) { unsigned b = 1; while (b < 64 && (v >> b)) b++; return b; }
static void p_free(Packed *p) { free(p->w); p->w = NULL; }

/* frame of reference: blocks of 16 store a base and a per-block width for the offsets */
#define BLK 16
typedef struct { uint32_t base; unsigned bits; size_t word_off; } Blk;
typedef struct { Blk *blk; uint64_t *w; size_t n, nblk, nwords; } FOR;

static size_t bitread_words(unsigned bits) { return ((size_t)BLK * bits + 63) / 64; }
static FOR for_build(const uint32_t *v, size_t n) {
    FOR f;
    f.n = n;
    f.nblk = (n + BLK - 1) / BLK;
    f.blk = malloc(f.nblk * sizeof(Blk));
    CHECK(f.blk);
    size_t total = 0;
    for (size_t b = 0; b < f.nblk; b++) {
        uint32_t mn = v[b * BLK], mx = mn;
        for (size_t i = b * BLK; i < n && i < (b + 1) * BLK; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; }
        f.blk[b].base = mn;
        f.blk[b].bits = mx == mn ? 1 : bits_needed((uint64_t)(mx - mn));
        f.blk[b].word_off = total;
        total += bitread_words(f.blk[b].bits) + 1;
    }
    f.nwords = total;
    f.w = calloc(total, sizeof(uint64_t));
    CHECK(f.w);
    for (size_t b = 0; b < f.nblk; b++) {
        Packed view = { f.w + f.blk[b].word_off, BLK, f.blk[b].bits };
        for (size_t i = b * BLK; i < n && i < (b + 1) * BLK; i++) p_set(&view, i - b * BLK, (uint64_t)(v[i] - f.blk[b].base));
    }
    return f;
}
static uint32_t for_get(const FOR *f, size_t i) {
    size_t b = i / BLK;
    Packed view = { f->w + f->blk[b].word_off, BLK, f->blk[b].bits };
    return f->blk[b].base + (uint32_t)p_get(&view, i - b * BLK);
}

int main(void) {
    long total_checks = 0;
    size_t straddles = 0;
    for (unsigned bits = 1; bits <= 64; bits++) {
        enum { N = 150 };
        Packed p = p_new(N, bits);
        uint64_t model[N];
        uint64_t m = mask_of(bits);
        for (int i = 0; i < N; i++) { model[i] = rnd64() & m; p_set(&p, (size_t)i, model[i]); }
        for (int i = 0; i < N; i++) { CHECK(p_get(&p, (size_t)i) == model[i]); total_checks++; }
        /* random overwrites must not disturb neighbours */
        for (int t = 0; t < 400; t++) {
            size_t i = (size_t)(rnd64() % N);
            uint64_t v = rnd64() & m;
            p_set(&p, i, v);
            model[i] = v;
            if (i > 0) CHECK(p_get(&p, i - 1) == model[i - 1]);
            if (i + 1 < N) CHECK(p_get(&p, i + 1) == model[i + 1]);
            CHECK(p_get(&p, i) == v);
            total_checks++;
        }
        /* values wider than the field are truncated, not spilled */
        if (bits < 64) {
            p_set(&p, 7, ~(uint64_t)0);
            model[7] = m;
            CHECK(p_get(&p, 6) == model[6] && p_get(&p, 8) == model[8] && p_get(&p, 7) == m);
        }
        /* add with overflow detection */
        for (int t = 0; t < 50; t++) {
            size_t i = (size_t)(rnd64() % N);
            uint64_t d = rnd64() & mask_of(bits > 3 ? bits - 2 : 1);
            int ok = p_add(&p, i, d);
            int expect_ok = d <= m - model[i];
            CHECK(ok == expect_ok);
            if (ok) model[i] += d;
            CHECK(p_get(&p, i) == model[i]);
        }
        for (size_t i = 0; i < N; i++) if ((i * bits) / 64 != ((i + 1) * bits - 1) / 64) straddles++;
        if (bits == 1 || bits == 7 || bits == 13 || bits == 32 || bits == 33 || bits == 64) {
            uint64_t sum = 0;
            for (int i = 0; i < N; i++) sum += p_get(&p, (size_t)i);
            printf("width %2u: %zu words for %d values, checksum %llu\n", bits, words_for(N, bits) - 1, N, (unsigned long long)(sum % 1000003u));
        }
        p_free(&p);
    }
    printf("all 64 widths verified: %ld reads/writes, %zu word-straddling elements\n", total_checks, straddles);

    /* width selection: minimal bits for the maximum value */
    CHECK(bits_needed(0) == 1 && bits_needed(1) == 1 && bits_needed(2) == 2 && bits_needed(255) == 8 && bits_needed(256) == 9);
    CHECK(bits_needed(~(uint64_t)0) == 64 && bits_needed((uint64_t)1 << 63) == 64);

    /* frame of reference over slowly growing timestamps with jitter and rare jumps */
    enum { M = 1000 };
    uint32_t ts[M];
    uint32_t t = 1000000;
    for (int i = 0; i < M; i++) {
        t += 3 + (uint32_t)(rnd64() % 20);
        if (rnd64() % 97 == 0) t += 100000;
        ts[i] = t;
    }
    FOR f = for_build(ts, M);
    for (int i = 0; i < M; i++) CHECK(for_get(&f, (size_t)i) == ts[i]);
    size_t packed_bytes = f.nwords * 8 + f.nblk * sizeof(Blk);
    int hist[33] = { 0 };
    for (size_t b = 0; b < f.nblk; b++) hist[f.blk[b].bits]++;
    printf("frame of reference: %d values in %zu blocks, %zu bytes vs %zu raw\n", M, f.nblk, packed_bytes, (size_t)M * sizeof(uint32_t));
    printf("  block widths:");
    for (int b = 1; b <= 32; b++) if (hist[b]) printf(" %d-bit x%d", b, hist[b]);
    printf("\n");
    free(f.blk);
    free(f.w);
    return 0;
}
