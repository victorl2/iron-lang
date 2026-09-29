/*
 * title: Odd-only bit-packed sieve
 * topic: algorithms
 * covers: bitset sieve, odd-only indexing, popcount, twin primes, Goldbach partitions
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LIMIT 2000000u

static uint32_t *bits; /* bit i set => 2*i+1 is composite */

static int composite_odd(unsigned n) { return (bits[(n >> 1) >> 5] >> ((n >> 1) & 31)) & 1u; }
static int is_prime(unsigned n) {
    if (n < 2) return 0;
    if (n == 2) return 1;
    if (!(n & 1)) return 0;
    return !composite_odd(n);
}

static int popcount32(uint32_t x) {
    int c = 0;
    while (x) { x &= x - 1; c++; }
    return c;
}

int main(void) {
    size_t nbits = (LIMIT + 1) / 2;
    size_t nwords = (nbits + 31) / 32;
    bits = calloc(nwords, sizeof *bits);
    if (!bits) return 2;
    bits[0] |= 1u; /* 1 is not prime */
    for (unsigned p = 3; (uint64_t)p * p <= LIMIT; p += 2) {
        if (composite_odd(p)) continue;
        for (unsigned m = p * p; m <= LIMIT; m += 2 * p) {
            unsigned i = m >> 1;
            bits[i >> 5] |= 1u << (i & 31);
        }
    }
    /* count via popcount over words, masking the tail beyond LIMIT */
    unsigned long composites = 0;
    for (size_t w = 0; w < nwords; w++) {
        uint32_t v = bits[w];
        if (w == nwords - 1 && (nbits & 31)) v &= (1u << (nbits & 31)) - 1u;
        composites += (unsigned long)popcount32(v);
    }
    unsigned long odd_total = nbits;
    unsigned long primes = 1 + (odd_total - composites); /* +1 for the prime 2 */
    printf("pi(%u) = %lu\n", LIMIT, primes);

    unsigned long twins = 0;
    unsigned last_twin = 0;
    for (unsigned n = 3; n + 2 <= LIMIT; n += 2)
        if (is_prime(n) && is_prime(n + 2)) { twins++; last_twin = n; }
    printf("twin prime pairs: %lu, last (%u,%u)\n", twins, last_twin, last_twin + 2);

    unsigned long cnt = 0;
    for (unsigned n = 2; n <= LIMIT; n++) cnt += (unsigned long)is_prime(n);
    if (cnt != primes) { fprintf(stderr, "count mismatch\n"); return 1; }

    unsigned evens[] = {4, 100, 1000, 9998, 100000, 1999998};
    for (size_t t = 0; t < sizeof evens / sizeof evens[0]; t++) {
        unsigned e = evens[t], reps = 0, smallest = 0;
        for (unsigned p = 2; p <= e / 2; p++) {
            if (is_prime(p) && is_prime(e - p)) { if (!reps) smallest = p; reps++; }
        }
        if (!reps) { fprintf(stderr, "Goldbach counterexample?!\n"); return 1; }
        printf("Goldbach %u: %u partitions, smallest %u + %u\n", e, reps, smallest, e - smallest);
    }
    printf("memory words: %zu\n", nwords);
    free(bits);
    return 0;
}
