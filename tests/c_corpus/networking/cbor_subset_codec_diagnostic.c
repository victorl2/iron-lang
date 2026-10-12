/*
 * title: CBOR encoder and decoder with diagnostic notation
 * topic: networking
 * covers: RFC 8949 major types, shortest integer heads, half single double float selection, indefinite-length items, tags, UTF-8 validation, well-formedness errors, appendix A vectors, tree round trips
 * deps: libc, libm
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

enum { T_UINT, T_NINT, T_BYTES, T_TEXT, T_ARRAY, T_MAP, T_TAG, T_SIMPLE, T_FLOAT };

typedef struct Node {
    int type, indef;
    uint64_t u;
    double f;
    const unsigned char *s;
    size_t len;
    struct Node *kid, *next; /* children as linked list; a tag has one child */
    size_t count;
} Node;

static Node pool[4096];
static int npool;
static Node *mk(int type) { CHECK(npool < 4096); Node *n = &pool[npool++]; memset(n, 0, sizeof *n); n->type = type; return n; }

/* ---- decoding ---- */
typedef struct { const unsigned char *p, *end; const char *err; int not_shortest; } Rd;

static double half_to_double(unsigned h) {
    unsigned e = (h >> 10) & 31, m = h & 1023;
    double v;
    if (e == 0) v = ldexp((double)m, -24);
    else if (e != 31) v = ldexp((double)(m + 1024), (int)e - 25);
    else v = m ? NAN : INFINITY;
    return (h & 0x8000) ? -v : v;
}

static int utf8_ok(const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        size_t k;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xc2 && c <= 0xdf) { k = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { k = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { k = 3; cp = c & 0x07; }
        else return 0;
        if (i + k >= n) return 0;
        for (size_t j = 1; j <= k; j++) { if ((p[i + j] & 0xc0) != 0x80) return 0; cp = (cp << 6) | (p[i + j] & 0x3f); }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        i += k + 1;
    }
    return 1;
}

static Node *decode(Rd *r, int depth);

static int read_head(Rd *r, unsigned *major, unsigned *ai, uint64_t *arg) {
    if (r->p >= r->end) { r->err = "truncated"; return 0; }
    unsigned b = *r->p++;
    *major = b >> 5; *ai = b & 31;
    if (*ai < 24) { *arg = *ai; return 1; }
    if (*ai > 27) return 1; /* 28..30 reserved, 31 indefinite: caller decides */
    size_t nb = (size_t)1 << (*ai - 24);
    if ((size_t)(r->end - r->p) < nb) { r->err = "truncated"; return 0; }
    uint64_t v = 0;
    for (size_t i = 0; i < nb; i++) v = (v << 8) | *r->p++;
    *arg = v;
    if ((*ai == 24 && v < 24) || (*ai == 25 && v < 256) || (*ai == 26 && v < 65536) || (*ai == 27 && v < 4294967296ull)) r->not_shortest = 1;
    return 1;
}

