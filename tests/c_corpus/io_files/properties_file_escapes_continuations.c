/*
 * title: Java .properties reader and writer with escapes and continuations
 * topic: io_files
 * covers: properties format, key separators = : and whitespace, backslash continuation, unicode escapes, comment lines, writer escaping, round trip
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

typedef struct { char *k, *v; } Prop;
typedef struct { Prop p[40]; int n; } Props;

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\f'; }

static void put_utf8(Buf *b, unsigned cp) {
    if (cp < 0x80) bbyte(b, cp);
    else if (cp < 0x800) { bbyte(b, 0xC0 | (cp >> 6)); bbyte(b, 0x80 | (cp & 0x3F)); }
    else { bbyte(b, 0xE0 | (cp >> 12)); bbyte(b, 0x80 | ((cp >> 6) & 0x3F)); bbyte(b, 0x80 | (cp & 0x3F)); }
}

/* Read one logical line into out (raw, escapes intact). Returns 0 at end of input. */
static int logical_line(const char **pp, Buf *out) {
    const char *p = *pp;
    out->n = 0;
    if (!*p) return 0;
    for (;;) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        size_t l = (size_t)(e - p);
        if (l && p[l - 1] == '\r') l--;
        const char *s = p;
        size_t skip = 0;
        if (out->n) while (skip < l && is_ws(s[skip])) skip++;   /* continuation lines lose leading blanks */
        size_t bs = 0;
        while (l > skip + bs && s[l - 1 - bs] == '\\') bs++;
        int cont = bs % 2 == 1;
        bput(out, s + skip, l - skip - (cont ? 1 : 0));
        p = *e ? e + 1 : e;
        if (!cont || !*p) break;
    }
    bbyte(out, 0);
    *pp = p;
    return 1;
}

static int unescape(const char *s, size_t n, Buf *out) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\') { bbyte(out, (unsigned char)s[i]); continue; }
        if (++i >= n) break;
        switch (s[i]) {
        case 't': bbyte(out, '\t'); break;
        case 'n': bbyte(out, '\n'); break;
        case 'r': bbyte(out, '\r'); break;
        case 'f': bbyte(out, '\f'); break;
        case 'u': {
            if (i + 4 >= n) return 0;
            unsigned cp = 0;
            for (int k = 1; k <= 4; k++) {
                int c = s[i + (size_t)k], d;
                d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) return 0;
                cp = cp * 16 + (unsigned)d;
            }
            put_utf8(out, cp);
            i += 4;
            break;
        }
        default: bbyte(out, (unsigned char)s[i]);
        }
    }
    return 1;
}

static void add(Props *ps, Buf *k, Buf *v) {
    for (int i = 0; i < ps->n; i++)
        if (!strcmp(ps->p[i].k, (char *)k->p)) { free(ps->p[i].v); ps->p[i].v = (char *)v->p; v->p = NULL; free(k->p); k->p = NULL; return; }
    CHECK(ps->n < 40);
    ps->p[ps->n].k = (char *)k->p; ps->p[ps->n].v = (char *)v->p;
    ps->n++;
    k->p = v->p = NULL;
}

static int parse(const char *text, Props *ps) {
    Buf line = {0};
    int bad = 0;
    while (logical_line(&text, &line)) {
        const char *s = (const char *)line.p;
        while (is_ws(*s)) s++;
        if (!*s || *s == '#' || *s == '!') continue;
        /* key ends at first unescaped '=', ':' or whitespace */
        const char *e = s;
        while (*e && !((*e == '=' || *e == ':' || is_ws(*e)))) { if (*e == '\\' && e[1]) e++; e++; }
        const char *ke = e;
        while (is_ws(*e)) e++;
        if ((*e == '=' || *e == ':')) { e++; while (is_ws(*e)) e++; }
        Buf k = {0}, v = {0};
        if (!unescape(s, (size_t)(ke - s), &k) || !unescape(e, strlen(e), &v)) { bad++; bfree(&k); bfree(&v); continue; }
        bbyte(&k, 0); bbyte(&v, 0);
        add(ps, &k, &v);
        bfree(&k); bfree(&v);
    }
    bfree(&line);
    return bad;
}

