/*
 * title: Bitmap block allocator with start-bit run tracking
 * topic: memory
 * covers: bitmap allocator, 64-bit word scanning, contiguous run search, start bitmap to recover run length, popcount, largest free run, fragmentation
 * deps: libc
 */
#define SEED 0xB17A11C0ULL
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

#define BLOCK 32u
#define NBLK 512u
#define WORDS (NBLK / 64u)

static _Alignas(16) unsigned char heap[NBLK * BLOCK];
static uint64_t used_bm[WORDS], start_bm[WORDS];
static unsigned blocks_used, peak_blocks;
static unsigned long words_skipped, bits_tested, allocs, frees, fails;

static int get(const uint64_t *bm, unsigned i) { return (int)((bm[i >> 6] >> (i & 63)) & 1u); }
static void put(uint64_t *bm, unsigned i, int v) {
    uint64_t m = (uint64_t)1 << (i & 63);
    if (v) bm[i >> 6] |= m; else bm[i >> 6] &= ~m;
}
static unsigned popcount(uint64_t x) { unsigned c = 0; while (x) { x &= x - 1; c++; } return c; }

/* first-fit search for `need` consecutive clear bits, skipping full words wholesale */
static long find_run(unsigned need) {
    unsigned i = 0;
    while (i + need <= NBLK) {
        if ((i & 63) == 0 && used_bm[i >> 6] == ~(uint64_t)0) { i += 64; words_skipped++; continue; }
        bits_tested++;
        if (get(used_bm, i)) { i++; continue; }
        unsigned j = i;
        while (j < NBLK && j - i < need && !get(used_bm, j)) { j++; bits_tested++; }
        if (j - i == need) return (long)i;
        i = j;
    }
    return -1;
}

static long bm_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + BLOCK - 1) / BLOCK);
    long s = find_run(need);
    if (s < 0) { fails++; return -1; }
    for (unsigned k = 0; k < need; k++) put(used_bm, (unsigned)s + k, 1);
    put(start_bm, (unsigned)s, 1);
    blocks_used += need;
    if (blocks_used > peak_blocks) peak_blocks = blocks_used;
    allocs++;
    return s * (long)BLOCK;
}

/* the length is recovered from the bitmaps: run ends at the next start bit or first unused bit */
static unsigned run_length(unsigned s) {
    unsigned k = s + 1;
    while (k < NBLK && get(used_bm, k) && !get(start_bm, k)) k++;
    return k - s;
}

static void bm_free(unsigned off) {
    CHECK(off % BLOCK == 0);
    unsigned s = off / BLOCK;
    CHECK(get(start_bm, s) && get(used_bm, s));
    unsigned len = run_length(s);
    for (unsigned k = 0; k < len; k++) put(used_bm, s + k, 0);
    put(start_bm, s, 0);
    blocks_used -= len;
    frees++;
}

static void bm_check(unsigned *nruns, unsigned *largest, unsigned *free_blocks) {
    unsigned pc = 0, sc = 0;
    for (unsigned w = 0; w < WORDS; w++) {
        pc += popcount(used_bm[w]); sc += popcount(start_bm[w]);
        CHECK((start_bm[w] & ~used_bm[w]) == 0);   /* every start bit lies inside a used block */
    }
    CHECK(pc == blocks_used);
    unsigned runs = 0, big = 0, cur = 0, fr = 0;
    for (unsigned i = 0; i < NBLK; i++) {
        if (!get(used_bm, i)) { cur++; fr++; if (cur > big) big = cur; }
        else { if (cur) runs++; cur = 0; }
    }
    if (cur) runs++;
    CHECK(fr == NBLK - blocks_used);
    (void)sc;
    *nruns = runs; *largest = big; *free_blocks = fr;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    Rec live[400];
    int nlive = 0;
    unsigned tag = 1, runs, big, fr;
    unsigned run_hist[6] = {0};
    for (int step = 0; step < 12000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 400)) {
            unsigned k = rnd() % 10;
            size_t n = k < 6 ? 1 + rnd() % 64 : k < 9 ? 65 + rnd() % 200 : 265 + rnd() % 800;
            long off = bm_alloc(n);
            if (off < 0) continue;
            unsigned need = (unsigned)((n + BLOCK - 1) / BLOCK);
            run_hist[need == 1 ? 0 : need <= 2 ? 1 : need <= 4 ? 2 : need <= 8 ? 3 : need <= 16 ? 4 : 5]++;
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            CHECK(run_length(live[i].off / BLOCK) == (unsigned)((live[i].n + BLOCK - 1) / BLOCK));
            bm_free(live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 100 == 0) {
            bm_check(&runs, &big, &fr);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    bm_check(&runs, &big, &fr);
    printf("allocs=%lu frees=%lu failed=%lu live=%d\n", allocs, frees, fails, nlive);
    printf("blocks used=%u peak=%u of %u; free=%u in %u runs, largest run=%u\n", blocks_used, peak_blocks, NBLK, fr, runs, big);
    printf("scan work: words skipped=%lu bits tested=%lu\n", words_skipped, bits_tested);
    printf("run-length classes 1,2,3-4,5-8,9-16,17+: %u %u %u %u %u %u\n", run_hist[0], run_hist[1], run_hist[2], run_hist[3], run_hist[4], run_hist[5]);
    for (int i = 0; i < nlive; i++) bm_free(live[i].off);
    bm_check(&runs, &big, &fr);
    CHECK(blocks_used == 0 && runs == 1 && big == NBLK);
    for (unsigned w = 0; w < WORDS; w++) CHECK(used_bm[w] == 0 && start_bm[w] == 0);
    printf("drained: one run of %u blocks\n", big);
    return 0;
}
