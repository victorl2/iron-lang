/*
 * title: Dynamic array with doubling growth and full API
 * topic: data_structures
 * covers: dynamic array, amortized growth, insert/erase, swap-remove, shrink, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 16);
}

typedef struct { int *data; size_t len, cap; long reallocs; } Vec;

static void vec_init(Vec *v) { v->data = NULL; v->len = v->cap = 0; v->reallocs = 0; }
static void vec_free(Vec *v) { free(v->data); vec_init(v); }

static void vec_reserve(Vec *v, size_t want) {
    if (want <= v->cap) return;
    size_t nc = v->cap ? v->cap : 4;
    while (nc < want) nc *= 2;
    int *nd = realloc(v->data, nc * sizeof *nd);
    CHECK(nd != NULL);
    v->data = nd; v->cap = nc; v->reallocs++;
}
static void vec_push(Vec *v, int x) { vec_reserve(v, v->len + 1); v->data[v->len++] = x; }
static int vec_pop(Vec *v) { CHECK(v->len > 0); return v->data[--v->len]; }
static void vec_insert(Vec *v, size_t i, int x) {
    CHECK(i <= v->len);
    vec_reserve(v, v->len + 1);
    memmove(v->data + i + 1, v->data + i, (v->len - i) * sizeof(int));
    v->data[i] = x; v->len++;
}
static int vec_erase(Vec *v, size_t i) {
    CHECK(i < v->len);
    int x = v->data[i];
    memmove(v->data + i, v->data + i + 1, (v->len - i - 1) * sizeof(int));
    v->len--;
    return x;
}
static int vec_swap_remove(Vec *v, size_t i) {
    CHECK(i < v->len);
    int x = v->data[i];
    v->data[i] = v->data[--v->len];
    return x;
}
static void vec_shrink(Vec *v) {
    /* shrink to the smallest power of two >= len (min 4), or free if empty */
    if (v->len == 0) { free(v->data); v->data = NULL; v->cap = 0; return; }
    size_t nc = 4;
    while (nc < v->len) nc *= 2;
    if (nc < v->cap) {
        int *nd = realloc(v->data, nc * sizeof *nd);
        CHECK(nd != NULL);
        v->data = nd; v->cap = nc; v->reallocs++;
    }
}
static long vec_find(const Vec *v, int x) {
    for (size_t i = 0; i < v->len; i++) if (v->data[i] == x) return (long)i;
    return -1;
}

#define MAXN 4096
static int model[MAXN];
static size_t mlen;

static void same(const Vec *v) {
    CHECK(v->len == mlen);
    CHECK(v->len <= v->cap);
    CHECK(v->len == 0 || memcmp(v->data, model, mlen * sizeof(int)) == 0);
    CHECK((v->cap & (v->cap - 1)) == 0);
}

int main(void) {
    Vec v; vec_init(&v);
    long counts[8] = {0};
    size_t max_len = 0, max_cap = 0;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        int x = (int)(rnd() % 1000);
        if (mlen > 3000) op = 90; /* force drain */
        if (op < 35) {
            vec_push(&v, x); model[mlen++] = x; counts[0]++;
        } else if (op < 50) {
            size_t i = mlen ? rnd() % (mlen + 1) : 0;
            vec_insert(&v, i, x);
            memmove(model + i + 1, model + i, (mlen - i) * sizeof(int));
            model[i] = x; mlen++; counts[1]++;
        } else if (op < 62 && mlen) {
            CHECK(vec_pop(&v) == model[--mlen]); counts[2]++;
        } else if (op < 75 && mlen) {
            size_t i = rnd() % mlen;
            int e = model[i];
            CHECK(vec_erase(&v, i) == e);
            memmove(model + i, model + i + 1, (mlen - i - 1) * sizeof(int));
            mlen--; counts[3]++;
        } else if (op < 85 && mlen) {
            size_t i = rnd() % mlen;
            int e = model[i];
            CHECK(vec_swap_remove(&v, i) == e);
            model[i] = model[--mlen]; counts[4]++;
        } else if (op < 90) {
            long f = vec_find(&v, x), g = -1;
            for (size_t i = 0; i < mlen; i++) if (model[i] == x) { g = (long)i; break; }
            CHECK(f == g); counts[5]++;
        } else if (op < 93) {
            vec_shrink(&v); counts[6]++;
            CHECK(v.cap < 8 || v.cap < 2 * v.len + 1 || v.len == 0);
        } else if (mlen > 0) {
            size_t drop = rnd() % mlen;
            while (mlen > drop) { CHECK(vec_pop(&v) == model[--mlen]); }
            counts[7]++;
        }
        same(&v);
        if (v.len > max_len) max_len = v.len;
        if (v.cap > max_cap) max_cap = v.cap;
    }
    printf("push=%ld insert=%ld pop=%ld erase=%ld swap_remove=%ld find=%ld shrink=%ld truncate=%ld\n",
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7]);
    printf("final len=%zu cap=%zu max_len=%zu max_cap=%zu reallocs=%ld\n", v.len, v.cap, max_len, max_cap, v.reallocs);
    unsigned long h = 1469598103u;
    for (size_t i = 0; i < v.len; i++) h = (h * 31u + (unsigned)v.data[i]) & 0xffffffffu;
    printf("hash=%lu\n", h);
    vec_free(&v);
    return 0;
}
