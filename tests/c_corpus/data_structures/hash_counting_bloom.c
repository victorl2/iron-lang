/*
 * title: Counting Bloom filter with 4-bit saturating counters
 * topic: data_structures
 * covers: counting bloom filter, packed nibble counters, deletion, saturation, multiplicity upper bound, false negatives after unsafe delete
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
enum { K = 4 };
typedef struct {
    uint8_t *nib; /* two counters per byte */
    size_t m;
    long saturated_incs;
} CBF;

static unsigned get_c(const CBF *f, size_t i) { return (f->nib[i >> 1] >> ((i & 1) * 4)) & 15u; }
static void set_c(CBF *f, size_t i, unsigned v) {
    unsigned sh = (unsigned)((i & 1) * 4);
    f->nib[i >> 1] = (uint8_t)((f->nib[i >> 1] & ~(15u << sh)) | (v << sh));
}

static void idx(const CBF *f, uint64_t key, size_t *p) {
    uint64_t h = mix64(key);
    for (int i = 0; i < K; i++)
        p[i] = (size_t)(((h & 0xffffffffu) + (uint64_t)i * ((h >> 32) | 1u)) % f->m);
}

static void cbf_add(CBF *f, uint64_t key) {
    size_t p[K];
    idx(f, key, p);
    for (int i = 0; i < K; i++) {
        unsigned c = get_c(f, p[i]);
        if (c == 15)
            f->saturated_incs++; /* stays at 15 forever: never decremented safely */
        else
            set_c(f, p[i], c + 1);
    }
}

static int cbf_del(CBF *f, uint64_t key) { /* returns 0 if definitely absent */
    size_t p[K];
    idx(f, key, p);
    for (int i = 0; i < K; i++)
        if (get_c(f, p[i]) == 0)
            return 0;
    for (int i = 0; i < K; i++) {
        unsigned c = get_c(f, p[i]);
        if (c < 15) /* saturated counters are sticky */
            set_c(f, p[i], c - 1);
    }
    return 1;
}

static int cbf_maybe(const CBF *f, uint64_t key) {
    size_t p[K];
    idx(f, key, p);
    for (int i = 0; i < K; i++)
        if (get_c(f, p[i]) == 0)
            return 0;
    return 1;
}

static unsigned cbf_count_ub(const CBF *f, uint64_t key) {
    size_t p[K];
    idx(f, key, p);
    unsigned mn = 15;
    for (int i = 0; i < K; i++)
        if (get_c(f, p[i]) < mn)
            mn = get_c(f, p[i]);
    return mn;
}

int main(void) {
    enum { UNIVERSE = 3000, M = 30000 };
    CBF f;
    f.m = M;
    f.nib = calloc(M / 2, 1);
    f.saturated_incs = 0;
    static int refc[UNIVERSE]; /* exact multiset counts */
    long adds = 0, dels = 0, refused = 0, present_checks = 0, over = 0;
    for (int step = 0; step < 60000; step++) {
        int k = (int)(rnd() % UNIVERSE);
        unsigned op = (unsigned)(rnd() % 10);
        if (op < 5) {
            cbf_add(&f, (uint64_t)k);
            refc[k]++;
            adds++;
        } else if (op < 8) {
            if (refc[k] > 0) { /* only delete what was inserted: safe usage */
                check(cbf_del(&f, (uint64_t)k), "delete of present key succeeds");
                refc[k]--;
                dels++;
            } else {
                refused++;
            }
        } else {
            present_checks++;
            if (refc[k] > 0)
                check(cbf_maybe(&f, (uint64_t)k), "no false negative under safe deletes");
        }
    }
    long live = 0, fp = 0, ub_ok = 0, ub_over = 0;
    for (int k = 0; k < UNIVERSE; k++) {
        if (refc[k] > 0) {
            live++;
            unsigned ub = cbf_count_ub(&f, (uint64_t)k);
            check(ub >= (unsigned)(refc[k] > 15 ? 15 : refc[k]), "count estimate is an upper bound (capped)");
            ub_ok++;
            ub_over += ub > (unsigned)refc[k];
        } else
            fp += cbf_maybe(&f, (uint64_t)k);
    }
    over = ub_over;
    long total_items = 0, nonzero = 0, maxc = 0;
    for (int k = 0; k < UNIVERSE; k++)
        total_items += refc[k];
    for (size_t i = 0; i < f.m; i++) {
        nonzero += get_c(&f, i) != 0;
        if ((long)get_c(&f, i) > maxc)
            maxc = get_c(&f, i);
    }
    printf("adds=%ld deletes=%ld refused (absent)=%ld membership checks=%ld\n", adds, dels, refused, present_checks);
    printf("live keys=%ld total multiplicity=%ld saturated increments=%ld\n", live, total_items, f.saturated_incs);
    printf("counters nonzero=%ld/%zu max counter=%ld false positives among %d absent=%ld\n", nonzero, f.m, maxc, UNIVERSE - (int)live,
           fp);
    printf("count estimates exact for %ld of %ld live keys (over-estimated: %ld)\n", ub_ok - over, ub_ok, over);
    /* unsafe delete demonstration: deleting a never-inserted key can cause a false negative */
    CBF g;
    g.m = 64;
    g.nib = calloc(32, 1);
    g.saturated_incs = 0;
    for (uint64_t k = 0; k < 12; k++)
        cbf_add(&g, k);
    long lost = 0;
    for (uint64_t k = 100; k < 400; k++)
        if (cbf_del(&g, k)) {
            for (uint64_t j = 0; j < 12; j++)
                if (!cbf_maybe(&g, j)) {
                    lost++;
                    break;
                }
        }
    printf("unsafe deletes of absent keys that broke a member: %ld\n", lost);
    free(g.nib);
    free(f.nib);
    return 0;
}
