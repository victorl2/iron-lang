/*
 * title: Count-Min sketch with conservative update and heavy hitters
 * topic: data_structures
 * covers: count-min sketch, pairwise independent rows, conservative update, overestimate guarantee, top-k heavy hitters
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
enum { D = 4, W = 256, U = 2000 };
typedef struct {
    uint32_t c[D][W];
    uint64_t seed[D];
    uint64_t total;
    int conservative;
} CMS;

static void cms_init(CMS *s, int conservative) {
    memset(s->c, 0, sizeof s->c);
    for (int i = 0; i < D; i++)
        s->seed[i] = mix64(0xC0FFEEull + (uint64_t)i * 7919u);
    s->total = 0;
    s->conservative = conservative;
}
static size_t col(const CMS *s, int row, uint32_t key) { return (size_t)(mix64(key ^ s->seed[row]) % W); }

static void cms_add(CMS *s, uint32_t key, uint32_t by) {
    s->total += by;
    if (!s->conservative) {
        for (int r = 0; r < D; r++)
            s->c[r][col(s, r, key)] += by;
        return;
    }
    uint32_t mn = UINT32_MAX;
    for (int r = 0; r < D; r++)
        if (s->c[r][col(s, r, key)] < mn)
            mn = s->c[r][col(s, r, key)];
    for (int r = 0; r < D; r++) {
        uint32_t *p = &s->c[r][col(s, r, key)];
        if (*p < mn + by)
            *p = mn + by; /* raise only the counters that are below the new minimum */
    }
}

static uint32_t cms_est(const CMS *s, uint32_t key) {
    uint32_t mn = UINT32_MAX;
    for (int r = 0; r < D; r++)
        if (s->c[r][col(s, r, key)] < mn)
            mn = s->c[r][col(s, r, key)];
    return mn;
}

typedef struct {
    uint32_t key, est;
} KE;
static int cmp_ke(const void *a, const void *b) {
    const KE *x = a, *y = b;
    if (x->est != y->est)
        return x->est > y->est ? -1 : 1;
    return x->key < y->key ? -1 : x->key > y->key;
}

int main(void) {
    static uint32_t exact[U];
    static CMS plain, cons;
    cms_init(&plain, 0);
    cms_init(&cons, 1);
    /* Zipf-like stream: key = floor(U / (1 + r*U)) style skew via product of two uniforms */
    for (int i = 0; i < 40000; i++) {
        uint32_t a = (uint32_t)(rnd() % U), b = (uint32_t)(rnd() % U);
        uint32_t key = (uint32_t)((uint64_t)a * b / U);
        exact[key]++;
        cms_add(&plain, key, 1);
        cms_add(&cons, key, 1);
    }
    check(plain.total == 40000 && cons.total == 40000, "totals");
    uint64_t err_p = 0, err_c = 0, max_p = 0, max_c = 0, exact_p = 0, exact_c = 0;
    long bound_viol = 0;
    double eps_n = 2.718281828 / W * 40000; /* e/w * N */
    for (uint32_t k = 0; k < U; k++) {
        uint32_t ep = cms_est(&plain, k), ec = cms_est(&cons, k);
        check(ep >= exact[k] && ec >= exact[k], "never underestimates");
        check(ec <= ep, "conservative update is never worse");
        uint64_t dp = ep - exact[k], dc = ec - exact[k];
        err_p += dp;
        err_c += dc;
        if (dp > max_p)
            max_p = dp;
        if (dc > max_c)
            max_c = dc;
        exact_p += dp == 0;
        exact_c += dc == 0;
        bound_viol += (double)dp > eps_n;
    }
    printf("stream=40000 universe=%d width=%d depth=%d\n", U, W, D);
    printf("plain:        mean overcount=%.4f max=%llu exact for %llu keys\n", (double)err_p / U, (unsigned long long)max_p,
           (unsigned long long)exact_p);
    printf("conservative: mean overcount=%.4f max=%llu exact for %llu keys\n", (double)err_c / U, (unsigned long long)max_c,
           (unsigned long long)exact_c);
    printf("keys with plain error above e*N/w=%.1f: %ld\n", eps_n, bound_viol);
    /* heavy hitters: top 8 by estimate vs top 8 by exact count */
    KE est[U], tru[U];
    for (uint32_t k = 0; k < U; k++) {
        est[k].key = k;
        est[k].est = cms_est(&cons, k);
        tru[k].key = k;
        tru[k].est = exact[k];
    }
    qsort(est, U, sizeof(KE), cmp_ke);
    qsort(tru, U, sizeof(KE), cmp_ke);
    printf("top 8 (key:estimate/true):");
    int agree = 0;
    for (int i = 0; i < 8; i++) {
        printf(" %u:%u/%u", est[i].key, est[i].est, exact[est[i].key]);
        for (int j = 0; j < 8; j++)
            agree += est[i].key == tru[j].key;
    }
    printf("\noverlap with true top 8: %d\n", agree);
    /* merge: sketches are linear, so adding tables equals sketching the concatenated stream */
    CMS a, b, whole;
    cms_init(&a, 0);
    cms_init(&b, 0);
    cms_init(&whole, 0);
    for (int i = 0; i < 5000; i++) {
        uint32_t key = (uint32_t)(rnd() % 300);
        cms_add(i % 2 ? &a : &b, key, 1);
        cms_add(&whole, key, 1);
    }
    for (int r = 0; r < D; r++)
        for (int c = 0; c < W; c++)
            a.c[r][c] += b.c[r][c];
    check(memcmp(a.c, whole.c, sizeof a.c) == 0, "merge is linear");
    printf("merge check passed\n");
    return 0;
}
