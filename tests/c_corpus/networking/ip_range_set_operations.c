/*
 * title: IPv4 range sets and range to CIDR decomposition
 * topic: networking
 * covers: interval merge, union, intersection, difference, complement, minimal CIDR cover of a range, bitmap cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { uint32_t lo, hi; } Rng;
typedef struct { Rng *v; int n, cap; } Set;

static void push(Set *s, uint32_t lo, uint32_t hi) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = realloc(s->v, (size_t)s->cap * sizeof(Rng));
        CHECK(s->v);
    }
    s->v[s->n].lo = lo;
    s->v[s->n].hi = hi;
    s->n++;
}

static int cmp(const void *a, const void *b) {
    const Rng *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* normalize: sort, merge overlapping and adjacent (careful with hi == UINT32_MAX) */
static void normalize(Set *s) {
    if (s->n > 1) qsort(s->v, (size_t)s->n, sizeof(Rng), cmp);
    int m = 0;
    for (int i = 0; i < s->n; i++) {
        if (m > 0 && (s->v[i].lo <= s->v[m - 1].hi || s->v[i].lo - 1 == s->v[m - 1].hi)) {
            if (s->v[i].hi > s->v[m - 1].hi) s->v[m - 1].hi = s->v[i].hi;
        } else {
            s->v[m++] = s->v[i];
        }
    }
    s->n = m;
}

static Set set_intersect(const Set *a, const Set *b) {
    Set r = { 0, 0, 0 };
    int i = 0, j = 0;
    while (i < a->n && j < b->n) {
        uint32_t lo = a->v[i].lo > b->v[j].lo ? a->v[i].lo : b->v[j].lo;
        uint32_t hi = a->v[i].hi < b->v[j].hi ? a->v[i].hi : b->v[j].hi;
        if (lo <= hi) push(&r, lo, hi);
        if (a->v[i].hi < b->v[j].hi) i++; else j++;
    }
    return r;
}

static Set set_complement(const Set *a) {
    Set r = { 0, 0, 0 };
    uint64_t next = 0;
    for (int i = 0; i < a->n; i++) {
        if (a->v[i].lo > next) push(&r, (uint32_t)next, a->v[i].lo - 1);
        next = (uint64_t)a->v[i].hi + 1;
    }
    if (next <= 0xffffffffull) push(&r, (uint32_t)next, 0xffffffffu);
    return r;
}

static Set set_union(const Set *a, const Set *b) {
    Set r = { 0, 0, 0 };
    for (int i = 0; i < a->n; i++) push(&r, a->v[i].lo, a->v[i].hi);
    for (int i = 0; i < b->n; i++) push(&r, b->v[i].lo, b->v[i].hi);
    normalize(&r);
    return r;
}

static Set set_diff(const Set *a, const Set *b) {
    Set nb = set_complement(b);
    Set r = set_intersect(a, &nb);
    free(nb.v);
    return r;
}

/* minimal CIDR cover of [lo, hi]; returns count and calls emit */
static int range_to_cidrs(uint32_t lo, uint32_t hi, uint32_t *addrs, int *lens) {
    int n = 0;
    uint64_t cur = lo;
    while (cur <= hi) {
        int len = 32;
        while (len > 0) {
            uint64_t size = 1ull << (32 - (len - 1));
            if ((cur & (size - 1)) != 0 || cur + size - 1 > hi) break;
            len--;
        }
        addrs[n] = (uint32_t)cur;
        lens[n] = len;
        n++;
        cur += 1ull << (32 - len);
    }
    return n;
}

