/*
 * title: Tagged-union variant values with deep copy, order, hash and text round trip
 * topic: data_structures
 * covers: tagged unions, nested heterogeneous lists, deep copy, total order, hashing, printer and parser
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 424243u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef enum { K_NIL, K_BOOL, K_INT, K_REAL, K_STR, K_LIST } Kind;
typedef struct Var Var;
struct Var {
    Kind kind;
    union {
        int b;
        long i;
        double r;
        char *s;
        struct { Var *items; size_t n; } list;
    } u;
};

static long live_allocs;
static void *xm(size_t n) { void *p = malloc(n ? n : 1); CHECK(p); live_allocs++; return p; }
static void xf(void *p) { free(p); live_allocs--; }

static Var v_nil(void) { Var v; memset(&v, 0, sizeof v); v.kind = K_NIL; return v; }
static Var v_bool(int b) { Var v = v_nil(); v.kind = K_BOOL; v.u.b = b != 0; return v; }
static Var v_int(long i) { Var v = v_nil(); v.kind = K_INT; v.u.i = i; return v; }
static Var v_real(double r) { Var v = v_nil(); v.kind = K_REAL; v.u.r = r; return v; }
static Var v_str(const char *s) {
    Var v = v_nil();
    v.kind = K_STR;
    size_t n = strlen(s) + 1;
    v.u.s = xm(n);
    memcpy(v.u.s, s, n);
    return v;
}
static Var v_list(size_t n) {
    Var v = v_nil();
    v.kind = K_LIST;
    v.u.list.n = n;
    v.u.list.items = xm(n * sizeof(Var));
    for (size_t i = 0; i < n; i++) v.u.list.items[i] = v_nil();
    return v;
}
static void v_free(Var *v) {
    if (v->kind == K_STR) xf(v->u.s);
    else if (v->kind == K_LIST) {
        for (size_t i = 0; i < v->u.list.n; i++) v_free(&v->u.list.items[i]);
        xf(v->u.list.items);
    }
    *v = v_nil();
}
static Var v_copy(const Var *v) {
    switch (v->kind) {
    case K_STR: return v_str(v->u.s);
    case K_LIST: {
        Var c = v_list(v->u.list.n);
        for (size_t i = 0; i < v->u.list.n; i++) c.u.list.items[i] = v_copy(&v->u.list.items[i]);
        return c;
    }
    default: return *v;
    }
}
/* total order: by kind first, then by content; lists lexicographically */
static int v_cmp(const Var *a, const Var *b) {
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    switch (a->kind) {
    case K_NIL: return 0;
    case K_BOOL: return a->u.b - b->u.b;
    case K_INT: return (a->u.i > b->u.i) - (a->u.i < b->u.i);
    case K_REAL: return (a->u.r > b->u.r) - (a->u.r < b->u.r);
    case K_STR: { int c = strcmp(a->u.s, b->u.s); return (c > 0) - (c < 0); }
    case K_LIST: {
        size_t n = a->u.list.n < b->u.list.n ? a->u.list.n : b->u.list.n;
        for (size_t i = 0; i < n; i++) {
            int c = v_cmp(&a->u.list.items[i], &b->u.list.items[i]);
            if (c) return c;
        }
        return (a->u.list.n > b->u.list.n) - (a->u.list.n < b->u.list.n);
    }
    }
    return 0;
}
static uint32_t v_hash(const Var *v) {
    uint32_t h = 2166136261u ^ (uint32_t)v->kind;
    switch (v->kind) {
    case K_NIL: break;
    case K_BOOL: h = (h ^ (uint32_t)v->u.b) * 16777619u; break;
    case K_INT: h = (h ^ (uint32_t)(uint64_t)v->u.i) * 16777619u; break;
    case K_REAL: h = (h ^ (uint32_t)(long)(v->u.r * 8.0)) * 16777619u; break;
    case K_STR: for (const char *p = v->u.s; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u; break;
    case K_LIST:
        for (size_t i = 0; i < v->u.list.n; i++) h = (h ^ v_hash(&v->u.list.items[i])) * 16777619u;
        break;
    }
    return h;
}

/* printer into a growable buffer */
typedef struct { char *s; size_t n, cap; } Buf;
static void b_put(Buf *b, const char *t) {
    size_t k = strlen(t);
    if (b->n + k + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->n + k + 1) nc *= 2;
        char *ns = xm(nc);
        if (b->s) { memcpy(ns, b->s, b->n); xf(b->s); }
        b->s = ns; b->cap = nc;
    }
    memcpy(b->s + b->n, t, k + 1);
    b->n += k;
}
static void v_print(const Var *v, Buf *b) {
    char t[64];
    switch (v->kind) {
    case K_NIL: b_put(b, "nil"); break;
    case K_BOOL: b_put(b, v->u.b ? "true" : "false"); break;
    case K_INT: snprintf(t, sizeof t, "%ld", v->u.i); b_put(b, t); break;
    case K_REAL: snprintf(t, sizeof t, "%.3f", v->u.r); b_put(b, t); break;
    case K_STR: b_put(b, "\""); b_put(b, v->u.s); b_put(b, "\""); break;
    case K_LIST:
        b_put(b, "[");
        for (size_t i = 0; i < v->u.list.n; i++) {
            if (i) b_put(b, ",");
            v_print(&v->u.list.items[i], b);
        }
        b_put(b, "]");
        break;
    }
}
/* parser: recursive descent over the printed form */
static Var v_parse(const char **pp) {
    const char *p = *pp;
    Var out;
    if (*p == '[') {
        p++;
        size_t cap = 4, n = 0;
        Var *tmp = xm(cap * sizeof(Var));
        while (*p != ']') {
            if (n == cap) {
                Var *nt = xm(cap * 2 * sizeof(Var));
                memcpy(nt, tmp, n * sizeof(Var));
                xf(tmp);
                tmp = nt;
                cap *= 2;
            }
            tmp[n++] = v_parse(&p);
            if (*p == ',') p++;
        }
        p++;
        out = v_list(n);
        for (size_t i = 0; i < n; i++) out.u.list.items[i] = tmp[i];
        xf(tmp);
    } else if (*p == '"') {
        const char *e = strchr(p + 1, '"');
        CHECK(e);
        char *s = xm((size_t)(e - p));
        memcpy(s, p + 1, (size_t)(e - p - 1));
        s[e - p - 1] = 0;
        out = v_str(s);
        xf(s);
        p = e + 1;
    } else if (strncmp(p, "nil", 3) == 0) { out = v_nil(); p += 3; }
    else if (strncmp(p, "true", 4) == 0) { out = v_bool(1); p += 4; }
    else if (strncmp(p, "false", 5) == 0) { out = v_bool(0); p += 5; }
    else {
        char *end;
        const char *q = p;
        int real = 0;
        while (*q == '-' || (*q >= '0' && *q <= '9') || *q == '.') { if (*q == '.') real = 1; q++; }
        CHECK(q > p);
        out = real ? v_real(strtod(p, &end)) : v_int(strtol(p, &end, 10));
        p = q;
    }
    *pp = p;
    return out;
}

