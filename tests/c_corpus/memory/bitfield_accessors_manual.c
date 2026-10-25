/*
 * title: Manual bit-field packing with mask and shift accessors
 * topic: memory
 * covers: bit layout descriptors, field accessors, overflow rejection, packed 64-bit words
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * A 64-bit descriptor word with an explicit, compiler-independent layout
 * (compiler bit-fields leave the order implementation-defined):
 *   bits  0..4   kind      (5)
 *   bits  5..6   priority  (2)
 *   bit   7      urgent    (1)
 *   bits  8..19  length    (12)
 *   bits 20..35  owner     (16)
 *   bits 36..59  offset    (24)
 *   bits 60..63  version   (4)
 */
typedef struct {
    const char *name;
    unsigned shift;
    unsigned width;
} FieldDef;

static const FieldDef defs[] = {
    {"kind", 0, 5}, {"priority", 5, 2}, {"urgent", 7, 1}, {"length", 8, 12},
    {"owner", 20, 16}, {"offset", 36, 24}, {"version", 60, 4},
};
enum { NF = sizeof defs / sizeof defs[0] };

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint64_t mask_of(unsigned width) {
    return width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
}

static uint64_t field_get(uint64_t w, int f) {
    return (w >> defs[f].shift) & mask_of(defs[f].width);
}

/* returns 0 when the value does not fit, leaving the word unchanged */
static int field_set(uint64_t *w, int f, uint64_t v) {
    uint64_t m = mask_of(defs[f].width);
    if (v > m)
        return 0;
    *w = (*w & ~(m << defs[f].shift)) | (v << defs[f].shift);
    return 1;
}

/* verify the descriptor table tiles the word with no overlaps */
static uint64_t coverage(void) {
    uint64_t seen = 0;
    for (int i = 0; i < NF; i++) {
        uint64_t m = mask_of(defs[i].width) << defs[i].shift;
        check((seen & m) == 0, "fields overlap");
        seen |= m;
    }
    return seen;
}

static uint64_t s = 31337;

static uint64_t rnd(void) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* second implementation: build a word by direct arithmetic and compare */
static uint64_t build_direct(const uint64_t v[NF]) {
    return v[0] + v[1] * 32u + v[2] * 128u + v[3] * 256u + v[4] * ((uint64_t)1 << 20) + v[5] * ((uint64_t)1 << 36) +
           v[6] * ((uint64_t)1 << 60);
}

int main(void) {
    uint64_t cov = coverage();
    printf("coverage mask: 0x%016llx\n", (unsigned long long)cov);
    check(cov == ~(uint64_t)0, "tiles all 64 bits");

    uint64_t w = 0;
    uint64_t vals[NF] = {17, 2, 1, 1500, 40000, 123456, 9};
    for (int i = 0; i < NF; i++)
        check(field_set(&w, i, vals[i]), "set");
    printf("packed word: 0x%016llx\n", (unsigned long long)w);
    check(w == build_direct(vals), "matches direct construction");
    for (int i = 0; i < NF; i++) {
        printf("  %-8s shift %2u width %2u = %llu\n", defs[i].name, defs[i].shift, defs[i].width,
               (unsigned long long)field_get(w, i));
        check(field_get(w, i) == vals[i], "get after set");
    }

    /* independence: updating one field never disturbs the others */
    unsigned long ops = 0;
    for (int trial = 0; trial < 5000; trial++) {
        uint64_t cur[NF];
        uint64_t word = 0;
        for (int i = 0; i < NF; i++) {
            cur[i] = rnd() & mask_of(defs[i].width);
            check(field_set(&word, i, cur[i]), "set random");
        }
        int f = (int)(rnd() % NF);
        uint64_t nv = rnd() & mask_of(defs[f].width);
        check(field_set(&word, f, nv), "update one");
        cur[f] = nv;
        for (int i = 0; i < NF; i++)
            check(field_get(word, i) == cur[i], "isolation");
        check(word == build_direct(cur), "direct build agrees");
        ops++;
    }
    printf("random isolation trials: %lu\n", ops);

    /* overflow rejection leaves the word untouched */
    uint64_t before = w;
    int rejected = 0;
    for (int i = 0; i < NF; i++) {
        uint64_t too_big = mask_of(defs[i].width) + 1;
        if (!field_set(&w, i, too_big))
            rejected++;
        check(w == before, "unchanged after reject");
    }
    printf("oversized values rejected: %d of %d\n", rejected, NF);

    /* saturated and cleared words */
    uint64_t full = 0;
    for (int i = 0; i < NF; i++)
        field_set(&full, i, mask_of(defs[i].width));
    printf("all fields max: 0x%016llx\n", (unsigned long long)full);
    check(full == ~(uint64_t)0, "all ones");
    for (int i = 0; i < NF; i += 2)
        field_set(&full, i, 0);
    printf("even fields cleared: 0x%016llx\n", (unsigned long long)full);

    /* 3-bit fields straddling a byte boundary in an array of packed values */
    uint8_t buf[8] = {0};
    for (unsigned i = 0; i < 21; i++) {
        unsigned bit = i * 3, byte = bit / 8, sh = bit % 8;
        unsigned v = (i * 5 + 3) & 7u;
        unsigned two = (unsigned)buf[byte] | ((byte + 1 < 8 ? (unsigned)buf[byte + 1] : 0u) << 8);
        two &= ~(7u << sh);
        two |= v << sh;
        buf[byte] = (uint8_t)two;
        if (byte + 1 < 8)
            buf[byte + 1] = (uint8_t)(two >> 8);
    }
    unsigned bad = 0;
    printf("3-bit values:");
    for (unsigned i = 0; i < 21; i++) {
        unsigned bit = i * 3, byte = bit / 8, sh = bit % 8;
        unsigned two = (unsigned)buf[byte] | ((byte + 1 < 8 ? (unsigned)buf[byte + 1] : 0u) << 8);
        unsigned v = (two >> sh) & 7u;
        if (v != ((i * 5 + 3) & 7u))
            bad++;
        printf(" %u", v);
    }
    printf("\nmismatches: %u\n", bad);
    check(bad == 0, "straddling fields");
    return 0;
}
