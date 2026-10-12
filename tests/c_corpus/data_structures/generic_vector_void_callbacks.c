/*
 * title: Generic vector over void pointers with element callbacks
 * topic: data_structures
 * covers: type-erased container, element size, destructor and copy callbacks, memcpy moves, stable merge sort
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 55555u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef int (*CmpFn)(const void *, const void *);
typedef void (*CopyFn)(void *dst, const void *src);
typedef void (*DtorFn)(void *);

typedef struct {
    unsigned char *data;
    size_t esz, len, cap;
    CmpFn cmp;
    CopyFn copy; /* NULL means memcpy */
    DtorFn dtor; /* NULL means nothing to release */
} Vec;

static void vec_init(Vec *v, size_t esz, CmpFn cmp, CopyFn copy, DtorFn dtor) {
    v->data = NULL; v->esz = esz; v->len = v->cap = 0;
    v->cmp = cmp; v->copy = copy; v->dtor = dtor;
}
static void *vec_at(const Vec *v, size_t i) { return v->data + i * v->esz; }
static void vec_reserve(Vec *v, size_t n) {
    if (n <= v->cap) return;
    size_t c = v->cap ? v->cap : 4;
    while (c < n) c *= 2;
    v->data = realloc(v->data, c * v->esz);
    CHECK(v->data);
    v->cap = c;
}
static void vec_put(const Vec *v, void *dst, const void *src) {
    if (v->copy) v->copy(dst, src); else memcpy(dst, src, v->esz);
}
static void vec_push(Vec *v, const void *elem) {
    vec_reserve(v, v->len + 1);
    vec_put(v, vec_at(v, v->len), elem);
    v->len++;
}
static void vec_insert(Vec *v, size_t i, const void *elem) {
    CHECK(i <= v->len);
    vec_reserve(v, v->len + 1);
    memmove(vec_at(v, i + 1), vec_at(v, i), (v->len - i) * v->esz);
    vec_put(v, vec_at(v, i), elem);
    v->len++;
}
static void vec_erase(Vec *v, size_t i) {
    CHECK(i < v->len);
    if (v->dtor) v->dtor(vec_at(v, i));
    memmove(vec_at(v, i), vec_at(v, i + 1), (v->len - i - 1) * v->esz);
    v->len--;
}
static void vec_swap(Vec *v, size_t i, size_t j) {
    if (i == j) return;
    unsigned char tmp[64];
    CHECK(v->esz <= sizeof tmp);
    memcpy(tmp, vec_at(v, i), v->esz);
    memcpy(vec_at(v, i), vec_at(v, j), v->esz);
    memcpy(vec_at(v, j), tmp, v->esz);
}
static void vec_reverse(Vec *v) {
    for (size_t i = 0, j = v->len; i + 1 < j; i++, j--) vec_swap(v, i, j - 1);
}
static void msort(Vec *v, unsigned char *buf, size_t lo, size_t hi) {
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    msort(v, buf, lo, mid);
    msort(v, buf, mid, hi);
    size_t i = lo, j = mid, k = 0;
    while (i < mid && j < hi) {
        if (v->cmp(vec_at(v, j), vec_at(v, i)) < 0) { memcpy(buf + k * v->esz, vec_at(v, j), v->esz); j++; }
        else { memcpy(buf + k * v->esz, vec_at(v, i), v->esz); i++; }
        k++;
    }
    while (i < mid) { memcpy(buf + k * v->esz, vec_at(v, i++), v->esz); k++; }
    while (j < hi) { memcpy(buf + k * v->esz, vec_at(v, j++), v->esz); k++; }
    memcpy(vec_at(v, lo), buf, k * v->esz);
}
static void vec_sort(Vec *v) {
    unsigned char *buf = malloc(v->len * v->esz + 1);
    CHECK(buf);
    msort(v, buf, 0, v->len);
    free(buf);
}
static long vec_lower_bound(const Vec *v, const void *key) {
    size_t lo = 0, hi = v->len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v->cmp(vec_at(v, mid), key) < 0) lo = mid + 1; else hi = mid;
    }
    return (long)lo;
}
static long vec_find(const Vec *v, const void *key) {
    for (size_t i = 0; i < v->len; i++)
        if (v->cmp(vec_at(v, i), key) == 0) return (long)i;
    return -1;
}
static void vec_foreach(const Vec *v, void (*fn)(void *elem, void *ctx), void *ctx) {
    for (size_t i = 0; i < v->len; i++) fn(vec_at(v, i), ctx);
}
static void vec_clone(Vec *dst, const Vec *src) {
    vec_init(dst, src->esz, src->cmp, src->copy, src->dtor);
    for (size_t i = 0; i < src->len; i++) vec_push(dst, vec_at(src, i));
}
static void vec_clear(Vec *v) {
    if (v->dtor) for (size_t i = 0; i < v->len; i++) v->dtor(vec_at(v, i));
    v->len = 0;
}
static void vec_free(Vec *v) {
    vec_clear(v);
    free(v->data);
    v->data = NULL; v->cap = 0;
}

