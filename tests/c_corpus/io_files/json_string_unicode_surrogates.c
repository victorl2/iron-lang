/*
 * title: JSON string escapes, surrogate pairs and UTF-8
 * topic: io_files
 * covers: json strings, \u escapes, surrogate pairs, UTF-8 encode/validate, ensure-ascii output
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

static inline void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

static inline unsigned char *rfile(const char *name, size_t *len) {
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

/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
static inline void bput(Buf *b, const void *s, size_t k) {
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
static inline void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
static inline void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static inline void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static uint32_t rng_s = 0x2545F491u;
static inline uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}

/* Decode JSON string literals into UTF-8 and encode them back in two styles. */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_utf8(Buf *b, uint32_t cp) {
    if (cp < 0x80) bbyte(b, cp);
    else if (cp < 0x800) { bbyte(b, 0xC0 | (cp >> 6)); bbyte(b, 0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        bbyte(b, 0xE0 | (cp >> 12)); bbyte(b, 0x80 | ((cp >> 6) & 0x3F)); bbyte(b, 0x80 | (cp & 0x3F));
    } else {
        bbyte(b, 0xF0 | (cp >> 18)); bbyte(b, 0x80 | ((cp >> 12) & 0x3F));
        bbyte(b, 0x80 | ((cp >> 6) & 0x3F)); bbyte(b, 0x80 | (cp & 0x3F));
    }
}

static int read_u4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv((unsigned char)s[i]);
        if (h < 0) return 0;
        v = v * 16 + (uint32_t)h;
    }
    *out = v;
    return 1;
}

/* s points just after the opening quote. Returns NULL and sets *why on error. */
static char *decode(const char *s, const char **why) {
    Buf b = {0};
    while (*s != '"') {
        unsigned char c = (unsigned char)*s;
        if (c == 0) { *why = "unterminated"; goto bad; }
        if (c < 0x20) { *why = "raw control"; goto bad; }
        if (c != '\\') { bbyte(&b, c); s++; continue; }
        s++;
        switch (*s) {
        case '"': bbyte(&b, '"'); s++; break;
        case '\\': bbyte(&b, '\\'); s++; break;
        case '/': bbyte(&b, '/'); s++; break;
        case 'b': bbyte(&b, 8); s++; break;
        case 'f': bbyte(&b, 12); s++; break;
        case 'n': bbyte(&b, 10); s++; break;
        case 'r': bbyte(&b, 13); s++; break;
        case 't': bbyte(&b, 9); s++; break;
        case 'u': {
            uint32_t cp, lo;
            if (!read_u4(s + 1, &cp)) { *why = "bad \\u digits"; goto bad; }
            s += 5;
            if (cp >= 0xDC00 && cp <= 0xDFFF) { *why = "lone low surrogate"; goto bad; }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (s[0] != '\\' || s[1] != 'u') { *why = "high surrogate without pair"; goto bad; }
                if (!read_u4(s + 2, &lo)) { *why = "bad \\u digits"; goto bad; }
                if (lo < 0xDC00 || lo > 0xDFFF) { *why = "high surrogate followed by non-low"; goto bad; }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                s += 6;
            }
            put_utf8(&b, cp);
            break;
        }
        default: *why = "unknown escape"; goto bad;
        }
    }
    bbyte(&b, 0);
    return (char *)b.p;
bad:
    bfree(&b);
    return NULL;
}

/* Decode one UTF-8 sequence, strictly. Returns length or 0 if invalid. */
static int next_cp(const unsigned char *s, size_t n, uint32_t *cp) {
    if (n == 0) return 0;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int len; uint32_t min, v;
    if ((s[0] & 0xE0) == 0xC0) { len = 2; v = s[0] & 0x1Fu; min = 0x80; }
    else if ((s[0] & 0xF0) == 0xE0) { len = 3; v = s[0] & 0x0Fu; min = 0x800; }
    else if ((s[0] & 0xF8) == 0xF0) { len = 4; v = s[0] & 0x07u; min = 0x10000; }
    else return 0;
    if (n < (size_t)len) return 0;
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return len;
}

