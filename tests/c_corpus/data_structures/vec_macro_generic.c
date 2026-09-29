/*
 * title: Type-generic vector via macros
 * topic: data_structures
 * covers: macro-generated containers, token pasting, struct elements, typed dynamic arrays, sort with total order
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x27B70A8546D22FFCULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 24); }

/* Declares type Vec_<name> and functions vec_<name>_*. */
#define DEFINE_VEC(name, T)                                                                  \
    typedef struct { T *a; size_t n, cap; } Vec_##name;                                      \
    static inline void vec_##name##_init(Vec_##name *v) { v->a = NULL; v->n = v->cap = 0; }         \
    static inline void vec_##name##_free(Vec_##name *v) { free(v->a); v->a = NULL; v->n = v->cap = 0; } \
    static inline void vec_##name##_push(Vec_##name *v, T x) {                                      \
        if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 4; v->a = realloc(v->a, v->cap * sizeof(T)); CHECK(v->a); } \
        v->a[v->n++] = x;                                                                    \
    }                                                                                        \
    static inline T vec_##name##_pop(Vec_##name *v) { CHECK(v->n); return v->a[--v->n]; }          \
    static inline void vec_##name##_insert(Vec_##name *v, size_t i, T x) {                          \
        CHECK(i <= v->n); vec_##name##_push(v, x);                                           \
        memmove(v->a + i + 1, v->a + i, (v->n - 1 - i) * sizeof(T)); v->a[i] = x;            \
    }                                                                                        \
    static inline T vec_##name##_remove(Vec_##name *v, size_t i) {                                  \
        CHECK(i < v->n); T x = v->a[i];                                                      \
        memmove(v->a + i, v->a + i + 1, (v->n - i - 1) * sizeof(T)); v->n--; return x;       \
    }                                                                                        \
    static inline void vec_##name##_reverse(Vec_##name *v) {                                        \
        for (size_t i = 0, j = v->n; i + 1 < j; i++, j--) { T t = v->a[i]; v->a[i] = v->a[j - 1]; v->a[j - 1] = t; } \
    }                                                                                        \
    static inline void vec_##name##_sort(Vec_##name *v, int (*cmp)(const void *, const void *)) { qsort(v->a, v->n, sizeof(T), cmp); }

typedef struct { int id; int score; } Rec;
typedef struct { double x, y; } Pt;
typedef unsigned char Byte;

DEFINE_VEC(int, int)
DEFINE_VEC(rec, Rec)
DEFINE_VEC(pt, Pt)
DEFINE_VEC(byte, Byte)

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int cmp_rec(const void *a, const void *b) { /* total order: score desc, then id asc */
    const Rec *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}
static int cmp_pt(const void *a, const void *b) { /* points are on an integer grid, so comparisons are exact */
    const Pt *x = a, *y = b;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    return (x->y > y->y) - (x->y < y->y);
}

static int cmp_byte(const void *a, const void *b) { return (int)*(const Byte *)a - (int)*(const Byte *)b; }

/* Exercise every generated function on a small vector: push c,a,b then insert, remove, sort, reverse. */
#define SELFTEST(name, cmpf, x, y, z)                                             \
    do {                                                                          \
        Vec_##name t; vec_##name##_init(&t);                                      \
        vec_##name##_push(&t, z); vec_##name##_push(&t, x); vec_##name##_push(&t, y); \
        vec_##name##_insert(&t, 1, y);                                            \
        (void)vec_##name##_remove(&t, 0);                                         \
        vec_##name##_sort(&t, cmpf); vec_##name##_reverse(&t);                    \
        CHECK(t.n == 3);                                                          \
        for (size_t q = 1; q < t.n; q++) CHECK(cmpf(&t.a[q - 1], &t.a[q]) >= 0);  \
        (void)vec_##name##_pop(&t); CHECK(t.n == 2);                              \
        vec_##name##_free(&t);                                                    \
    } while (0)

