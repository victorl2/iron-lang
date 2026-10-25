/*
 * title: Dynamic bitset with set algebra, shifts and iteration
 * topic: data_structures
 * covers: bitset, 64-bit words, and/or/xor/andnot, complement with tail mask, shift left and right across words, next-set-bit iteration, popcount, bool array oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned long long rs = 0xB175E7ULL * 0x9E3779ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { size_t n, nw; uint64_t *w; } Bits;

static Bits bnew(size_t n) { Bits b; b.n = n; b.nw = (n + 63) / 64; b.w = calloc(b.nw ? b.nw : 1, sizeof(uint64_t)); return b; }
static void bfree(Bits *b) { free(b->w); }
static void trim(Bits *b) { if (b->n % 64) b->w[b->nw - 1] &= (((uint64_t)1 << (b->n % 64)) - 1); }
static void bset(Bits *b, size_t i) { b->w[i >> 6] |= (uint64_t)1 << (i & 63); }
static void bclr(Bits *b, size_t i) { b->w[i >> 6] &= ~((uint64_t)1 << (i & 63)); }
static int btest(const Bits *b, size_t i) { return (int)((b->w[i >> 6] >> (i & 63)) & 1u); }
static size_t pop64(uint64_t x) { size_t c = 0; x = x - ((x >> 1) & 0x5555555555555555ULL); x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL); x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL; c = (size_t)((x * 0x0101010101010101ULL) >> 56); return c; }
static size_t bcount(const Bits *b) { size_t c = 0; for (size_t i = 0; i < b->nw; i++) c += pop64(b->w[i]); return c; }
static void band(Bits *d, const Bits *a, const Bits *b) { for (size_t i = 0; i < d->nw; i++) d->w[i] = a->w[i] & b->w[i]; }
static void bor(Bits *d, const Bits *a, const Bits *b) { for (size_t i = 0; i < d->nw; i++) d->w[i] = a->w[i] | b->w[i]; }
static void bxor(Bits *d, const Bits *a, const Bits *b) { for (size_t i = 0; i < d->nw; i++) d->w[i] = a->w[i] ^ b->w[i]; }
static void bandnot(Bits *d, const Bits *a, const Bits *b) { for (size_t i = 0; i < d->nw; i++) d->w[i] = a->w[i] & ~b->w[i]; }
static void bnot(Bits *d, const Bits *a) { for (size_t i = 0; i < d->nw; i++) d->w[i] = ~a->w[i]; trim(d); }
static int bsubset(const Bits *a, const Bits *b) { for (size_t i = 0; i < a->nw; i++) if (a->w[i] & ~b->w[i]) return 0; return 1; }
static void bshl(Bits *d, const Bits *a, size_t k) {
    size_t ws = k / 64, bs = k % 64;
    for (size_t i = d->nw; i-- > 0; ) {
        uint64_t v = 0;
        if (i >= ws) { v = a->w[i - ws] << bs; if (bs && i > ws) v |= a->w[i - ws - 1] >> (64 - bs); }
        d->w[i] = v;
    }
    trim(d);
}
static void bshr(Bits *d, const Bits *a, size_t k) {
    size_t ws = k / 64, bs = k % 64;
    for (size_t i = 0; i < d->nw; i++) {
        uint64_t v = 0;
        if (i + ws < a->nw) { v = a->w[i + ws] >> bs; if (bs && i + ws + 1 < a->nw) v |= a->w[i + ws + 1] << (64 - bs); }
        d->w[i] = v;
    }
}
/* index of next set bit at or after i, or n */
static size_t bnext(const Bits *b, size_t i) {
    if (i >= b->n) return b->n;
    size_t wi = i >> 6;
    uint64_t x = b->w[wi] & (~(uint64_t)0 << (i & 63));
    for (;;) {
        if (x) { size_t p = wi * 64, bit = 0; while (!((x >> bit) & 1u)) bit++; return p + bit; }
        if (++wi >= b->nw) return b->n;
        x = b->w[wi];
    }
}

static void fill_random(Bits *b, unsigned char *ref, int density) {
    for (size_t i = 0; i < b->n; i++) { ref[i] = (int)(rnd() % 100) < density; if (ref[i]) bset(b, i); }
}
static void same(const Bits *b, const unsigned char *ref, const char *what) {
    for (size_t i = 0; i < b->n; i++) check(btest(b, i) == ref[i], what);
    if (b->n % 64) check((b->w[b->nw - 1] >> (b->n % 64)) == 0, "tail bits clear");
}

int main(void) {
    size_t sizes[5] = {1, 63, 64, 130, 1000};
    for (int si = 0; si < 5; si++) {
        size_t n = sizes[si];
        Bits a = bnew(n), b = bnew(n), d = bnew(n);
        unsigned char *ra = calloc(n, 1), *rb = calloc(n, 1), *rd = calloc(n, 1);
        fill_random(&a, ra, 30); fill_random(&b, rb, 50);
        band(&d, &a, &b); for (size_t i = 0; i < n; i++) rd[i] = ra[i] & rb[i]; same(&d, rd, "and");
        size_t inter = bcount(&d);
        bor(&d, &a, &b); for (size_t i = 0; i < n; i++) rd[i] = ra[i] | rb[i]; same(&d, rd, "or");
        size_t uni = bcount(&d);
        bxor(&d, &a, &b); for (size_t i = 0; i < n; i++) rd[i] = ra[i] ^ rb[i]; same(&d, rd, "xor");
        size_t sym = bcount(&d);
        bandnot(&d, &a, &b); for (size_t i = 0; i < n; i++) rd[i] = ra[i] & !rb[i]; same(&d, rd, "andnot");
        bnot(&d, &a); for (size_t i = 0; i < n; i++) rd[i] = !ra[i]; same(&d, rd, "not");
        check(bcount(&d) + bcount(&a) == n, "complement count");
        check(uni == bcount(&a) + bcount(&b) - inter && sym == uni - inter, "inclusion exclusion");
        band(&d, &a, &b); check(bsubset(&d, &a) && bsubset(&d, &b), "intersection is subset");
        size_t shifts[5] = {0, 1, 63, 64, 77};
        size_t shifted_total = 0;
        for (int k = 0; k < 5; k++) {
            size_t s = shifts[k];
            bshl(&d, &a, s); for (size_t i = 0; i < n; i++) rd[i] = i >= s ? ra[i - s] : 0; same(&d, rd, "shl"); shifted_total += bcount(&d);
            bshr(&d, &a, s); for (size_t i = 0; i < n; i++) rd[i] = i + s < n ? ra[i + s] : 0; same(&d, rd, "shr"); shifted_total += bcount(&d);
        }
        /* iteration */
        size_t it = 0, first = n, last = n, gaps = 0, prev = 0;
        for (size_t i = bnext(&a, 0); i < n; i = bnext(&a, i + 1)) {
            check(ra[i], "iterated bit is set");
            if (it == 0) first = i; else gaps += i - prev;
            prev = i; last = i; it++;
        }
        check(it == bcount(&a), "iteration count");
        bclr(&a, 0); ra[0] = 0; same(&a, ra, "clear");
        printf("n=%4zu |a|=%3zu |b|=%3zu and=%3zu or=%3zu xor=%3zu shifted=%4zu first=%zu last=%zu gaps=%zu\n",
               n, it, bcount(&b), inter, uni, sym, shifted_total, first == n ? 0 : first, last == n ? 0 : last, gaps);
        bfree(&a); bfree(&b); bfree(&d); free(ra); free(rb); free(rd);
    }
    return 0;
}
