/*
 * title: Dynamic bit vector with rank and select
 * topic: data_structures
 * covers: bit array, word packing, popcount, rank/select with block counters, resize, bit operations
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xAB1C5ED5DA6D8118ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 33); }

typedef struct { uint64_t *w; size_t nbits, nwords; uint32_t *blk; int dirty; } BitVec; /* blk[i] = ones before word i */

static int popc(uint64_t x) { x = x - ((x >> 1) & 0x5555555555555555ULL); x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL); x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL; return (int)((x * 0x0101010101010101ULL) >> 56); }
static void bv_init(BitVec *b) { b->w = NULL; b->blk = NULL; b->nbits = b->nwords = 0; b->dirty = 1; }
static void bv_free(BitVec *b) { free(b->w); free(b->blk); }
static void bv_resize(BitVec *b, size_t nbits) {
    size_t nw = (nbits + 63) / 64;
    b->w = realloc(b->w, (nw ? nw : 1) * sizeof(uint64_t)); CHECK(b->w);
    if (nw > b->nwords) memset(b->w + b->nwords, 0, (nw - b->nwords) * sizeof(uint64_t));
    if (nbits < b->nbits && nbits % 64) b->w[nw - 1] &= (1ULL << (nbits % 64)) - 1; /* clear stale tail bits */
    b->nbits = nbits; b->nwords = nw;
    b->blk = realloc(b->blk, (nw + 1) * sizeof(uint32_t)); CHECK(b->blk);
    b->dirty = 1;
}
static int bv_get(const BitVec *b, size_t i) { CHECK(i < b->nbits); return (int)((b->w[i / 64] >> (i % 64)) & 1); }
static void bv_set(BitVec *b, size_t i, int v) {
    CHECK(i < b->nbits);
    if (v) b->w[i / 64] |= 1ULL << (i % 64); else b->w[i / 64] &= ~(1ULL << (i % 64));
    b->dirty = 1;
}
static void bv_flip(BitVec *b, size_t i) { CHECK(i < b->nbits); b->w[i / 64] ^= 1ULL << (i % 64); b->dirty = 1; }
static void build_index(BitVec *b) {
    if (!b->dirty) return;
    uint32_t acc = 0;
    for (size_t i = 0; i < b->nwords; i++) { b->blk[i] = acc; acc += (uint32_t)popc(b->w[i]); }
    b->blk[b->nwords] = acc; b->dirty = 0;
}
/* number of ones in [0, i) */
static size_t bv_rank1(BitVec *b, size_t i) {
    CHECK(i <= b->nbits); build_index(b);
    size_t wi = i / 64, r = b->blk[wi];
    if (i % 64) r += (size_t)popc(b->w[wi] & ((1ULL << (i % 64)) - 1));
    return r;
}
/* position of the k-th one (0-based) or nbits when there is none */
static size_t bv_select1(BitVec *b, size_t k) {
    build_index(b);
    if (k >= b->blk[b->nwords]) return b->nbits;
    size_t lo = 0, hi = b->nwords; /* last word with blk <= k */
    while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (b->blk[mid] <= k) lo = mid; else hi = mid; }
    uint64_t w = b->w[lo]; size_t need = k - b->blk[lo];
    for (size_t bit = 0; bit < 64; bit++) if ((w >> bit) & 1) { if (need == 0) return lo * 64 + bit; need--; }
    CHECK(0); return 0;
}
static size_t bv_next_zero(const BitVec *b, size_t from) {
    for (size_t i = from; i < b->nbits; i++) if (!bv_get(b, i)) return i;
    return b->nbits;
}

int main(void) {
    BitVec b; bv_init(&b);
    static unsigned char m[20000]; size_t mn = 0;
    long cnt[7] = {0};
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        if (op < 10 && mn < 15000) { size_t n = mn + rnd() % 200; bv_resize(&b, n); while (mn < n) m[mn++] = 0; cnt[0]++; }
        else if (op < 15 && mn > 300) { size_t n = mn - rnd() % 250; bv_resize(&b, n); mn = n; cnt[1]++; }
        else if (mn == 0) continue;
        else if (op < 45) { size_t i = rnd() % mn; int v = (int)(rnd() % 2); bv_set(&b, i, v); m[i] = (unsigned char)v; cnt[2]++; }
        else if (op < 55) { size_t i = rnd() % mn; bv_flip(&b, i); m[i] ^= 1; cnt[3]++; }
        else if (op < 80) {
            size_t i = rnd() % (mn + 1), want = 0; for (size_t j = 0; j < i; j++) want += m[j];
            CHECK(bv_rank1(&b, i) == want); cnt[4]++;
        } else if (op < 95) {
            size_t total = bv_rank1(&b, mn), k = rnd() % (total + 3), want = mn, seen = 0;
            for (size_t j = 0; j < mn; j++) if (m[j]) { if (seen == k) { want = j; break; } seen++; }
            CHECK(bv_select1(&b, k) == want); cnt[5]++;
        } else {
            size_t i = rnd() % mn, want = mn; for (size_t j = i; j < mn; j++) if (!m[j]) { want = j; break; }
            CHECK(bv_next_zero(&b, i) == want); cnt[6]++;
        }
        CHECK(b.nbits == mn);
        if (step % 200 == 0) for (size_t i = 0; i < mn; i++) CHECK(bv_get(&b, i) == m[i]);
    }
    printf("grow=%ld shrink=%ld set=%ld flip=%ld rank=%ld select=%ld next_zero=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6]);
    size_t ones = bv_rank1(&b, mn);
    printf("bits=%zu words=%zu ones=%zu\n", mn, b.nwords, ones);
    printf("select samples: %zu %zu %zu\n", bv_select1(&b, 0), bv_select1(&b, ones / 2), bv_select1(&b, ones - 1));
    for (size_t k = 0; k < ones; k++) CHECK(bv_rank1(&b, bv_select1(&b, k)) == k); /* rank(select(k)) == k */
    bv_free(&b);
    return 0;
}