static Node *decode(Rd *r, int depth) {
    if (depth > 16) { r->err = "too-deep"; return NULL; }
    unsigned major, ai;
    uint64_t arg = 0;
    if (!read_head(r, &major, &ai, &arg)) return NULL;
    if (ai >= 28 && ai <= 30) { r->err = "reserved-additional-info"; return NULL; }
    int indef = ai == 31;
    if (major == 7) {
        if (indef) { r->err = "unexpected-break"; return NULL; }
        Node *n;
        if (ai == 25) { n = mk(T_FLOAT); n->f = half_to_double((unsigned)arg); n->len = 2; return n; }
        if (ai == 26) { float f; uint32_t u = (uint32_t)arg; memcpy(&f, &u, 4); n = mk(T_FLOAT); n->f = (double)f; n->len = 4; return n; }
        if (ai == 27) { n = mk(T_FLOAT); memcpy(&n->f, &arg, 8); n->len = 8; return n; }
        if (ai == 24 && arg < 32) { r->err = "invalid-simple-encoding"; return NULL; }
        n = mk(T_SIMPLE); n->u = arg; return n;
    }
    if (indef && (major <= 1 || major == 6)) { r->err = "indefinite-not-allowed"; return NULL; }
    Node *n;
    switch (major) {
    case 0: n = mk(T_UINT); n->u = arg; return n;
    case 1: n = mk(T_NINT); n->u = arg; return n;
    case 2: case 3:
        n = mk(major == 2 ? T_BYTES : T_TEXT);
        if (indef) {
            n->indef = 1;
            Node **tail = &n->kid;
            for (;;) {
                if (r->p >= r->end) { r->err = "truncated"; return NULL; }
                if (*r->p == 0xff) { r->p++; break; }
                Node *c = decode(r, depth + 1);
                if (!c) return NULL;
                if (c->type != n->type || c->indef) { r->err = "bad-string-chunk"; return NULL; }
                *tail = c; tail = &c->next; n->count++;
            }
            return n;
        }
        if (arg > (uint64_t)(r->end - r->p)) { r->err = "truncated"; return NULL; }
        n->s = r->p; n->len = (size_t)arg; r->p += arg;
        if (major == 3 && !utf8_ok(n->s, n->len)) { r->err = "invalid-utf8"; return NULL; }
        return n;
    case 4: case 5: {
        n = mk(major == 4 ? T_ARRAY : T_MAP);
        n->indef = indef;
        Node **tail = &n->kid;
        uint64_t items = indef ? 0 : (major == 5 ? arg * 2 : arg);
        if (!indef && items > (uint64_t)(r->end - r->p)) { r->err = "truncated"; return NULL; }
        for (uint64_t i = 0; indef || i < items; i++) {
            if (indef) {
                if (r->p >= r->end) { r->err = "truncated"; return NULL; }
                if (*r->p == 0xff) {
                    r->p++;
                    if (major == 5 && (n->count & 1)) { r->err = "odd-map-items"; return NULL; }
                    break;
                }
            }
            Node *c = decode(r, depth + 1);
            if (!c) return NULL;
            *tail = c; tail = &c->next; n->count++;
        }
        if (major == 5) n->count /= 2;
        return n;
    }
    default: /* 6 */
        n = mk(T_TAG); n->u = arg;
        n->kid = decode(r, depth + 1);
        return n->kid ? n : NULL;
    }
}

/* ---- encoding ---- */
typedef struct { unsigned char b[4096]; size_t n; } W;
static void head(W *w, unsigned major, uint64_t v) {
    unsigned m = major << 5;
    if (v < 24) w->b[w->n++] = (unsigned char)(m | v);
    else {
        int nb = v < 256 ? 1 : v < 65536 ? 2 : v < 4294967296ull ? 4 : 8;
        w->b[w->n++] = (unsigned char)(m | (nb == 1 ? 24 : nb == 2 ? 25 : nb == 4 ? 26 : 27));
        for (int i = nb - 1; i >= 0; i--) w->b[w->n++] = (unsigned char)(v >> (8 * i));
    }
}

static int to_half(double d, unsigned *h) {
    float f = (float)d;
    if ((double)f != d || d != d) return 0;
    uint32_t u; memcpy(&u, &f, 4);
    unsigned s = u >> 31, e = (u >> 23) & 255, m = u & 0x7fffff;
    if (d == 0) { *h = s << 15; return 1; }
    if (e == 255) { *h = (s << 15) | 0x7c00; return 1; }
    int ex = (int)e - 127;
    if (ex >= -14 && ex <= 15) { if (m & 0x1fff) return 0; *h = (s << 15) | (unsigned)((ex + 15) << 10) | (m >> 13); return 1; }
    if (ex >= -24 && ex < -14) {
        int sh = -(ex + 1);
        uint32_t full = 0x800000u | m;
        if (full & ((1u << sh) - 1)) return 0;
        *h = (s << 15) | (full >> sh);
        return 1;
    }
    return 0;
}

static void encode(W *w, const Node *n) {
    switch (n->type) {
    case T_UINT: head(w, 0, n->u); break;
    case T_NINT: head(w, 1, n->u); break;
    case T_BYTES: case T_TEXT:
        if (n->indef) { w->b[w->n++] = (unsigned char)((n->type == T_BYTES ? 2 : 3) << 5 | 31); for (const Node *c = n->kid; c; c = c->next) encode(w, c); w->b[w->n++] = 0xff; }
        else { head(w, n->type == T_BYTES ? 2 : 3, n->len); memcpy(w->b + w->n, n->s, n->len); w->n += n->len; }
        break;
    case T_ARRAY: case T_MAP: {
        unsigned mj = n->type == T_ARRAY ? 4 : 5;
        if (n->indef) w->b[w->n++] = (unsigned char)(mj << 5 | 31); else head(w, mj, n->count);
        for (const Node *c = n->kid; c; c = c->next) encode(w, c);
        if (n->indef) w->b[w->n++] = 0xff;
        break;
    }
    case T_TAG: head(w, 6, n->u); encode(w, n->kid); break;
    case T_SIMPLE: if (n->u < 24) head(w, 7, n->u); else { w->b[w->n++] = 0xf8; w->b[w->n++] = (unsigned char)n->u; } break;
    case T_FLOAT: {
        unsigned h;
        if (to_half(n->f, &h)) { w->b[w->n++] = 0xf9; w->b[w->n++] = (unsigned char)(h >> 8); w->b[w->n++] = (unsigned char)h; }
        else if ((double)(float)n->f == n->f) { float f = (float)n->f; uint32_t u; memcpy(&u, &f, 4); w->b[w->n++] = 0xfa; for (int i = 3; i >= 0; i--) w->b[w->n++] = (unsigned char)(u >> (8 * i)); }
        else { uint64_t u; memcpy(&u, &n->f, 8); w->b[w->n++] = 0xfb; for (int i = 7; i >= 0; i--) w->b[w->n++] = (unsigned char)(u >> (8 * i)); }
        break;
    }
    }
}

