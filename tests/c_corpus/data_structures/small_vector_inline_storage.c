/*
 * title: Small-buffer vector that spills to the heap and returns inline
 * topic: data_structures
 * covers: small buffer optimization, inline storage, spill and unspill, self-referential pointer fixups on move, swap across storage modes, allocation counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 8642u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define INLINE_BYTES 64
typedef struct {
    unsigned char *data;          /* points at inl while the contents fit, else at a heap block */
    size_t len, cap, esz;
    _Alignas(8) unsigned char inl[INLINE_BYTES];
} SVec;

static long heap_allocs, heap_frees, spills, unspills;

static void *counted_malloc(size_t n) { void *p = malloc(n); CHECK(p); heap_allocs++; return p; }
static void counted_free(void *p) { free(p); heap_frees++; }

static void sv_init(SVec *v, size_t esz) {
    v->esz = esz; v->len = 0;
    v->cap = INLINE_BYTES / esz;
    v->data = v->inl;
}
static int sv_is_inline(const SVec *v) { return v->data == v->inl; }
static void sv_free(SVec *v) {
    if (!sv_is_inline(v)) counted_free(v->data);
    sv_init(v, v->esz);
}
static void sv_reserve(SVec *v, size_t n) {
    if (n <= v->cap) return;
    size_t nc = v->cap * 2;
    while (nc < n) nc *= 2;
    unsigned char *nd = counted_malloc(nc * v->esz);
    memcpy(nd, v->data, v->len * v->esz);
    if (sv_is_inline(v)) spills++; else counted_free(v->data);
    v->data = nd; v->cap = nc;
}
static void *sv_at(const SVec *v, size_t i) { return v->data + i * v->esz; }
static void sv_push(SVec *v, const void *e) {
    sv_reserve(v, v->len + 1);
    memcpy(sv_at(v, v->len), e, v->esz);
    v->len++;
}
static void sv_pop(SVec *v) { CHECK(v->len); v->len--; }
static void sv_insert(SVec *v, size_t i, const void *e) {
    CHECK(i <= v->len);
    sv_reserve(v, v->len + 1);
    memmove(sv_at(v, i + 1), sv_at(v, i), (v->len - i) * v->esz);
    memcpy(sv_at(v, i), e, v->esz);
    v->len++;
}
static void sv_erase(SVec *v, size_t i) {
    CHECK(i < v->len);
    memmove(sv_at(v, i), sv_at(v, i + 1), (v->len - i - 1) * v->esz);
    v->len--;
}
/* move contents back into the inline buffer when they fit */
static void sv_shrink(SVec *v) {
    size_t inl_cap = INLINE_BYTES / v->esz;
    if (sv_is_inline(v) || v->len > inl_cap) return;
    memcpy(v->inl, v->data, v->len * v->esz);
    counted_free(v->data);
    v->data = v->inl;
    v->cap = inl_cap;
    unspills++;
}
/* a plain struct copy would leave `data` pointing into the old object, so moves and swaps fix it up */
static void sv_swap(SVec *a, SVec *b) {
    SVec t;
    memcpy(&t, a, sizeof t);
    memcpy(a, b, sizeof t);
    memcpy(b, &t, sizeof t);
    /* after the raw swap each pointer refers to the other object's inline buffer if it was inline */
    if (a->data == b->inl) a->data = a->inl;
    if (b->data == a->inl) b->data = b->inl;
}
static void sv_move(SVec *dst, SVec *src) {
    sv_free(dst);
    memcpy(dst, src, sizeof *dst);
    if (sv_is_inline(src)) dst->data = dst->inl;
    sv_init(src, src->esz);
}

typedef struct { int a; short b; char c[6]; long d; } Rec;   /* 24 bytes on LP64 */

