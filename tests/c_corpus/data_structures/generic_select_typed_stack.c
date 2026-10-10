/*
 * title: C11 _Generic dispatch over a heterogeneous value stack
 * topic: data_structures
 * covers: _Generic selection, type-directed push and pop, tagged storage, generic helper macros
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 606060u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef enum { T_INT, T_LONG, T_DOUBLE, T_STR, T_BOOL } Tag;
typedef struct {
    Tag tag;
    union { int i; long l; double d; const char *s; } u;
} Val;
typedef struct { Val *v; size_t n, cap; } Stack;

static const char *tag_name(Tag t) {
    switch (t) {
    case T_INT: return "int";
    case T_LONG: return "long";
    case T_DOUBLE: return "double";
    case T_STR: return "str";
    case T_BOOL: return "bool";
    }
    return "?";
}

static void grow(Stack *s) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = realloc(s->v, s->cap * sizeof *s->v);
        CHECK(s->v);
    }
}
static void push_int(Stack *s, int x) { grow(s); s->v[s->n].tag = T_INT; s->v[s->n++].u.i = x; }
static void push_long(Stack *s, long x) { grow(s); s->v[s->n].tag = T_LONG; s->v[s->n++].u.l = x; }
static void push_double(Stack *s, double x) { grow(s); s->v[s->n].tag = T_DOUBLE; s->v[s->n++].u.d = x; }
static void push_str(Stack *s, const char *x) { grow(s); s->v[s->n].tag = T_STR; s->v[s->n++].u.s = x; }
static void push_bool(Stack *s, _Bool x) { grow(s); s->v[s->n].tag = T_BOOL; s->v[s->n++].u.i = x; }

/* the pop family checks the stored tag against the type of the destination */
static int pop_int(Stack *s, int *out) { if (!s->n || s->v[s->n - 1].tag != T_INT) return 0; *out = s->v[--s->n].u.i; return 1; }
static int pop_long(Stack *s, long *out) { if (!s->n || s->v[s->n - 1].tag != T_LONG) return 0; *out = s->v[--s->n].u.l; return 1; }
static int pop_double(Stack *s, double *out) { if (!s->n || s->v[s->n - 1].tag != T_DOUBLE) return 0; *out = s->v[--s->n].u.d; return 1; }
static int pop_str(Stack *s, const char **out) { if (!s->n || s->v[s->n - 1].tag != T_STR) return 0; *out = s->v[--s->n].u.s; return 1; }
static int pop_bool(Stack *s, _Bool *out) { if (!s->n || s->v[s->n - 1].tag != T_BOOL) return 0; *out = (_Bool)s->v[--s->n].u.i; return 1; }

#define PUSH(s, x) _Generic((x),                     \
        _Bool: push_bool,                            \
        int: push_int,                               \
        long: push_long,                             \
        double: push_double,                         \
        char *: push_str,                            \
        const char *: push_str)((s), (x))

#define POP(s, outp) _Generic((outp),                \
        _Bool *: pop_bool,                           \
        int *: pop_int,                              \
        long *: pop_long,                            \
        double *: pop_double,                        \
        const char **: pop_str)((s), (outp))

#define TYPE_NAME(x) _Generic((x), _Bool: "bool", int: "int", long: "long", double: "double", \
                              char *: "str", const char *: "str", default: "other")

#define GMAX(a, b) _Generic((a) + (b), int: max_int, long: max_long, double: max_double)((a), (b))
static int max_int(int a, int b) { return a > b ? a : b; }
static long max_long(long a, long b) { return a > b ? a : b; }
static double max_double(double a, double b) { return a > b ? a : b; }

static void show(const Val *v, char *buf, size_t n) {
    switch (v->tag) {
    case T_INT: snprintf(buf, n, "%d", v->u.i); break;
    case T_LONG: snprintf(buf, n, "%ldL", v->u.l); break;
    case T_DOUBLE: snprintf(buf, n, "%.2f", v->u.d); break;
    case T_STR: snprintf(buf, n, "\"%s\"", v->u.s); break;
    case T_BOOL: snprintf(buf, n, "%s", v->u.i ? "true" : "false"); break;
    }
}

