/*
 * title: Conservative collection with ambiguous stack roots
 * topic: memory
 * covers: address-range and cell-alignment tests, interior pointers, false retention from integers that look like addresses, stale stack slots, non-moving mark-sweep, precise versus conservative oracle sets
 * deps: libc
 */
#define SEED 0x5EED0012ULL
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*@RNG*/
/* Deterministic splitmix64 generator so every platform sees the same run. */
static uint64_t rng_s = SEED;
static inline uint32_t rnd(void) {
    uint64_t z = (rng_s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (uint32_t)(z >> 32);
}
static inline uint32_t rnd_n(uint32_t n) {
    uint32_t r = rnd();
    return r % n;
}

/*@ENDRNG*/
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__);    \
            exit(1);                                                          \
        }                                                                     \
    } while (0)



#define BASE 0x40000000u
#define CELLB 16u /* bytes per cell: two pointer words and two data words */
#define NCELL 220
#define SD 96

static uint32_t cell[NCELL][4];
static unsigned char used[NCELL], mark[NCELL];
static uint32_t stack[SD];
static unsigned char truth[SD]; /* 1: a real reference, 0: noise or dead slot */
static int sp;
static int rounds, freed, false_kept_total, false_kept_max, interior_hits, ignored_free, ambiguous_words, max_sp;

static uint32_t addr_of(int c) { return BASE + (uint32_t)c * CELLB; }

/* Collector's decoder: arithmetic. Returns a cell index or -1. */
static int decode(uint32_t w) {
    if (w < BASE || w >= BASE + NCELL * CELLB)
        return -1;
    return (int)((w - BASE) / CELLB);
}
/* Oracle's decoder: linear search, no division. */
static int decode_slow(uint32_t w) {
    for (int c = 0; c < NCELL; c++)
        if (used[c] && w >= addr_of(c) && w < addr_of(c) + CELLB)
            return c;
    return -1;
}

static void mark_from_word(uint32_t w, int *work, int *nw) {
    ambiguous_words++;
    int c = decode(w);
    if (c < 0)
        return;
    if (!used[c]) {
        ignored_free++;
        return;
    }
    if (w != addr_of(c))
        interior_hits++;
    if (!mark[c]) {
        mark[c] = 1;
        work[(*nw)++] = c;
    }
}

static void conservative_mark(void) {
    int work[NCELL], nw = 0;
    for (int i = 0; i < sp; i++)
        mark_from_word(stack[i], work, &nw);
    while (nw > 0) {
        int c = work[--nw];
        for (int k = 0; k < 4; k++) /* heap cells are untyped: every word is ambiguous */
            mark_from_word(cell[c][k], work, &nw);
    }
}

/* Precise closure from the real references only, following the two pointer fields. */
static int precise_reach(unsigned char *seen) {
    int work[NCELL], nw = 0, n = 0;
    memset(seen, 0, NCELL);
    for (int i = 0; i < sp; i++)
        if (truth[i]) {
            int c = decode_slow(stack[i]);
            CHECK(c >= 0);
            if (!seen[c]) {
                seen[c] = 1;
                work[nw++] = c;
            }
        }
    while (nw > 0) {
        int c = work[--nw];
        n++;
        for (int k = 0; k < 2; k++)
            if (cell[c][k] != 0) {
                int t = decode_slow(cell[c][k]);
                CHECK(t >= 0);
                if (!seen[t]) {
                    seen[t] = 1;
                    work[nw++] = t;
                }
            }
    }
    return n;
}

/* Oracle for the conservative result: same words, independent decoding. */
static void conservative_oracle(unsigned char *seen) {
    int work[NCELL], nw = 0;
    memset(seen, 0, NCELL);
    for (int i = 0; i < sp; i++) {
        int c = decode_slow(stack[i]);
        if (c >= 0 && !seen[c]) {
            seen[c] = 1;
            work[nw++] = c;
        }
    }
    while (nw > 0) {
        int c = work[--nw];
        for (int k = 0; k < 4; k++) {
            int t = decode_slow(cell[c][k]);
            if (t >= 0 && !seen[t]) {
                seen[t] = 1;
                work[nw++] = t;
            }
        }
    }
}

