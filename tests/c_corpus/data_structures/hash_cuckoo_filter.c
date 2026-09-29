/*
 * title: Cuckoo filter with partial-key cuckoo hashing
 * topic: data_structures
 * covers: cuckoo filter, fingerprints, alternate bucket via xor, deletion, eviction kicks, achievable load factor
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
enum { BSZ = 4, MAXKICK = 500 };
typedef struct {
    uint16_t *fp; /* 0 = empty */
    size_t nb, count;
    int fbits;
    long kicks;
} CF;

static void cf_init(CF *f, size_t nb, int fbits) {
    f->fp = calloc(nb * BSZ, sizeof(uint16_t));
    f->nb = nb;
    f->count = 0;
    f->fbits = fbits;
    f->kicks = 0;
}

static uint16_t fprint(const CF *f, uint64_t h) {
    uint16_t x = (uint16_t)((h >> 40) & ((1u << f->fbits) - 1));
    return x ? x : 1;
}
static size_t alt(const CF *f, size_t i, uint16_t fp) { return (i ^ (size_t)(mix32(fp) & (uint32_t)(f->nb - 1))) & (f->nb - 1); }

static int bucket_put(CF *f, size_t b, uint16_t fp) {
    for (int s = 0; s < BSZ; s++)
        if (!f->fp[b * BSZ + (size_t)s]) {
            f->fp[b * BSZ + (size_t)s] = fp;
            return 1;
        }
    return 0;
}

static int cf_insert(CF *f, uint64_t key) {
    uint64_t h = mix64(key);
    uint16_t fp = fprint(f, h);
    size_t i1 = (size_t)(h & (f->nb - 1)), i2 = alt(f, i1, fp);
    if (bucket_put(f, i1, fp) || bucket_put(f, i2, fp)) {
        f->count++;
        return 1;
    }
    size_t i = (rnd() & 1) ? i1 : i2;
    for (int n = 0; n < MAXKICK; n++) {
        size_t s = (size_t)(rnd() % BSZ);
        uint16_t tmp = f->fp[i * BSZ + s];
        f->fp[i * BSZ + s] = fp;
        fp = tmp;
        f->kicks++;
        i = alt(f, i, fp);
        if (bucket_put(f, i, fp)) {
            f->count++;
            return 1;
        }
    }
    /* the displaced fingerprint is lost: the caller must treat the filter as full */
    return 0;
}

static int cf_has(const CF *f, uint64_t key) {
    uint64_t h = mix64(key);
    uint16_t fp = fprint(f, h);
    size_t i1 = (size_t)(h & (f->nb - 1)), i2 = alt(f, i1, fp);
    for (int s = 0; s < BSZ; s++)
        if (f->fp[i1 * BSZ + (size_t)s] == fp || f->fp[i2 * BSZ + (size_t)s] == fp)
            return 1;
    return 0;
}

static int cf_del(CF *f, uint64_t key) {
    uint64_t h = mix64(key);
    uint16_t fp = fprint(f, h);
    size_t i1 = (size_t)(h & (f->nb - 1)), i2 = alt(f, i1, fp);
    size_t bs[2] = {i1, i2};
    for (int w = 0; w < 2; w++)
        for (int s = 0; s < BSZ; s++)
            if (f->fp[bs[w] * BSZ + (size_t)s] == fp) {
                f->fp[bs[w] * BSZ + (size_t)s] = 0;
                f->count--;
                return 1;
            }
    return 0;
}

int main(void) {
    static const int fb[3] = {8, 12, 16};
    for (int c = 0; c < 3; c++) {
        CF f;
        cf_init(&f, 1024, fb[c]);
        uint64_t inserted = 0;
        /* insert distinct keys until the first failure, which reveals the achievable load */
        while (cf_insert(&f, inserted))
            inserted++;
        /* the failing key displaced one fingerprint; undo by not counting it, then check the others */
        size_t cap = f.nb * BSZ;
        long fneg = 0;
        for (uint64_t k = 0; k < inserted; k++)
            fneg += !cf_has(&f, k);
        check(fneg <= 1, "at most one victim of the failed insert");
        long fp = 0, Q = 50000;
        for (long q = 0; q < Q; q++)
            fp += cf_has(&f, 1000000 + (uint64_t)q);
        printf("fingerprint bits=%2d first failure at %llu items load=%.4f kicks=%ld fpr=%.4f\n", fb[c], (unsigned long long)inserted,
               (double)f.count / (double)cap, f.kicks, (double)fp / (double)Q);
        free(f.fp);
    }
    /* mixed workload with deletes, checked against an exact multiset of keys */
    CF f;
    cf_init(&f, 2048, 12);
    enum { U = 4000 };
    static uint8_t present[U];
    size_t live = 0;
    long ins = 0, del = 0, fneg = 0;
    for (int step = 0; step < 40000; step++) {
        int k = (int)(rnd() % U);
        if (rnd() % 2 && !present[k] && live < f.nb * BSZ * 85 / 100) {
            check(cf_insert(&f, (uint64_t)k), "insert below 85 percent load");
            present[k] = 1;
            live++;
            ins++;
        } else if (present[k] && rnd() % 2) {
            check(cf_del(&f, (uint64_t)k), "delete present");
            present[k] = 0;
            live--;
            del++;
        }
        if (step % 1000 == 0)
            for (int j = 0; j < U; j++)
                if (present[j] && !cf_has(&f, (uint64_t)j))
                    fneg++;
    }
    check(fneg == 0, "no false negatives (12-bit aliasing is negligible here)");
    check(f.count == live, "count tracks live keys");
    printf("mixed: inserts=%ld deletes=%ld live=%zu load=%.4f kicks=%ld\n", ins, del, live, (double)live / (double)(f.nb * BSZ), f.kicks);
    free(f.fp);
    return 0;
}