/* ---- diagnostic notation ---- */
typedef struct { char b[1024]; size_t n; } S;
static void sp(S *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(s->b + s->n, sizeof s->b - s->n, fmt, ap);
    va_end(ap);
    CHECK(k >= 0 && s->n + (size_t)k < sizeof s->b);
    s->n += (size_t)k;
}

static void fmt_double(S *s, double d) {
    if (d != d) { sp(s, "NaN"); return; }
    if (isinf(d)) { sp(s, d < 0 ? "-Infinity" : "Infinity"); return; }
    char buf[40];
    if (d == floor(d) && fabs(d) < 1e16) { snprintf(buf, sizeof buf, "%.1f", d); sp(s, "%s", buf); return; }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (!strpbrk(buf, ".eE")) strcat(buf, ".0");
    else if (strchr(buf, 'e') && !strchr(buf, '.')) { char *e = strchr(buf, 'e'); char tail[16]; snprintf(tail, sizeof tail, "%s", e); snprintf(e, 20, ".0%s", tail); }
    sp(s, "%s", buf);
}

static void diag(S *s, const Node *n) {
    switch (n->type) {
    case T_UINT: sp(s, "%llu", (unsigned long long)n->u); break;
    case T_NINT: if (n->u == UINT64_MAX) sp(s, "-18446744073709551616"); else sp(s, "-%llu", (unsigned long long)n->u + 1); break;
    case T_BYTES: case T_TEXT: {
        int t = n->type == T_TEXT;
        if (n->indef) {
            sp(s, "(_ ");
            for (const Node *c = n->kid; c; c = c->next) { if (c != n->kid) sp(s, ", "); diag(s, c); }
            sp(s, ")");
        } else if (t) {
            sp(s, "\"");
            for (size_t i = 0; i < n->len; i++) {
                unsigned char c = n->s[i];
                if (c == '"' || c == '\\') sp(s, "\\%c", c);
                else if (c < 32) sp(s, "\\u%04x", c);
                else sp(s, "%c", c);
            }
            sp(s, "\"");
        } else {
            sp(s, "h'");
            for (size_t i = 0; i < n->len; i++) sp(s, "%02x", n->s[i]);
            sp(s, "'");
        }
        break;
    }
    case T_ARRAY: case T_MAP: {
        int m = n->type == T_MAP;
        sp(s, "%s%s", m ? "{" : "[", n->indef ? "_ " : "");
        for (const Node *c = n->kid; c; c = c->next) {
            if (c != n->kid) sp(s, ", ");
            diag(s, c);
            if (m) { c = c->next; sp(s, ": "); diag(s, c); }
        }
        sp(s, "%s", m ? "}" : "]");
        break;
    }
    case T_TAG: sp(s, "%llu(", (unsigned long long)n->u); diag(s, n->kid); sp(s, ")"); break;
    case T_SIMPLE:
        if (n->u == 20) sp(s, "false"); else if (n->u == 21) sp(s, "true"); else if (n->u == 22) sp(s, "null"); else if (n->u == 23) sp(s, "undefined");
        else sp(s, "simple(%llu)", (unsigned long long)n->u);
        break;
    case T_FLOAT: fmt_double(s, n->f); break;
    }
}

static size_t unhex(const char *h, unsigned char *o) {
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) { unsigned v; char t[3] = { h[0], h[1], 0 }; sscanf(t, "%x", &v); o[n++] = (unsigned char)v; }
    return n;
}

static Node *rand_tree(int depth);
static uint32_t rs = 0xcb0ecb0eu;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static unsigned char strbuf[64][24];
static int nstr;

