/*
 * title: XOR tricks for missing, duplicate and unique elements
 * topic: algorithms
 * covers: xor cancellation, lowest-bit partitioning, mod-3 bit counters, xor linear basis, maximum subset xor, swap without temporary
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t st = 0x1F2E3D4Cu;

static uint32_t rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void shuffle(uint32_t *a, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        uint32_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

/* one value appears once, the rest twice */
static uint32_t single_of_pairs(const uint32_t *a, int n) {
    uint32_t x = 0;
    for (int i = 0; i < n; i++)
        x ^= a[i];
    return x;
}

/* two values appear once, the rest twice: split the array on any bit where they differ */
static void two_singles(const uint32_t *a, int n, uint32_t *p, uint32_t *q) {
    uint32_t x = 0;
    for (int i = 0; i < n; i++)
        x ^= a[i];
    uint32_t low = x & (0u - x);
    *p = *q = 0;
    for (int i = 0; i < n; i++)
        if (a[i] & low)
            *p ^= a[i];
        else
            *q ^= a[i];
    if (*p > *q) {
        uint32_t t = *p;
        *p = *q;
        *q = t;
    }
}

/* one value appears once, the rest three times: two-bit counter per bit position */
static uint32_t single_of_triples(const uint32_t *a, int n) {
    uint32_t ones = 0, twos = 0;
    for (int i = 0; i < n; i++) {
        ones = (ones ^ a[i]) & ~twos;
        twos = (twos ^ a[i]) & ~ones;
    }
    return ones;
}

/* array holds 1..n with one value missing and one duplicated */
static void missing_and_duplicate(const uint32_t *a, int n, uint32_t *missing, uint32_t *dup) {
    uint32_t x = 0;
    for (int i = 0; i < n; i++)
        x ^= a[i] ^ (uint32_t)(i + 1);
    uint32_t low = x & (0u - x);
    uint32_t g1 = 0, g2 = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] & low)
            g1 ^= a[i];
        else
            g2 ^= a[i];
        if ((uint32_t)(i + 1) & low)
            g1 ^= (uint32_t)(i + 1);
        else
            g2 ^= (uint32_t)(i + 1);
    }
    /* g1 and g2 are {missing, dup} in some order: the one present in the array is the duplicate */
    int g1_in = 0;
    for (int i = 0; i < n; i++)
        if (a[i] == g1)
            g1_in = 1;
    *dup = g1_in ? g1 : g2;
    *missing = g1_in ? g2 : g1;
}

/* Linear basis over GF(2): insert values, then greedily maximise the xor of a subset. */
typedef struct {
    uint32_t b[32];
    int rank;
} Basis;

static void basis_insert(Basis *bs, uint32_t v) {
    for (int bit = 31; bit >= 0 && v; bit--) {
        if (!((v >> bit) & 1u))
            continue;
        if (!bs->b[bit]) {
            bs->b[bit] = v;
            bs->rank++;
            return;
        }
        v ^= bs->b[bit];
    }
}

static uint32_t basis_max_xor(const Basis *bs) {
    uint32_t r = 0;
    for (int bit = 31; bit >= 0; bit--)
        if (bs->b[bit] && (r ^ bs->b[bit]) > r)
            r ^= bs->b[bit];
    return r;
}

static uint32_t brute_max_subset_xor(const uint32_t *a, int n) {
    uint32_t best = 0;
    for (uint32_t m = 0; m < (1u << n); m++) {
        uint32_t x = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1u)
                x ^= a[i];
        if (x > best)
            best = x;
    }
    return best;
}

int main(void) {
    uint32_t a[401];
    long checksum = 0;
    for (int t = 0; t < 200; t++) {
        int pairs = 1 + (int)(rnd() % 100);
        int n = 0;
        uint32_t target = rnd() % 100000;
        for (int i = 0; i < pairs; i++) {
            uint32_t v = 100000 + (uint32_t)i * 7 + (rnd() % 5);
            a[n++] = v;
            a[n++] = v;
        }
        a[n++] = target;
        shuffle(a, n);
        check(single_of_pairs(a, n) == target, "single among pairs");
        checksum += target;
    }
    printf("single-of-pairs checksum %ld\n", checksum);
    for (int t = 0; t < 200; t++) {
        int pairs = 1 + (int)(rnd() % 60), n = 0;
        uint32_t p = rnd() % 50000, q = 50000 + rnd() % 50000;
        for (int i = 0; i < pairs; i++) {
            uint32_t v = 200000 + (uint32_t)i * 13 + (rnd() % 11);
            a[n++] = v;
            a[n++] = v;
        }
        a[n++] = p;
        a[n++] = q;
        shuffle(a, n);
        uint32_t gp, gq;
        two_singles(a, n, &gp, &gq);
        check(gp == p && gq == q, "two singles");
    }
    printf("two singles: verified on 200 arrays\n");
    for (int t = 0; t < 200; t++) {
        int triples = 1 + (int)(rnd() % 50), n = 0;
        uint32_t target = rnd();
        for (int i = 0; i < triples; i++) {
            uint32_t v = rnd() | 1u;
            for (int k = 0; k < 3; k++)
                a[n++] = v;
        }
        a[n++] = target;
        shuffle(a, n);
        /* repeated random values could collide with the target; then it is not a valid instance */
        int cnt = 0;
        for (int i = 0; i < n; i++)
            cnt += a[i] == target;
        if (cnt != 1)
            continue;
        check(single_of_triples(a, n) == target, "single among triples");
    }
    printf("single-of-triples: verified\n");
    for (int t = 0; t < 200; t++) {
        int n = 2 + (int)(rnd() % 90);
        uint32_t perm[100];
        for (int i = 0; i < n; i++)
            perm[i] = (uint32_t)(i + 1);
        shuffle(perm, n);
        uint32_t missing = perm[0], dup = perm[1];
        perm[0] = dup; /* replace the missing value by a copy of another */
        uint32_t m, d;
        missing_and_duplicate(perm, n, &m, &d);
        check(m == missing && d == dup, "missing and duplicate");
    }
    printf("missing/duplicate pair: verified\n");
    uint32_t x = 0xDEADBEEFu, y = 0x12345678u;
    x ^= y;
    y ^= x;
    x ^= y;
    check(x == 0x12345678u && y == 0xDEADBEEFu, "xor swap");
    printf("xor swap: x=%08x y=%08x\n", x, y);
    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rnd() % 14);
        uint32_t v[16];
        Basis bs = {{0}, 0};
        for (int i = 0; i < n; i++) {
            v[i] = rnd();
            v[i] >>= rnd() % 24;
            basis_insert(&bs, v[i]);
        }
        check(basis_max_xor(&bs) == brute_max_subset_xor(v, n), "max subset xor");
        checksum += bs.rank;
    }
    printf("xor basis: max subset xor matches brute force, rank checksum %ld\n", checksum);
    Basis fixed = {{0}, 0};
    uint32_t vs[] = {3, 10, 5, 25, 2, 8};
    for (int i = 0; i < 6; i++)
        basis_insert(&fixed, vs[i]);
    printf("fixed set {3,10,5,25,2,8}: rank=%d max subset xor=%u\n", fixed.rank, basis_max_xor(&fixed));
    check(basis_max_xor(&fixed) == 31, "known maximum subset xor");
    return 0;
}