/* ascii_only: emit \uXXXX (with surrogate pairs) for everything non-ASCII */
static void encode(Buf *out, const char *utf8, int ascii_only) {
    const unsigned char *s = (const unsigned char *)utf8;
    size_t n = strlen(utf8);
    char t[16];
    bbyte(out, '"');
    while (n) {
        uint32_t cp;
        int l = next_cp(s, n, &cp);
        CHECK(l > 0);
        if (cp == '"') bstr(out, "\\\"");
        else if (cp == '\\') bstr(out, "\\\\");
        else if (cp == '\n') bstr(out, "\\n");
        else if (cp == '\t') bstr(out, "\\t");
        else if (cp < 0x20) { snprintf(t, sizeof t, "\\u%04x", cp); bstr(out, t); }
        else if (cp < 0x80 || !ascii_only) bput(out, s, (size_t)l);
        else if (cp < 0x10000) { snprintf(t, sizeof t, "\\u%04x", cp); bstr(out, t); }
        else {
            uint32_t v = cp - 0x10000;
            snprintf(t, sizeof t, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            bstr(out, t);
        }
        s += l; n -= (size_t)l;
    }
    bbyte(out, '"');
    bbyte(out, 0);
}

static void show_cps(const char *u) {
    const unsigned char *s = (const unsigned char *)u;
    size_t n = strlen(u);
    while (n) {
        uint32_t cp;
        int l = next_cp(s, n, &cp);
        CHECK(l > 0);
        printf("U+%04X ", (unsigned)cp);
        s += l; n -= (size_t)l;
    }
    putchar('\n');
}

int main(void) {
    static const char *good[] = {
        "hello\"", "caf\\u00e9\"", "\\ud83d\\ude00 smile\"", "\\u20AC5 \\u00A3\"", "tab\\tnl\\n\\/\"",
        "\\uD834\\uDD1E clef\"", "\\u0000x\"", "\\uFFFF\\u0800\\u07ff\""
    };
    for (int i = 0; i < 8; i++) {
        const char *why = "";
        if (i == 6) continue; /* an embedded NUL cannot live in a C string */
        char *d = decode(good[i], &why);
        CHECK(d);
        Buf a = {0}, u = {0};
        encode(&a, d, 0);
        encode(&u, d, 1);
        printf("%d: bytes=%zu ascii=%s\n   cps: ", i, strlen(d), (char *)u.p);
        show_cps(d);
        /* the ascii form must decode back to the identical bytes */
        char *back = decode((char *)u.p + 1, &why);
        CHECK(back && strcmp(back, d) == 0);
        char *back2 = decode((char *)a.p + 1, &why);
        CHECK(back2 && strcmp(back2, d) == 0);
        free(d); free(back); free(back2); bfree(&a); bfree(&u);
    }
    static const char *bad[] = {
        "\\ud800\"", "\\udc00\"", "\\ud800\\u0041\"", "\\ud800x\"", "\\u12g4\"", "\\u12\"",
        "\\x41\"", "abc", "a\tb\"", "\\ud83d\\ud83d\""
    };
    for (int i = 0; i < 10; i++) {
        const char *why = "";
        char *d = decode(bad[i], &why);
        CHECK(d == NULL);
        printf("bad %d: %s\n", i, why);
    }
    /* invalid UTF-8 must be rejected by the strict decoder */
    static const unsigned char inv[][4] = {
        {0xC0, 0x80, 0, 0}, {0xED, 0xA0, 0x80, 0}, {0xF4, 0x90, 0x80, 0x80}, {0xE0, 0x80, 0xAF, 0},
        {0x80, 0, 0, 0}, {0xF8, 0x88, 0x80, 0x80}
    };
    int rejected = 0;
    for (int i = 0; i < 6; i++) {
        uint32_t cp;
        if (next_cp(inv[i], 4, &cp) == 0) rejected++;
    }
    printf("invalid utf-8 rejected: %d/6\n", rejected);
    /* random code points: encode ascii-only, decode, compare */
    int pairs = 0, checked = 0;
    for (int i = 0; i < 300; i++) {
        uint32_t cp = rnd() % 0x110000u;
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        if (cp == 0) continue;
        Buf raw = {0}, enc = {0};
        put_utf8(&raw, cp);
        bbyte(&raw, 0);
        encode(&enc, (char *)raw.p, 1);
        const char *why;
        char *d = decode((char *)enc.p + 1, &why);
        CHECK(d && strcmp(d, (char *)raw.p) == 0);
        if (cp >= 0x10000) pairs++;
        checked++;
        free(d); bfree(&raw); bfree(&enc);
    }
    printf("random code points checked=%d astral pairs=%d\n", checked, pairs);
    /* file round trip */
    const char *why;
    char *d = decode("\\u00e9\\ud83d\\ude00\"", &why);
    CHECK(d);
    wfile("s.json", d, strlen(d));
    size_t len;
    unsigned char *r = rfile("s.json", &len);
    printf("file bytes:");
    for (size_t i = 0; i < len; i++) printf(" %02X", r[i]);
    putchar('\n');
    free(r); free(d);
    remove("s.json");
    return 0;
}
