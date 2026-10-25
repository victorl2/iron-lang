/*
 * title: MessagePack subset encoder and decoder with minimal-width encoding
 * topic: io_files
 * covers: messagepack, positive/negative fixint, uint8-64, int8-64, fixstr/str8/str16, bin, fixarray/array16, fixmap/map16, bool, nil, big-endian, canonical width selection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

unsigned char *rfile(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) fail("open for read");
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap);
    if (!b) fail("oom");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) fail("oom");
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}


static uint32_t rng_s = 0x2545F491u;
uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}
/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
void bput(Buf *b, const void *s, size_t k) {
    if (b->n + k > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->n + k) nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p) fail("oom");
        b->cap = nc;
    }
    if (k) memcpy(b->p + b->n, s, k);
    b->n += k;
}
void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* Value tree */
enum { M_NIL, M_BOOL, M_INT, M_UINT, M_STR, M_BIN, M_ARR, M_MAP };
typedef struct M {
    int t;
    int64_t i;
    uint64_t u;
    unsigned char *s;
    size_t slen;
    struct M **kid;   /* array items, or key0 val0 key1 val1 ... for maps */
    int n;            /* number of kids */
} M;

static M *mnew(int t) { M *m = calloc(1, sizeof *m); if (!m) fail("oom"); m->t = t; return m; }
static void mfree(M *m) { for (int i = 0; i < m->n; i++) mfree(m->kid[i]); free(m->kid); free(m->s); free(m); }
static void madd(M *c, M *v) { c->kid = realloc(c->kid, (size_t)(c->n + 1) * sizeof *c->kid); c->kid[c->n++] = v; }
static M *mint(int64_t v) { M *m = mnew(v >= 0 ? M_UINT : M_INT); if (v >= 0) m->u = (uint64_t)v; else m->i = v; return m; }
static M *muint(uint64_t v) { M *m = mnew(M_UINT); m->u = v; return m; }
static M *mstr(int t, const void *s, size_t n) { M *m = mnew(t); m->s = malloc(n + 1); memcpy(m->s, s, n); m->s[n] = 0; m->slen = n; return m; }

static void be(Buf *b, uint64_t v, int bytes) { for (int i = bytes - 1; i >= 0; i--) bbyte(b, (unsigned)((v >> (8 * i)) & 255u)); }

static void enc(Buf *b, const M *m) {
    switch (m->t) {
    case M_NIL: bbyte(b, 0xC0); break;
    case M_BOOL: bbyte(b, m->u ? 0xC3 : 0xC2); break;
    case M_UINT:
        if (m->u < 128) bbyte(b, (unsigned)m->u);
        else if (m->u <= 0xFF) { bbyte(b, 0xCC); be(b, m->u, 1); }
        else if (m->u <= 0xFFFF) { bbyte(b, 0xCD); be(b, m->u, 2); }
        else if (m->u <= 0xFFFFFFFFu) { bbyte(b, 0xCE); be(b, m->u, 4); }
        else { bbyte(b, 0xCF); be(b, m->u, 8); }
        break;
    case M_INT:
        if (m->i >= -32) bbyte(b, (unsigned)(m->i & 0xFF));
        else if (m->i >= -128) { bbyte(b, 0xD0); be(b, (uint64_t)m->i, 1); }
        else if (m->i >= -32768) { bbyte(b, 0xD1); be(b, (uint64_t)m->i, 2); }
        else if (m->i >= -2147483648LL) { bbyte(b, 0xD2); be(b, (uint64_t)m->i, 4); }
        else { bbyte(b, 0xD3); be(b, (uint64_t)m->i, 8); }
        break;
    case M_STR:
        if (m->slen < 32) bbyte(b, 0xA0 | (unsigned)m->slen);
        else if (m->slen < 256) { bbyte(b, 0xD9); be(b, m->slen, 1); }
        else { bbyte(b, 0xDA); be(b, m->slen, 2); }
        bput(b, m->s, m->slen);
        break;
    case M_BIN:
        if (m->slen < 256) { bbyte(b, 0xC4); be(b, m->slen, 1); }
        else { bbyte(b, 0xC5); be(b, m->slen, 2); }
        bput(b, m->s, m->slen);
        break;
    case M_ARR:
        if (m->n < 16) bbyte(b, 0x90 | (unsigned)m->n); else { bbyte(b, 0xDC); be(b, (uint64_t)m->n, 2); }
        for (int i = 0; i < m->n; i++) enc(b, m->kid[i]);
        break;
    default: {
        int pairs = m->n / 2;
        if (pairs < 16) bbyte(b, 0x80 | (unsigned)pairs); else { bbyte(b, 0xDE); be(b, (uint64_t)pairs, 2); }
        for (int i = 0; i < m->n; i++) enc(b, m->kid[i]);
    }
    }
}