int main(void) {
    /* int vector against a plain model, tracking when storage spills */
    SVec v;
    sv_init(&v, sizeof(int));
    int model[400], mn = 0;
    size_t inline_cap = INLINE_BYTES / sizeof(int);
    long inline_ops = 0, heap_ops = 0;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 12;
        int x = (int)(rnd() % 1000);
        if (op < 5 && mn < 400) { sv_push(&v, &x); model[mn++] = x; }
        else if (op < 6 && mn < 400) {
            size_t i = rnd() % (unsigned)(mn + 1);
            sv_insert(&v, i, &x);
            memmove(&model[i + 1], &model[i], (size_t)(mn - (int)i) * sizeof(int));
            model[i] = x; mn++;
        } else if (op < 8 && mn > 0) { sv_pop(&v); mn--; }
        else if (op < 10 && mn > 0) {
            size_t i = rnd() % (unsigned)mn;
            sv_erase(&v, i);
            memmove(&model[i], &model[i + 1], (size_t)(mn - (int)i - 1) * sizeof(int));
            mn--;
        } else if (op < 11) sv_shrink(&v);
        else if (rnd() % 20 == 0) { while (mn > 0) { sv_pop(&v); mn--; } }
        CHECK(v.len == (size_t)mn);
        CHECK(memcmp(v.data, model, (size_t)mn * sizeof(int)) == 0);
        if ((size_t)mn <= inline_cap) CHECK(sv_is_inline(&v) || v.cap > inline_cap);
        if (sv_is_inline(&v)) inline_ops++; else heap_ops++;
        CHECK(!sv_is_inline(&v) || (size_t)mn <= inline_cap);
    }
    printf("int vec: final len %zu, inline capacity %zu, steps inline=%ld heap=%ld\n", v.len, inline_cap, inline_ops, heap_ops);
    printf("  spills=%ld unspills=%ld heap allocs=%ld frees=%ld\n", spills, unspills, heap_allocs, heap_frees);
    sv_free(&v);
    CHECK(heap_allocs == heap_frees);

    /* staying small never touches the heap */
    long before = heap_allocs;
    SVec s;
    sv_init(&s, sizeof(int));
    for (int i = 0; i < (int)inline_cap; i++) sv_push(&s, &i);
    CHECK(sv_is_inline(&s) && heap_allocs == before);
    int extra = 99;
    sv_push(&s, &extra);
    CHECK(!sv_is_inline(&s) && heap_allocs == before + 1);
    sv_pop(&s);
    sv_shrink(&s);
    CHECK(sv_is_inline(&s) && heap_allocs == heap_frees);
    printf("boundary: %zu elements stay inline, one more spills, shrink brings it back\n", inline_cap);

    /* swap and move across storage modes, with struct elements (only 2 fit inline) */
    SVec a, b, c;
    sv_init(&a, sizeof(Rec)); sv_init(&b, sizeof(Rec)); sv_init(&c, sizeof(Rec));
    size_t rec_inline = INLINE_BYTES / sizeof(Rec);
    for (int round = 0; round < 200; round++) {
        size_t na = rnd() % 6, nb = rnd() % 6;
        sv_free(&a); sv_free(&b);
        Rec ra[6], rb[6];
        for (size_t i = 0; i < na; i++) { memset(&ra[i], 0, sizeof ra[i]); ra[i].a = (int)(rnd() % 100); ra[i].d = (long)i; snprintf(ra[i].c, sizeof ra[i].c, "a%zu", i); sv_push(&a, &ra[i]); }
        for (size_t i = 0; i < nb; i++) { memset(&rb[i], 0, sizeof rb[i]); rb[i].a = (int)(rnd() % 100); rb[i].d = (long)i; snprintf(rb[i].c, sizeof rb[i].c, "b%zu", i); sv_push(&b, &rb[i]); }
        int a_inl = sv_is_inline(&a), b_inl = sv_is_inline(&b);
        CHECK(a_inl == (na <= rec_inline) && b_inl == (nb <= rec_inline));
        sv_swap(&a, &b);
        CHECK(a.len == nb && b.len == na);
        CHECK(sv_is_inline(&a) == b_inl && sv_is_inline(&b) == a_inl);
        for (size_t i = 0; i < nb; i++) CHECK(memcmp(sv_at(&a, i), &rb[i], sizeof(Rec)) == 0);
        for (size_t i = 0; i < na; i++) CHECK(memcmp(sv_at(&b, i), &ra[i], sizeof(Rec)) == 0);
        sv_move(&c, &a);
        CHECK(c.len == nb && a.len == 0 && sv_is_inline(&a));
        for (size_t i = 0; i < nb; i++) CHECK(memcmp(sv_at(&c, i), &rb[i], sizeof(Rec)) == 0);
        CHECK(sv_is_inline(&c) == b_inl);
    }
    sv_free(&a); sv_free(&b); sv_free(&c);
    CHECK(heap_allocs == heap_frees);
    printf("struct vec (inline capacity %zu): 200 swap/move rounds consistent, allocs=%ld frees=%ld\n", rec_inline, heap_allocs, heap_frees);
    return 0;
}