static void ip(uint32_t a, char *b) {
    snprintf(b, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
}

static void show(const char *name, const Set *s) {
    printf("%s:", name);
    for (int i = 0; i < s->n; i++) {
        char a[16], b[16];
        ip(s->v[i].lo, a);
        ip(s->v[i].hi, b);
        printf(" [%s-%s]", a, b);
    }
    printf("\n");
}

static uint32_t rs = 0x7a11beefu;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    Set a = { 0, 0, 0 }, b = { 0, 0, 0 };
    push(&a, 0x0a000005u, 0x0a00000fu);
    push(&a, 0x0a000010u, 0x0a000020u);
    push(&a, 0x0a000100u, 0x0a0001ffu);
    push(&a, 0x0a000012u, 0x0a000014u);
    push(&b, 0x0a00000au, 0x0a000018u);
    push(&b, 0x0a000180u, 0x0a000300u);
    normalize(&a);
    normalize(&b);
    show("A", &a);
    show("B", &b);
    Set u = set_union(&a, &b), in = set_intersect(&a, &b), df = set_diff(&a, &b);
    show("A|B", &u);
    show("A&B", &in);
    show("A-B", &df);
    Set ca = set_complement(&a);
    printf("complement of A has %d ranges; first ends at %u.%u.%u.%u\n", ca.n, (unsigned)(ca.v[0].hi >> 24),
           (unsigned)((ca.v[0].hi >> 16) & 255), (unsigned)((ca.v[0].hi >> 8) & 255), (unsigned)(ca.v[0].hi & 255));
    free(a.v); free(b.v); free(u.v); free(in.v); free(df.v); free(ca.v);

    /* Range to CIDR examples. */
    static const uint32_t ex[][2] = { { 0x0a000001u, 0x0a000006u }, { 0xc0a80000u, 0xc0a8ffffu }, { 0x0a000000u, 0x0a0000ffu }, { 0x00000000u, 0xffffffffu }, { 0xfffffffeu, 0xffffffffu }, { 0x0a00000au, 0x0a00000au } };
    for (size_t e = 0; e < sizeof ex / sizeof ex[0]; e++) {
        uint32_t ad[64];
        int ln[64];
        int n = range_to_cidrs(ex[e][0], ex[e][1], ad, ln);
        char x[16], y[16];
        ip(ex[e][0], x);
        ip(ex[e][1], y);
        printf("%s-%s ->", x, y);
        uint64_t sum = 0;
        for (int i = 0; i < n; i++) { char z[16]; ip(ad[i], z); printf(" %s/%d", z, ln[i]); sum += 1ull << (32 - ln[i]); }
        printf("\n");
        CHECK(sum == (uint64_t)ex[e][1] - ex[e][0] + 1);
    }

    /* Random sets in a 4096-address universe checked against bitmaps. */
    enum { U = 4096 };
    unsigned char ba[U], bb[U];
    long checks = 0;
    for (int t = 0; t < 300; t++) {
        Set x = { 0, 0, 0 }, y = { 0, 0, 0 };
        memset(ba, 0, U); memset(bb, 0, U);
        int nx = (int)(rnd() % 8), ny = (int)(rnd() % 8);
        for (int i = 0; i < nx; i++) { uint32_t lo = rnd() % U; uint32_t hi = lo + rnd() % 300; if (hi >= U) hi = U - 1; push(&x, lo, hi); for (uint32_t k = lo; k <= hi; k++) ba[k] = 1; }
        for (int i = 0; i < ny; i++) { uint32_t lo = rnd() % U; uint32_t hi = lo + rnd() % 300; if (hi >= U) hi = U - 1; push(&y, lo, hi); for (uint32_t k = lo; k <= hi; k++) bb[k] = 1; }
        normalize(&x); normalize(&y);
        Set su = set_union(&x, &y), si = set_intersect(&x, &y), sd = set_diff(&x, &y);
        for (uint32_t k = 0; k < U; k++) {
            int iu = 0, ii = 0, id = 0;
            for (int i = 0; i < su.n; i++) if (k >= su.v[i].lo && k <= su.v[i].hi) iu = 1;
            for (int i = 0; i < si.n; i++) if (k >= si.v[i].lo && k <= si.v[i].hi) ii = 1;
            for (int i = 0; i < sd.n; i++) if (k >= sd.v[i].lo && k <= sd.v[i].hi) id = 1;
            CHECK(iu == (ba[k] | bb[k]));
            CHECK(ii == (ba[k] & bb[k]));
            CHECK(id == (ba[k] & !bb[k]));
            checks += 3;
        }
        for (int i = 0; i + 1 < su.n; i++) CHECK(su.v[i].hi + 1 < su.v[i + 1].lo);
        free(x.v); free(y.v); free(su.v); free(si.v); free(sd.v);
    }
    printf("bitmap cross-check passed %ld membership tests\n", checks);
    return 0;
}