/* element type 1: int */
static int cmp_int(const void *a, const void *b) {
    int x, y;
    memcpy(&x, a, sizeof x); memcpy(&y, b, sizeof y);
    return (x > y) - (x < y);
}
/* element type 2: record owning a heap string, ordered by age only (to observe stability) */
typedef struct { char *name; int age; } Person;
static int live_names;
static void person_copy(void *dst, const void *src) {
    const Person *s = src;
    Person p;
    p.age = s->age;
    size_t n = strlen(s->name) + 1;
    p.name = malloc(n);
    CHECK(p.name);
    memcpy(p.name, s->name, n);
    live_names++;
    memcpy(dst, &p, sizeof p);
}
static void person_dtor(void *e) {
    Person *p = e;
    free(p->name);
    live_names--;
}
static int cmp_age(const void *a, const void *b) {
    const Person *x = a, *y = b;
    return (x->age > y->age) - (x->age < y->age);
}
static void sum_ages(void *e, void *ctx) { *(long *)ctx += ((Person *)e)->age; }
static void sum_ints(void *e, void *ctx) { int v; memcpy(&v, e, sizeof v); *(long *)ctx += v; }

int main(void) {
    /* int vector against a plain array model */
    Vec v;
    vec_init(&v, sizeof(int), cmp_int, NULL, NULL);
    int model[600];
    size_t mn = 0;
    for (int step = 0; step < 2000; step++) {
        unsigned op = rnd() % 10;
        int val = (int)(rnd() % 500);
        if (op < 5 || mn == 0) {
            if (mn >= 600) continue;
            size_t pos = rnd() % (mn + 1);
            vec_insert(&v, pos, &val);
            memmove(&model[pos + 1], &model[pos], (mn - pos) * sizeof(int));
            model[pos] = val;
            mn++;
        } else if (op < 8) {
            size_t pos = rnd() % mn;
            vec_erase(&v, pos);
            memmove(&model[pos], &model[pos + 1], (mn - pos - 1) * sizeof(int));
            mn--;
        } else if (op < 9) {
            size_t i = rnd() % mn, j = rnd() % mn;
            vec_swap(&v, i, j);
            int t = model[i]; model[i] = model[j]; model[j] = t;
        } else {
            vec_reverse(&v);
            for (size_t i = 0, j = mn; i + 1 < j; i++, j--) { int t = model[i]; model[i] = model[j - 1]; model[j - 1] = t; }
        }
        CHECK(v.len == mn);
    }
    for (size_t i = 0; i < mn; i++) CHECK(*(int *)vec_at(&v, i) == model[i]);
    Vec c;
    vec_clone(&c, &v);
    vec_sort(&c);
    for (size_t i = 1; i < c.len; i++) CHECK(*(int *)vec_at(&c, i - 1) <= *(int *)vec_at(&c, i));
    long s1 = 0, s2 = 0;
    vec_foreach(&v, sum_ints, &s1);
    vec_foreach(&c, sum_ints, &s2);
    CHECK(s1 == s2);
    for (int probe = 0; probe < 500; probe += 37) {
        long lb = vec_lower_bound(&c, &probe);
        long expect = 0;
        for (size_t i = 0; i < mn; i++) if (model[i] < probe) expect++;
        CHECK(lb == expect);
    }
    int needle = model[mn / 2];
    long at = vec_find(&v, &needle);
    CHECK(at >= 0 && model[at] == needle);
    printf("int vec: len=%zu sum=%ld first=%d last=%d find(%d)=%ld\n", v.len, s1, model[0], model[mn - 1], needle, at);
    printf("sorted clone: min=%d max=%d lower_bound(250)=%ld\n", *(int *)vec_at(&c, 0), *(int *)vec_at(&c, c.len - 1), vec_lower_bound(&c, &(int){250}));
    vec_free(&v);
    vec_free(&c);

    /* record vector with owned strings, sorted by age (stable) */
    Vec ps;
    vec_init(&ps, sizeof(Person), cmp_age, person_copy, person_dtor);
    static const char *names[] = { "ann", "bob", "cy", "dee", "eli", "fay", "gus", "hal" };
    for (int i = 0; i < 200; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "%s%d", names[i % 8], i);
        Person p = { nm, (int)(rnd() % 12) };
        vec_push(&ps, &p);
    }
    CHECK(live_names == 200);
    vec_sort(&ps);
    long agesum = 0;
    vec_foreach(&ps, sum_ages, &agesum);
    int prev_age = -1, prev_seq = -1;
    for (size_t i = 0; i < ps.len; i++) {
        Person *p = vec_at(&ps, i);
        const char *d = p->name;
        while (*d < '0' || *d > '9') d++;
        int seq = atoi(d);
        if (p->age == prev_age) CHECK(seq > prev_seq);
        prev_age = p->age; prev_seq = seq;
    }
    printf("people: %zu records, age sum %ld, youngest=%s(%d) oldest=%s(%d)\n", ps.len, agesum,
           ((Person *)vec_at(&ps, 0))->name, ((Person *)vec_at(&ps, 0))->age,
           ((Person *)vec_at(&ps, ps.len - 1))->name, ((Person *)vec_at(&ps, ps.len - 1))->age);
    Vec pc;
    vec_clone(&pc, &ps);
    CHECK(live_names == 400);
    for (int i = 0; i < 50; i++) vec_erase(&pc, 0);
    CHECK(live_names == 350);
    vec_free(&pc);
    vec_free(&ps);
    CHECK(live_names == 0);
    printf("all names released: %d live\n", live_names);
    return 0;
}
