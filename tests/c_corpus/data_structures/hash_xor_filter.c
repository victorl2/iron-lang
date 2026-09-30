/*
 * title: Xor filter built by peeling a 3-hypergraph
 * topic: data_structures
 * covers: xor filter, static approximate membership, hypergraph peeling, construction retries with new seeds, 8-bit fingerprints
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
typedef struct {
    uint64_t seed;
    uint32_t block, size; /* size = 3 * block */
    uint8_t *fp;
    int tries;
} Xor8;

static uint32_t reduce32(uint32_t x, uint32_t n) { return (uint32_t)(((uint64_t)x * n) >> 32); }
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint8_t fingerprint(uint64_t h) { return (uint8_t)((h ^ (h >> 32)) & 0xff); }

static void positions(const Xor8 *f, uint64_t key, uint32_t *p, uint64_t *hh) {
    uint64_t h = mix64(key + f->seed);
    p[0] = reduce32((uint32_t)h, f->block);
    p[1] = reduce32((uint32_t)rotl64(h, 21), f->block) + f->block;
    p[2] = reduce32((uint32_t)rotl64(h, 42), f->block) + 2 * f->block;
    *hh = h;
}

static int xor8_build(Xor8 *f, const uint64_t *keys, uint32_t n) {
    f->block = (uint32_t)(1.23 * n + 32) / 3 + 1;
    f->size = 3 * f->block;
    f->fp = calloc(f->size, 1);
    f->tries = 0;
    uint8_t *count = malloc(f->size);
    uint64_t *xh = malloc(f->size * sizeof(uint64_t)); /* xor of the key hashes seeded in each slot */
    uint64_t *xk = malloc(f->size * sizeof(uint64_t)); /* xor of the keys themselves */
    uint32_t *queue = malloc(f->size * sizeof(uint32_t));
    uint64_t *stack_key = malloc(n * sizeof(uint64_t));
    uint32_t *stack_slot = malloc(n * sizeof(uint32_t));
    int ok = 0;
    for (uint64_t seed = 1; !ok && seed < 200; seed++) {
        f->seed = seed * 0x9E3779B97F4A7C15ull;
        f->tries++;
        memset(count, 0, f->size);
        memset(xh, 0, f->size * sizeof(uint64_t));
        memset(xk, 0, f->size * sizeof(uint64_t));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p[3];
            uint64_t h;
            positions(f, keys[i], p, &h);
            for (int j = 0; j < 3; j++) {
                count[p[j]]++;
                xh[p[j]] ^= h;
                xk[p[j]] ^= keys[i];
            }
        }
        uint32_t qh = 0, qt = 0, sp = 0;
        for (uint32_t s = 0; s < f->size; s++)
            if (count[s] == 1)
                queue[qt++] = s;
        while (qh < qt) {
            uint32_t s = queue[qh++];
            if (count[s] != 1)
                continue;
            uint64_t key = xk[s];
            uint32_t p[3];
            uint64_t h;
            positions(f, key, p, &h);
            stack_key[sp] = key;
            stack_slot[sp++] = s;
            for (int j = 0; j < 3; j++) {
                count[p[j]]--;
                xh[p[j]] ^= h;
                xk[p[j]] ^= key;
                if (p[j] != s && count[p[j]] == 1)
                    queue[qt++] = p[j];
            }
        }
        if (sp == n) {
            ok = 1;
            memset(f->fp, 0, f->size);
            while (sp > 0) { /* assign in reverse peeling order */
                sp--;
                uint32_t p[3];
                uint64_t h;
                positions(f, stack_key[sp], p, &h);
                f->fp[stack_slot[sp]] = fingerprint(h) ^ f->fp[p[0]] ^ f->fp[p[1]] ^ f->fp[p[2]];
            }
        }
    }
    free(count);
    free(xh);
    free(xk);
    free(queue);
    free(stack_key);
    free(stack_slot);
    return ok;
}

static int xor8_contains(const Xor8 *f, uint64_t key) {
    uint32_t p[3];
    uint64_t h;
    positions(f, key, p, &h);
    return fingerprint(h) == (f->fp[p[0]] ^ f->fp[p[1]] ^ f->fp[p[2]]);
}

int main(void) {
    static const uint32_t sizes[4] = {10, 200, 3000, 40000};
    for (int t = 0; t < 4; t++) {
        uint32_t n = sizes[t];
        uint64_t *keys = malloc(n * sizeof(uint64_t));
        for (uint32_t i = 0; i < n; i++)
            keys[i] = ((uint64_t)i << 1 | 1) * 1000003ull; /* distinct odd multiples */
        Xor8 f;
        check(xor8_build(&f, keys, n), "construction succeeds");
        for (uint32_t i = 0; i < n; i++)
            check(xor8_contains(&f, keys[i]), "no false negatives");
        long fp = 0, Q = 100000;
        for (long q = 0; q < Q; q++)
            fp += xor8_contains(&f, ((uint64_t)q << 1) * 1000003ull + 2000000000ull); /* never a member: even, offset */
        printf("n=%5u slots=%6u (%.3f per key, %.2f bits/key) seeds tried=%d fpr=%.5f (ideal %.5f)\n", n, f.size, (double)f.size / n,
               8.0 * f.size / n, f.tries, (double)fp / (double)Q, 1.0 / 256);
        check((double)fp / (double)Q < 0.012, "fpr near 1/256");
        free(f.fp);
        free(keys);
    }
    return 0;
}