static Node *rand_tree(int depth) {
    uint32_t k = rnd() % (depth > 3 ? 6 : 9);
    Node *n;
    switch (k) {
    case 0: case 1: {
        n = mk(k == 0 ? T_UINT : T_NINT);
        uint64_t hi = rnd();
        uint64_t lo = rnd();
        unsigned sh = rnd() % 64;
        n->u = ((hi << 32) | lo) >> sh;
        return n;
    }
    case 2: case 3: {
        n = mk(k == 2 ? T_BYTES : T_TEXT);
        n->len = rnd() % 20;
        unsigned char *b = strbuf[nstr++ % 64];
        for (size_t i = 0; i < n->len; i++) b[i] = (unsigned char)(k == 3 ? 'a' + rnd() % 26 : rnd());
        n->s = b;
        return n;
    }
    case 4: n = mk(T_SIMPLE); n->u = 20 + rnd() % 4; return n;
    case 5: {
        n = mk(T_FLOAT);
        uint32_t c = rnd() % 4;
        n->f = c == 0 ? (double)(int)(rnd() % 2000) / 8.0 : c == 1 ? (double)(float)(rnd() % 100000) / 16.0 : c == 2 ? (double)(rnd() % 1000000) / 7.0 : -(double)(rnd() % 100) / 64.0;
        return n;
    }
    case 6: case 7: {
        n = mk(k == 6 ? T_ARRAY : T_MAP);
        size_t cnt = rnd() % 4;
        n->indef = (int)(rnd() % 4 == 0);
        Node **tail = &n->kid;
        for (size_t i = 0; i < cnt * (k == 7 ? 2 : 1); i++) { Node *c = rand_tree(depth + 1); *tail = c; tail = &c->next; }
        n->count = cnt;
        return n;
    }
    default: n = mk(T_TAG); n->u = rnd() % 1000; n->kid = rand_tree(depth + 1); return n;
    }
}

static int same(const Node *a, const Node *b) {
    if (a->type != b->type || a->indef != b->indef) return 0;
    switch (a->type) {
    case T_UINT: case T_NINT: case T_SIMPLE: return a->u == b->u;
    case T_FLOAT: return a->f == b->f;
    case T_BYTES: case T_TEXT: return a->len == b->len && (!a->len || memcmp(a->s, b->s, a->len) == 0);
    case T_TAG: return a->u == b->u && same(a->kid, b->kid);
    default: {
        if (a->count != b->count) return 0;
        const Node *x = a->kid, *y = b->kid;
        while (x && y) { if (!same(x, y)) return 0; x = x->next; y = y->next; }
        return !x && !y;
    }
    }
}