int main(void) {
    { Rec r1 = { 1, 5 }, r2 = { 2, 7 }, r3 = { 3, 5 }; SELFTEST(rec, cmp_rec, r1, r2, r3); }
    { Pt p1 = { 1, 2 }, p2 = { 3, 1 }, p3 = { 2, 9 }; SELFTEST(pt, cmp_pt, p1, p2, p3); }
    SELFTEST(byte, cmp_byte, (Byte)7, (Byte)200, (Byte)19);
    SELFTEST(int, cmp_int, 5, -3, 12);

    Vec_int vi; vec_int_init(&vi);
    Vec_rec vr; vec_rec_init(&vr);
    Vec_pt vp; vec_pt_init(&vp);
    Vec_byte vb; vec_byte_init(&vb);
    int mi[1000]; size_t mn = 0;
    for (int step = 0; step < 4000; step++) {
        unsigned op = rnd() % 100;
        int x = (int)(rnd() % 1000);
        if (op < 40 || mn < 5) { vec_int_push(&vi, x); mi[mn++] = x; }
        else if (op < 60) { size_t i = rnd() % (mn + 1); vec_int_insert(&vi, i, x); memmove(mi + i + 1, mi + i, (mn - i) * sizeof(int)); mi[i] = x; mn++; }
        else if (op < 80) { CHECK(vec_int_pop(&vi) == mi[--mn]); }
        else { size_t i = rnd() % mn; CHECK(vec_int_remove(&vi, i) == mi[i]); memmove(mi + i, mi + i + 1, (mn - i - 1) * sizeof(int)); mn--; }
        if (mn > 900) while (mn > 400) { CHECK(vec_int_pop(&vi) == mi[--mn]); }
        CHECK(vi.n == mn);
    }
    CHECK(memcmp(vi.a, mi, mn * sizeof(int)) == 0);
    vec_int_sort(&vi, cmp_int);
    for (size_t i = 1; i < vi.n; i++) CHECK(vi.a[i - 1] <= vi.a[i]);
    vec_int_reverse(&vi);
    printf("ints: n=%zu max=%d min=%d\n", vi.n, vi.a[0], vi.a[vi.n - 1]);

    for (int i = 0; i < 200; i++) { Rec r = { i, (int)(rnd() % 10) }; vec_rec_push(&vr, r); }
    vec_rec_sort(&vr, cmp_rec);
    for (size_t i = 1; i < vr.n; i++) CHECK(cmp_rec(&vr.a[i - 1], &vr.a[i]) < 0);
    printf("recs: top=%d/%d/%d bottom=%d/%d\n", vr.a[0].id, vr.a[0].score, vr.a[1].id, vr.a[199].id, vr.a[199].score);

    for (int i = 0; i < 100; i++) { Pt p = { (double)(rnd() % 20), (double)(rnd() % 20) }; vec_pt_push(&vp, p); }
    vec_pt_sort(&vp, cmp_pt);
    double weighted = 0; /* position-weighted checksum over the whole sorted array (exact on integers) */
    for (size_t i = 0; i < vp.n; i++) weighted += (vp.a[i].x * 20 + vp.a[i].y) * (double)(i + 1);
    printf("pts: first=(%.0f,%.0f) last=(%.0f,%.0f) weighted=%.0f\n", vp.a[0].x, vp.a[0].y, vp.a[99].x, vp.a[99].y, weighted);

    for (int i = 0; i < 1000; i++) vec_byte_push(&vb, (Byte)(i * 37 + 11));
    Byte first = vec_byte_pop(&vb); vec_byte_reverse(&vb);
    unsigned sum = 0; for (size_t i = 0; i < vb.n; i++) sum += vb.a[i];
    printf("bytes: n=%zu popped=%u sum=%u sizeof(Rec)=%zu\n", vb.n, (unsigned)first, sum, sizeof(Rec));
    vec_int_free(&vi); vec_rec_free(&vr); vec_pt_free(&vp); vec_byte_free(&vb);
    return 0;
}