static Var gen(int depth) {
    unsigned k = rnd() % (depth > 0 ? 7u : 5u);
    switch (k) {
    case 0: return v_nil();
    case 1: return v_bool((int)(rnd() % 2));
    case 2: return v_int((long)(rnd() % 2001) - 1000);
    case 3: return v_real((double)((int)(rnd() % 8001) - 4000) / 8.0);
    case 4: {
        char s[8];
        size_t n = rnd() % 6;
        for (size_t i = 0; i < n; i++) s[i] = (char)('a' + rnd() % 26);
        s[n] = 0;
        return v_str(s);
    }
    default: {
        size_t n = rnd() % 5;
        Var l = v_list(n);
        for (size_t i = 0; i < n; i++) l.u.list.items[i] = gen(depth - 1);
        return l;
    }
    }
}
static int cmp_qs(const void *a, const void *b) { return v_cmp(a, b); }
static int depth_of(const Var *v) {
    if (v->kind != K_LIST) return 0;
    int d = 0;
    for (size_t i = 0; i < v->u.list.n; i++) { int c = depth_of(&v->u.list.items[i]); if (c > d) d = c; }
    return d + 1;
}

int main(void) {
    enum { N = 300 };
    Var *bag = xm(N * sizeof(Var));
    long kinds[6] = { 0 };
    int maxdepth = 0;
    for (int i = 0; i < N; i++) {
        bag[i] = gen(4);
        kinds[bag[i].kind]++;
        int d = depth_of(&bag[i]);
        if (d > maxdepth) maxdepth = d;
    }
    printf("generated %d values: nil=%ld bool=%ld int=%ld real=%ld str=%ld list=%ld, max depth %d\n", N,
           kinds[K_NIL], kinds[K_BOOL], kinds[K_INT], kinds[K_REAL], kinds[K_STR], kinds[K_LIST], maxdepth);
    long chars = 0;
    for (int i = 0; i < N; i++) {
        Var c = v_copy(&bag[i]);
        CHECK(v_cmp(&c, &bag[i]) == 0 && v_hash(&c) == v_hash(&bag[i]));
        Buf b = { NULL, 0, 0 };
        v_print(&bag[i], &b);
        const char *p = b.s;
        Var parsed = v_parse(&p);
        CHECK(*p == 0);
        CHECK(v_cmp(&parsed, &bag[i]) == 0);
        chars += (long)b.n;
        xf(b.s);
        v_free(&parsed);
        v_free(&c);
    }
    printf("copy, hash and print/parse round trip verified, %ld characters printed\n", chars);
    /* sorting with a total order: checks transitivity indirectly and dedupes */
    qsort(bag, N, sizeof(Var), cmp_qs);
    int distinct = 1;
    for (int i = 1; i < N; i++) {
        CHECK(v_cmp(&bag[i - 1], &bag[i]) <= 0);
        if (v_cmp(&bag[i - 1], &bag[i]) != 0) distinct++;
    }
    for (int i = 0; i + 2 < N; i += 37) CHECK(v_cmp(&bag[i], &bag[i + 2]) <= 0);
    printf("sorted: %d distinct of %d\n", distinct, N);
    for (int i = 0; i < N; i += 60) {
        Buf b = { NULL, 0, 0 };
        v_print(&bag[i], &b);
        printf("  #%3d %s\n", i, b.s);
        xf(b.s);
    }
    for (int i = 0; i < N; i++) v_free(&bag[i]);
    xf(bag);
    CHECK(live_allocs == 0);
    printf("no leaks: %ld live allocations\n", live_allocs);
    return 0;
}