int main(void) {
    static const struct { const char *hex, *dg; } vec[] = {
        { "00", "0" }, { "01", "1" }, { "0a", "10" }, { "17", "23" }, { "1818", "24" }, { "1819", "25" }, { "1864", "100" }, { "1903e8", "1000" },
        { "1a000f4240", "1000000" }, { "1b000000e8d4a51000", "1000000000000" }, { "1bffffffffffffffff", "18446744073709551615" },
        { "3bffffffffffffffff", "-18446744073709551616" }, { "20", "-1" }, { "29", "-10" }, { "3863", "-100" }, { "3903e7", "-1000" },
        { "f90000", "0.0" }, { "f93c00", "1.0" }, { "f93e00", "1.5" }, { "f97bff", "65504.0" }, { "fa47c35000", "100000.0" },
        { "fa7f7fffff", "3.4028234663852886e+38" }, { "fb3ff199999999999a", "1.1" }, { "fb7e37e43c8800759c", "1.0e+300" },
        { "f90400", "6.103515625e-05" }, { "f9c400", "-4.0" }, { "fbc010666666666666", "-4.1" },
        { "f97c00", "Infinity" }, { "f9fc00", "-Infinity" }, { "f4", "false" }, { "f5", "true" }, { "f6", "null" }, { "f7", "undefined" }, { "f0", "simple(16)" },
        { "f8ff", "simple(255)" }, { "c11a514b67b0", "1(1363896240)" }, { "d74401020304", "23(h'01020304')" }, { "40", "h''" }, { "4401020304", "h'01020304'" },
        { "60", "\"\"" }, { "6161", "\"a\"" }, { "6449455446", "\"IETF\"" }, { "62225c", "\"\\\"\\\\\"" }, { "62c3bc", "\"\xc3\xbc\"" }, { "63e6b0b4", "\"\xe6\xb0\xb4\"" },
        { "80", "[]" }, { "83010203", "[1, 2, 3]" }, { "8301820203820405", "[1, [2, 3], [4, 5]]" }, { "a0", "{}" }, { "a201020304", "{1: 2, 3: 4}" },
        { "a26161016162820203", "{\"a\": 1, \"b\": [2, 3]}" }, { "826161a161626163", "[\"a\", {\"b\": \"c\"}]" },
        { "9f018202039f0405ffff", "[_ 1, [2, 3], [_ 4, 5]]" }, { "bf61610161629f0203ffff", "{_ \"a\": 1, \"b\": [_ 2, 3]}" }, { "5f42010243030405ff", "(_ h'0102', h'030405')" },
        { "7f657374726561646d696e67ff", "(_ \"strea\", \"ming\")" }, { "9f9f9f9fffffffff", "[_ [_ [_ [_ ]]]]" },
    };
    int nv = (int)(sizeof vec / sizeof vec[0]);
    int shortest_roundtrips = 0;
    for (int i = 0; i < nv; i++) {
        unsigned char b[64];
        size_t n = unhex(vec[i].hex, b);
        npool = 0;
        Rd r = { b, b + n, NULL, 0 };
        Node *t = decode(&r, 0);
        if (!t || r.p != r.end) { fprintf(stderr, "vector %s failed: %s\n", vec[i].hex, r.err ? r.err : "trailing"); exit(1); }
        S s = { { 0 }, 0 };
        diag(&s, t);
        if (strcmp(s.b, vec[i].dg) != 0) { fprintf(stderr, "vector %s: got %s want %s\n", vec[i].hex, s.b, vec[i].dg); exit(1); }
        W w = { { 0 }, 0 };
        encode(&w, t);
        int same_bytes = w.n == n && memcmp(w.b, b, n) == 0;
        if (same_bytes) shortest_roundtrips++;
        else CHECK(t->type == T_FLOAT || t->indef || t->type == T_ARRAY || t->type == T_MAP); /* only re-encoding differences allowed are float width choices and NaN */
        printf("%-24s %s\n", vec[i].hex, s.b);
    }
    printf("%d vectors decoded to diagnostic notation, %d re-encoded byte for byte\n", nv, shortest_roundtrips);

    /* Smallest half subnormal is 2^-24; checked numerically rather than by text. */
    { unsigned char b[] = { 0xf9, 0x00, 0x01 }; npool = 0; Rd r = { b, b + 3, NULL, 0 }; Node *t = decode(&r, 0); CHECK(t && t->f == ldexp(1.0, -24)); W w = { { 0 }, 0 }; encode(&w, t); CHECK(w.n == 3 && w.b[2] == 1); }
    /* Errors. */
    static const struct { const char *hex; } bad[] = {
        { "18" }, { "1c" }, { "ff" }, { "5f01ff" }, { "a1" }, { "62c328" }, { "7f4161ff" }, { "bf01ff" }, { "f818" }, { "1f" }, { "8201" }, { "62ed a080" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char hx[32];
        size_t k = 0;
        for (const char *p = bad[i].hex; *p; p++) if (*p != ' ') hx[k++] = *p;
        hx[k] = 0;
        unsigned char b[32];
        size_t n = unhex(hx, b);
        npool = 0;
        Rd r = { b, b + n, NULL, 0 };
        Node *t = decode(&r, 0);
        printf("bad %-12s -> %s\n", hx, t ? "accepted" : r.err);
        CHECK(t == NULL);
    }
    /* Non-shortest heads decode but are flagged. */
    { unsigned char b[] = { 0x18, 0x05 }; npool = 0; Rd r = { b, b + 2, NULL, 0 }; Node *t = decode(&r, 0); CHECK(t && t->u == 5 && r.not_shortest); printf("0x1805 decodes as 5 but is not shortest form: %d\n", r.not_shortest); }
    /* Random trees survive encode then decode. */
    long bytes = 0;
    for (int t = 0; t < 400; t++) {
        npool = 0; nstr = 0;
        Node *tree = rand_tree(0);
        W w = { { 0 }, 0 };
        encode(&w, tree);
        int p1 = npool;
        Rd r = { w.b, w.b + w.n, NULL, 0 };
        Node *back = decode(&r, 0);
        CHECK(back && r.p == r.end && same(tree, back));
        W w2 = { { 0 }, 0 };
        encode(&w2, back);
        CHECK(w2.n == w.n && memcmp(w.b, w2.b, w.n) == 0);
        CHECK(npool >= p1);
        bytes += (long)w.n;
    }
    printf("400 random trees round-trip through bytes (%ld total)\n", bytes);
    return 0;
}