typedef struct { const unsigned char *p; size_t n, pos; const char *err; } R;

static int need(R *r, size_t k) { if (r->n - r->pos < k) { r->err = "truncated"; return 0; } return 1; }
static uint64_t rbe(R *r, int bytes) { uint64_t v = 0; for (int i = 0; i < bytes; i++) v = (v << 8) | r->p[r->pos++]; return v; }

static M *dec(R *r, int depth) {
    if (depth > 10) { r->err = "too deep"; return NULL; }
    if (!need(r, 1)) return NULL;
    unsigned c = r->p[r->pos++];
    if (c < 0x80) return muint(c);
    if (c >= 0xE0) return mint((int8_t)(uint8_t)c);
    if (c >= 0xA0 && c <= 0xBF) { size_t l = c & 31u; if (!need(r, l)) return NULL; M *m = mstr(M_STR, r->p + r->pos, l); r->pos += l; return m; }
    if ((c >= 0x90 && c <= 0x9F) || c == 0xDC || (c >= 0x80 && c <= 0x8F) || c == 0xDE) {
        int map = (c >= 0x80 && c <= 0x8F) || c == 0xDE;
        size_t cnt;
        if (c == 0xDC || c == 0xDE) { if (!need(r, 2)) return NULL; cnt = (size_t)rbe(r, 2); } else cnt = c & 15u;
        if (map) cnt *= 2;
        if (cnt > r->n - r->pos) { r->err = "count exceeds input"; return NULL; }
        M *m = mnew(map ? M_MAP : M_ARR);
        for (size_t i = 0; i < cnt; i++) {
            M *k = dec(r, depth + 1);
            if (!k) { mfree(m); return NULL; }
            madd(m, k);
        }
        return m;
    }
    switch (c) {
    case 0xC0: return mnew(M_NIL);
    case 0xC2: case 0xC3: { M *m = mnew(M_BOOL); m->u = c == 0xC3; return m; }
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        int by = 1 << (c - 0xCC);
        if (!need(r, (size_t)by)) return NULL;
        return muint(rbe(r, by));
    }
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int by = 1 << (c - 0xD0);
        if (!need(r, (size_t)by)) return NULL;
        uint64_t u = rbe(r, by);
        int sh = 64 - 8 * by;
        int64_t v = sh ? (int64_t)(u << sh) >> sh : (int64_t)u;
        return mint(v);
    }
    case 0xD9: case 0xDA: case 0xC4: case 0xC5: {
        int lb = (c == 0xD9 || c == 0xC4) ? 1 : 2;
        if (!need(r, (size_t)lb)) return NULL;
        size_t l = (size_t)rbe(r, lb);
        if (!need(r, l)) return NULL;
        M *m = mstr(c >= 0xC4 && c <= 0xC5 ? M_BIN : M_STR, r->p + r->pos, l);
        r->pos += l;
        return m;
    }
    default: r->err = "unsupported type byte"; return NULL;
    }
}

static void dump(const M *m) {
    switch (m->t) {
    case M_NIL: fputs("nil", stdout); break;
    case M_BOOL: fputs(m->u ? "true" : "false", stdout); break;
    case M_INT: printf("%lld", (long long)m->i); break;
    case M_UINT: printf("%llu", (unsigned long long)m->u); break;
    case M_STR: printf("\"%s\"", (char *)m->s); break;
    case M_BIN: printf("bin(%zu)", m->slen); break;
    case M_ARR: putchar('['); for (int i = 0; i < m->n; i++) { if (i) putchar(','); dump(m->kid[i]); } putchar(']'); break;
    default: putchar('{'); for (int i = 0; i < m->n; i += 2) { if (i) putchar(','); dump(m->kid[i]); putchar(':'); dump(m->kid[i + 1]); } putchar('}');
    }
}

static M *gen(int depth) {
    uint32_t k = rnd() % (depth > 0 ? 9u : 6u);
    uint32_t r = rnd();
    switch (k) {
    case 0: return mnew(M_NIL);
    case 1: { M *m = mnew(M_BOOL); m->u = r & 1; return m; }
    case 2: { int sh = (int)(r % 9) * 8; return muint((uint64_t)(r & 0xFF) << (sh > 56 ? 56 : sh)); }
    case 3: { static const int64_t v[] = {-1, -32, -33, -128, -129, -32768, -32769, -2147483648LL, -2147483649LL, -9000000000000LL}; return mint(v[r % 10]); }
    case 4: case 5: {
        unsigned char s[300];
        size_t l = r % 3 == 0 ? 20 + r % 300 : r % 12;
        if (l > 299) l = 299;
        for (size_t i = 0; i < l; i++) s[i] = (unsigned char)('a' + (r + i) % 26);
        return mstr(r & 8 ? M_BIN : M_STR, s, l);
    }
    case 6: case 7: { M *a = mnew(M_ARR); uint32_t n = r % 20 < 2 ? 16 + r % 3 : r % 4; for (uint32_t i = 0; i < n; i++) madd(a, gen(depth - 1)); return a; }
    default: { M *a = mnew(M_MAP); uint32_t n = r % 4; for (uint32_t i = 0; i < n; i++) { char key[8]; snprintf(key, sizeof key, "k%u", i); madd(a, mstr(M_STR, key, strlen(key))); madd(a, gen(depth - 1)); } return a; }
    }
}

