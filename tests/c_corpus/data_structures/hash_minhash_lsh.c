/*
 * title: MinHash signatures and LSH banding for Jaccard similarity
 * topic: data_structures
 * covers: minhash, universal hash permutations, Jaccard estimate, locality sensitive hashing bands, candidate pair detection
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
#define PRIME 2147483647ull /* 2^31 - 1 */
enum { UNIV = 4000, K = 128, DOCS = 24, BANDS = 32, ROWS = 4 };

static uint64_t ha[K], hb[K];

static void mh_setup(void) {
    for (int i = 0; i < K; i++) {
        ha[i] = 1 + rnd() % (PRIME - 1);
        hb[i] = rnd() % PRIME;
    }
}

static void signature(const uint8_t *set, uint32_t *sig) {
    for (int i = 0; i < K; i++)
        sig[i] = UINT32_MAX;
    for (uint32_t x = 0; x < UNIV; x++) {
        if (!set[x])
            continue;
        for (int i = 0; i < K; i++) {
            uint32_t h = (uint32_t)((ha[i] * x + hb[i]) % PRIME);
            if (h < sig[i])
                sig[i] = h;
        }
    }
}

static double exact_jaccard(const uint8_t *a, const uint8_t *b, int *inter, int *uni) {
    int i = 0, u = 0;
    for (int x = 0; x < UNIV; x++) {
        i += a[x] && b[x];
        u += a[x] || b[x];
    }
    *inter = i;
    *uni = u;
    return u ? (double)i / u : 0.0;
}

static uint64_t band_hash(const uint32_t *sig, int band) {
    uint64_t h = 0xcbf29ce484222325ull + (uint64_t)band;
    for (int r = 0; r < ROWS; r++)
        h = mix64(h ^ sig[band * ROWS + r]);
    return h;
}

int main(void) {
    mh_setup();
    static uint8_t docs[DOCS][UNIV];
    static uint32_t sig[DOCS][K];
    /* build 6 families of 4 near-duplicate documents: a base set plus small mutations */
    for (int f = 0; f < 6; f++) {
        for (int x = 0; x < UNIV; x++)
            docs[f * 4][x] = (uint8_t)(rnd() % 100 < 8);
        for (int v = 1; v < 4; v++) {
            memcpy(docs[f * 4 + v], docs[f * 4], UNIV);
            int flips = 20 * v;
            for (int i = 0; i < flips; i++) {
                int x = (int)(rnd() % UNIV);
                docs[f * 4 + v][x] ^= 1;
            }
        }
    }
    for (int d = 0; d < DOCS; d++)
        signature(docs[d], sig[d]);

    /* estimate error across all pairs */
    double sumerr = 0, maxerr = 0;
    int pairs = 0;
    for (int i = 0; i < DOCS; i++)
        for (int j = i + 1; j < DOCS; j++) {
            int inter, uni;
            double ex = exact_jaccard(docs[i], docs[j], &inter, &uni);
            int same = 0;
            for (int k = 0; k < K; k++)
                same += sig[i][k] == sig[j][k];
            double est = (double)same / K;
            double e = est > ex ? est - ex : ex - est;
            sumerr += e;
            if (e > maxerr)
                maxerr = e;
            pairs++;
        }
    printf("pairs=%d mean abs error=%.4f max abs error=%.4f (k=%d)\n", pairs, sumerr / pairs, maxerr, K);
    check(maxerr < 0.2, "estimates close to exact");

    /* show a few within-family similarities */
    for (int v = 1; v < 4; v++) {
        int inter, uni;
        double ex = exact_jaccard(docs[0], docs[v], &inter, &uni);
        int same = 0;
        for (int k = 0; k < K; k++)
            same += sig[0][k] == sig[v][k];
        printf("doc0 vs doc%d: |inter|=%d |union|=%d exact=%.4f estimate=%.4f\n", v, inter, uni, ex, (double)same / K);
    }

    /* LSH: pairs colliding in at least one band become candidates */
    int cand = 0, true_family = 0, false_cand = 0, missed = 0;
    for (int i = 0; i < DOCS; i++)
        for (int j = i + 1; j < DOCS; j++) {
            int hit = 0;
            for (int b = 0; b < BANDS && !hit; b++)
                hit = band_hash(sig[i], b) == band_hash(sig[j], b);
            int fam = i / 4 == j / 4;
            cand += hit;
            true_family += hit && fam;
            false_cand += hit && !fam;
            missed += !hit && fam;
        }
    printf("LSH bands=%d rows=%d candidates=%d family hits=%d cross-family=%d missed family pairs=%d\n", BANDS, ROWS, cand, true_family,
           false_cand, missed);
    check(false_cand == 0, "unrelated documents do not collide");
    /* S-curve: probability a pair with Jaccard s becomes a candidate */
    printf("S-curve 1-(1-s^r)^b:");
    for (int s10 = 1; s10 <= 9; s10 += 2) {
        double s = s10 / 10.0, sr = 1.0;
        for (int r = 0; r < ROWS; r++)
            sr *= s;
        double miss = 1.0;
        for (int b = 0; b < BANDS; b++)
            miss *= 1.0 - sr;
        printf(" s=%.1f:%.4f", s, 1.0 - miss);
    }
    printf("\n");
    return 0;
}