int main(void) {
    Stack s = { NULL, 0, 0 };
    static const char *words[] = { "alpha", "beta", "gamma", "delta" };
    int i0 = 7;
    long l0 = 9000000000L;
    double d0 = 2.5;
    _Bool b0 = 1;
    printf("names: %s %s %s %s %s\n", TYPE_NAME(i0), TYPE_NAME(l0), TYPE_NAME(d0), TYPE_NAME(b0), TYPE_NAME("lit"));
    PUSH(&s, i0);
    PUSH(&s, l0);
    PUSH(&s, d0);
    PUSH(&s, b0);
    PUSH(&s, "literal");
    PUSH(&s, words[2]);
    char buf[32];
    printf("stack:");
    for (size_t i = 0; i < s.n; i++) { show(&s.v[i], buf, sizeof buf); printf(" %s:%s", tag_name(s.v[i].tag), buf); }
    printf("\n");
    const char *sp = NULL;
    int ip = 0;
    CHECK(!POP(&s, &ip));            /* top is a string, int pop must refuse */
    CHECK(POP(&s, &sp) && strcmp(sp, "gamma") == 0);
    CHECK(POP(&s, &sp) && strcmp(sp, "literal") == 0);
    _Bool bp = 0;
    CHECK(POP(&s, &bp) && bp);
    double dp = 0;
    CHECK(POP(&s, &dp) && dp == 2.5);
    long lp = 0;
    CHECK(POP(&s, &lp) && lp == 9000000000L);
    CHECK(POP(&s, &ip) && ip == 7);
    CHECK(s.n == 0);
    printf("typed pops matched; refused mismatched pop\n");

    /* randomized: push random typed values, model keeps the tags, then pop with the same tags */
    Tag model[400];
    long ml[400]; double md[400];
    size_t n = 0;
    for (int i = 0; i < 400; i++) {
        unsigned r = rnd() % 5;
        long lv = (long)(rnd() % 100000) - 50000;
        double dv = (double)lv / 16.0;
        switch (r) {
        case 0: { int x = (int)lv; PUSH(&s, x); model[n] = T_INT; ml[n] = x; break; }
        case 1: PUSH(&s, lv); model[n] = T_LONG; ml[n] = lv; break;
        case 2: PUSH(&s, dv); model[n] = T_DOUBLE; md[n] = dv; break;
        case 3: { const char *w = words[(unsigned long)lv % 4]; PUSH(&s, w); model[n] = T_STR; ml[n] = (long)((unsigned long)lv % 4); break; }
        default: { _Bool b = lv > 0; PUSH(&s, b); model[n] = T_BOOL; ml[n] = b; break; }
        }
        n++;
    }
    CHECK(s.n == n);
    long counts[5] = { 0 };
    long isum = 0;
    double dsum = 0;
    int maxi = -1000000;
    long maxl = -1000000000L;
    double maxd = -1e9;
    while (n > 0) {
        n--;
        counts[model[n]]++;
        switch (model[n]) {
        case T_INT: { int x; CHECK(POP(&s, &x) && x == ml[n]); isum += x; maxi = GMAX(maxi, x); break; }
        case T_LONG: { long x; CHECK(POP(&s, &x) && x == ml[n]); isum += x; maxl = GMAX(maxl, x); break; }
        case T_DOUBLE: { double x; CHECK(POP(&s, &x) && x == md[n]); dsum += x; maxd = GMAX(maxd, x); break; }
        case T_STR: { const char *x; CHECK(POP(&s, &x) && strcmp(x, words[ml[n]]) == 0); break; }
        case T_BOOL: { _Bool x; CHECK(POP(&s, &x) && x == (_Bool)ml[n]); break; }
        }
    }
    CHECK(s.n == 0);
    printf("counts int=%ld long=%ld double=%ld str=%ld bool=%ld\n", counts[T_INT], counts[T_LONG], counts[T_DOUBLE], counts[T_STR], counts[T_BOOL]);
    printf("int/long sum=%ld double sum=%.3f max int=%d max long=%ld max double=%.3f\n", isum, dsum, maxi, maxl, maxd);
    free(s.v);
    return 0;
}