static void gc(void) {
    unsigned char precise[NCELL], conservative[NCELL];
    int nprecise = precise_reach(precise);
    conservative_oracle(conservative);
    conservative_mark();
    int retained = 0;
    for (int c = 0; c < NCELL; c++) {
        CHECK((mark[c] != 0) == (conservative[c] != 0));
        CHECK(!precise[c] || mark[c]); /* nothing truly live is ever lost */
        if (used[c] && !mark[c]) {
            used[c] = 0;
            freed++;
        }
        retained += mark[c];
        mark[c] = 0;
    }
    int fk = retained - nprecise;
    false_kept_total += fk;
    if (fk > false_kept_max)
        false_kept_max = fk;
    rounds++;
}

static int alloc_cell(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        for (int c = 0; c < NCELL; c++)
            if (!used[c]) {
                used[c] = 1;
                for (int k = 0; k < 4; k++)
                    cell[c][k] = 0;
                return c;
            }
        gc();
    }
    fprintf(stderr, "out of cells\n");
    exit(1);
}

static void push(uint32_t w, int is_true) {
    CHECK(sp < SD);
    stack[sp] = w;
    truth[sp] = (unsigned char)is_true;
    sp++;
    if (sp > max_sp)
        max_sp = sp;
}

static uint32_t noise_word(void) {
    unsigned k = rnd_n(10);
    if (k < 3)
        return rnd_n(1000);
    if (k < 4)
        return rnd();
    return BASE + rnd_n(NCELL * CELLB + 200); /* looks like a heap address */
}

int main(void) {
    for (int round = 0; round < 90; round++) {
        if (sp > SD - 24)
            sp -= 40;
        int pops = sp ? (int)rnd_n((unsigned)(sp < 10 ? sp : 10) + 1) : 0;
        sp -= pops; /* popped words stay in the array, like uninitialised frame slots */
        unsigned char seen[NCELL];
        precise_reach(seen);
        int list[NCELL], nl = 0;
        for (int c = 0; c < NCELL; c++)
            if (seen[c])
                list[nl++] = c;
        for (int n = 0; n < 6; n++) {
            unsigned k = rnd_n(10);
            if (k < 3 && nl > 0) {
                int c = list[rnd_n((unsigned)nl)];
                uint32_t off = rnd_n(3) == 0 ? 4u * rnd_n(4) : 0u; /* sometimes an interior pointer */
                push(addr_of(c) + off, 1);
            } else if (k < 6) {
                int c = alloc_cell();
                if (nl > 0) {
                    cell[c][0] = addr_of(list[rnd_n((unsigned)nl)]);
                    if (rnd_n(2) == 0)
                        cell[c][1] = addr_of(list[rnd_n((unsigned)nl)]);
                }
                cell[c][2] = noise_word(); /* data that may alias a heap address */
                cell[c][3] = rnd_n(3) == 0 ? noise_word() : rnd_n(100);
                push(addr_of(c), 1);
                list[nl++] = c;
            } else if (k < 8) {
                push(noise_word(), 0);
            } else {
                sp += 3; /* frame gap: slots keep whatever was there before */
                CHECK(sp <= SD);
                for (int g = sp - 3; g < sp; g++)
                    truth[g] = 0;
            }
        }
        if (round % 3 == 2)
            gc();
    }
    gc();
    int live = 0;
    for (int c = 0; c < NCELL; c++)
        live += used[c];
    printf("collections: %d, cells freed: %d, cells in use at end: %d\n", rounds, freed, live);
    printf("ambiguous words examined: %d, max stack depth: %d\n", ambiguous_words, max_sp);
    printf("interior pointers recognised: %d, words hitting free cells: %d\n", interior_hits, ignored_free);
    printf("false retention: %d cells total, %d worst collection\n", false_kept_total, false_kept_max);
    return 0;
}