static void write_escaped(Buf *b, const char *s, int is_key) {
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; u[i]; i++) {
        unsigned c = u[i];
        if (c == '\\') bstr(b, "\\\\");
        else if (c == '\t') bstr(b, "\\t");
        else if (c == '\n') bstr(b, "\\n");
        else if (c == '\r') bstr(b, "\\r");
        else if (c == '\f') bstr(b, "\\f");
        else if ((c == ' ' && (is_key || i == 0)) || c == '=' || c == ':' || c == '#' || c == '!') {
            if (!is_key && c != ' ' && i > 0) bbyte(b, c);       /* only leading specials need it in values */
            else { bbyte(b, '\\'); bbyte(b, c); }
        } else if (c >= 0x80) {
            unsigned cp = 0;
            int l = c >= 0xE0 ? 3 : 2;
            cp = c & (l == 3 ? 0x0Fu : 0x1Fu);
            for (int k = 1; k < l; k++) cp = (cp << 6) | (u[i + (size_t)k] & 0x3Fu);
            char t[8];
            snprintf(t, sizeof t, "\\u%04X", cp);
            bstr(b, t);
            i += (size_t)l - 1;
        } else bbyte(b, c);
    }
}

static const char *find(const Props *ps, const char *k) {
    for (int i = 0; i < ps->n; i++) if (!strcmp(ps->p[i].k, k)) return ps->p[i].v;
    return NULL;
}

static void show(const char *s) {
    putchar('<');
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", stdout); else if (*s == '\t') fputs("\\t", stdout); else putchar(*s);
    }
    putchar('>');
}

int main(void) {
    const char *src =
        "# comment line\n"
        "! another comment\n"
        "simple=value\n"
        "  spaced.key   =   padded value  \n"
        "colon.sep: v2\n"
        "space.sep v3 with words\n"
        "empty.value=\n"
        "only.key\n"
        "multi = one \\\n"
        "        two \\\n"
        "   three\n"
        "escaped\\ key\\=x=val\\tTab\\nNL\n"
        "unicode = caf\\u00e9 \\u20AC \\u0041\n"
        "trailing.backslashes = keep\\\\\n"
        "next.line = after\n"
        "dup=first\n"
        "dup=second\n"
        "bad.escape = \\u12G4\n"
        "crlf.line = windows\r\n"
        "last=no newline at end";
    wfile("app.properties", src, strlen(src));
    size_t n;
    unsigned char *raw = rfile("app.properties", &n);
    char *text = malloc(n + 1);
    memcpy(text, raw, n);
    text[n] = 0;
    Props ps = {{{0}}, 0};
    int bad = parse(text, &ps);
    for (int i = 0; i < ps.n; i++) { printf("%2d ", i); show(ps.p[i].k); printf(" = "); show(ps.p[i].v); putchar('\n'); }
    printf("properties=%d bad=%d\n", ps.n, bad);
    CHECK(!strcmp(find(&ps, "multi"), "one two three"));
    CHECK(!strcmp(find(&ps, "dup"), "second"));
    CHECK(!strcmp(find(&ps, "trailing.backslashes"), "keep\\"));
    CHECK(!strcmp(find(&ps, "next.line"), "after"));
    CHECK(!strcmp(find(&ps, "escaped key=x"), "val\tTab\nNL"));
    CHECK(!strcmp(find(&ps, "unicode"), "caf\xC3\xA9 \xE2\x82\xAC A"));
    CHECK(bad == 1 && ps.n == 14);

    /* write, re-read, compare: writer must escape everything the reader treats specially */
    Buf out = {0};
    for (int i = 0; i < ps.n; i++) {
        write_escaped(&out, ps.p[i].k, 1);
        bstr(&out, "=");
        write_escaped(&out, ps.p[i].v, 0);
        bbyte(&out, '\n');
    }
    wfile("out.properties", out.p, out.n);
    size_t n2;
    unsigned char *raw2 = rfile("out.properties", &n2);
    char *text2 = malloc(n2 + 1);
    memcpy(text2, raw2, n2);
    text2[n2] = 0;
    Props p2 = {{{0}}, 0};
    CHECK(parse(text2, &p2) == 0);
    CHECK(p2.n == ps.n);
    for (int i = 0; i < ps.n; i++) CHECK(!strcmp(p2.p[i].k, ps.p[i].k) && !strcmp(p2.p[i].v, ps.p[i].v));
    printf("rewritten file: %zu bytes, round trip identical\n", n2);
    fwrite(raw2, 1, 96, stdout);
    printf("...\n");
    for (int i = 0; i < ps.n; i++) { free(ps.p[i].k); free(ps.p[i].v); }
    for (int i = 0; i < p2.n; i++) { free(p2.p[i].k); free(p2.p[i].v); }
    free(text); free(raw); free(text2); free(raw2); bfree(&out);
    remove("app.properties"); remove("out.properties");
    return 0;
}