int main(void) {
    /* width selection table */
    static const uint64_t us[] = {0, 127, 128, 255, 256, 65535, 65536, 4294967295u, 4294967296u};
    printf("uint sizes:");
    for (int i = 0; i < 9; i++) { Buf b = {0}; M *m = muint(us[i]); enc(&b, m); printf(" %llu->%zu", (unsigned long long)us[i], b.n); mfree(m); bfree(&b); }
    static const int64_t is[] = {-1, -32, -33, -128, -129, -32768, -32769, -2147483648LL, -2147483649LL};
    printf("\nint sizes:");
    for (int i = 0; i < 9; i++) { Buf b = {0}; M *m = mint(is[i]); enc(&b, m); printf(" %lld->%zu", (long long)is[i], b.n); mfree(m); bfree(&b); }
    putchar('\n');
    /* known encodings from the MessagePack spec examples */
    Buf b = {0};
    M *doc = mnew(M_MAP);
    madd(doc, mstr(M_STR, "compact", 7)); { M *t = mnew(M_BOOL); t->u = 1; madd(doc, t); }
    madd(doc, mstr(M_STR, "schema", 6)); madd(doc, muint(0));
    enc(&b, doc);
    static const unsigned char expect[] = {0x82, 0xA7, 'c', 'o', 'm', 'p', 'a', 'c', 't', 0xC3, 0xA6, 's', 'c', 'h', 'e', 'm', 'a', 0x00};
    CHECK(b.n == sizeof expect && !memcmp(b.p, expect, b.n));
    printf("spec example: %zu bytes:", b.n);
    for (size_t i = 0; i < b.n; i++) printf(" %02X", b.p[i]);
    putchar('\n');
    mfree(doc); bfree(&b);
    /* random documents through a file */
    int total = 0, hist[8] = {0};
    for (int i = 0; i < 80; i++) {
        M *g = gen(4);
        Buf e = {0}, e2 = {0};
        enc(&e, g);
        wfile("v.mp", e.p, e.n);
        size_t n;
        unsigned char *raw = rfile("v.mp", &n);
        R r = {raw, n, 0, NULL};
        M *back = dec(&r, 0);
        CHECK(back && r.pos == n);
        enc(&e2, back);
        CHECK(e.n == e2.n && !memcmp(e.p, e2.p, e.n));   /* canonical: re-encoding is byte identical */
        hist[g->t]++;
        total += (int)n;
        if (i < 3) { printf("doc %d (%zu bytes): ", i, n); dump(back); putchar('\n'); }
        mfree(g); mfree(back); bfree(&e); bfree(&e2); free(raw);
    }
    printf("80 documents, %d bytes; root types nil/bool/int/uint/str/bin/arr/map: %d %d %d %d %d %d %d %d\n", total, hist[0], hist[1], hist[2],
           hist[3], hist[4], hist[5], hist[6], hist[7]);
    /* truncation at every point must fail cleanly */
    M *g = mnew(M_ARR);
    madd(g, mint(-40000)); madd(g, mstr(M_STR, "hello world, this is longer than thirty-one bytes!", 50));
    { M *mm = mnew(M_MAP); madd(mm, mstr(M_STR, "k", 1)); madd(mm, muint(70000)); madd(g, mm); }
    Buf e = {0};
    enc(&e, g);
    int rejected = 0;
    for (size_t cut = 0; cut < e.n; cut++) {
        R r = {e.p, cut, 0, NULL};
        M *x = dec(&r, 0);
        if (!x) rejected++; else mfree(x);
    }
    printf("truncations rejected: %d of %zu\n", rejected, e.n);
    CHECK(rejected == (int)e.n);
    static const unsigned char bad[] = {0xC1, 0xDC, 0x00, 0x05, 0x91};
    R r1 = {bad, 1, 0, NULL}, r2 = {bad + 1, 4, 0, NULL};
    CHECK(!dec(&r1, 0) && !dec(&r2, 0));
    printf("invalid: %s / %s\n", r1.err, r2.err);
    mfree(g); bfree(&e);
    remove("v.mp");
    return 0;
}
