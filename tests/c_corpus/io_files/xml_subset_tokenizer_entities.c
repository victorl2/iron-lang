/*
 * title: XML subset tokenizer with entities and well-formedness
 * topic: io_files
 * covers: xml tokens, attributes, entities, numeric character references, CDATA, comments, tag stack, errors
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

typedef struct { const char *s; size_t pos; const char *err; } Lex;

static int name_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
}

static void put_cp(Buf *b, unsigned cp) {
    if (cp < 0x80) bbyte(b, cp);
    else if (cp < 0x800) { bbyte(b, 0xC0 | (cp >> 6)); bbyte(b, 0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { bbyte(b, 0xE0 | (cp >> 12)); bbyte(b, 0x80 | ((cp >> 6) & 0x3F)); bbyte(b, 0x80 | (cp & 0x3F)); }
    else { bbyte(b, 0xF0 | (cp >> 18)); bbyte(b, 0x80 | ((cp >> 12) & 0x3F)); bbyte(b, 0x80 | ((cp >> 6) & 0x3F)); bbyte(b, 0x80 | (cp & 0x3F)); }
}

/* Decode entities in s[0..n); returns malloc'd string or NULL (err set). */
static char *decode_entities(Lex *lx, const char *s, size_t n) {
    Buf b = {0};
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { bbyte(&b, (unsigned char)s[i]); continue; }
        size_t j = i + 1;
        while (j < n && s[j] != ';') j++;
        if (j >= n) { lx->err = "unterminated entity"; bfree(&b); return NULL; }
        size_t l = j - i - 1;
        const char *e = s + i + 1;
        if (l == 2 && !strncmp(e, "lt", 2)) bbyte(&b, '<');
        else if (l == 2 && !strncmp(e, "gt", 2)) bbyte(&b, '>');
        else if (l == 3 && !strncmp(e, "amp", 3)) bbyte(&b, '&');
        else if (l == 4 && !strncmp(e, "quot", 4)) bbyte(&b, '"');
        else if (l == 4 && !strncmp(e, "apos", 4)) bbyte(&b, '\'');
        else if (l >= 2 && e[0] == '#') {
            unsigned cp = 0;
            int hex = e[1] == 'x';
            size_t k = hex ? 2 : 1;
            if (k >= l) { lx->err = "empty character reference"; bfree(&b); return NULL; }
            for (; k < l; k++) {
                int d = e[k] >= '0' && e[k] <= '9' ? e[k] - '0'
                      : hex && e[k] >= 'a' && e[k] <= 'f' ? e[k] - 'a' + 10
                      : hex && e[k] >= 'A' && e[k] <= 'F' ? e[k] - 'A' + 10 : -1;
                if (d < 0) { lx->err = "bad character reference"; bfree(&b); return NULL; }
                cp = cp * (hex ? 16u : 10u) + (unsigned)d;
                if (cp > 0x10FFFF) { lx->err = "character reference out of range"; bfree(&b); return NULL; }
            }
            put_cp(&b, cp);
        } else { lx->err = "unknown entity"; bfree(&b); return NULL; }
        i = j;
    }
    bbyte(&b, 0);
    return (char *)b.p;
}

static void print_esc(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", stdout);
        else if (*s == '\t') fputs("\\t", stdout);
        else putchar(*s);
    }
    putchar('"');
}

/* Tokenize the document, printing tokens; returns 0 if well formed. */
static int tokenize(const char *doc, int *ntok) {
    Lex lx = {doc, 0, NULL};
    char stack[8][16];
    int depth = 0, seen_root = 0;
    *ntok = 0;
    while (doc[lx.pos]) {
        if (doc[lx.pos] != '<') {
            size_t st = lx.pos;
            while (doc[lx.pos] && doc[lx.pos] != '<') lx.pos++;
            char *t = decode_entities(&lx, doc + st, lx.pos - st);
            if (!t) goto fail;
            int blank = 1;
            for (char *c = t; *c; c++) if (*c != ' ' && *c != '\n' && *c != '\t') blank = 0;
            if (!blank) {
                if (depth == 0) { lx.err = "text outside root"; free(t); goto fail; }
                fputs("  TEXT ", stdout); print_esc(t); putchar('\n');
                (*ntok)++;
            }
            free(t);
            continue;
        }
        if (!strncmp(doc + lx.pos, "<!--", 4)) {
            const char *e = strstr(doc + lx.pos + 4, "-->");
            if (!e) { lx.err = "unterminated comment"; goto fail; }
            printf("  COMMENT len=%d\n", (int)(e - (doc + lx.pos + 4)));
            lx.pos = (size_t)(e - doc) + 3;
            (*ntok)++;
        } else if (!strncmp(doc + lx.pos, "<![CDATA[", 9)) {
            const char *e = strstr(doc + lx.pos + 9, "]]>");
            if (!e) { lx.err = "unterminated CDATA"; goto fail; }
            fputs("  CDATA ", stdout);
            char *t = malloc((size_t)(e - (doc + lx.pos + 9)) + 1);
            memcpy(t, doc + lx.pos + 9, (size_t)(e - (doc + lx.pos + 9)));
            t[e - (doc + lx.pos + 9)] = 0;
            print_esc(t); putchar('\n');
            free(t);
            lx.pos = (size_t)(e - doc) + 3;
            (*ntok)++;
        } else if (!strncmp(doc + lx.pos, "<?", 2)) {
            const char *e = strstr(doc + lx.pos, "?>");
            if (!e) { lx.err = "unterminated PI"; goto fail; }
            printf("  PI\n");
            lx.pos = (size_t)(e - doc) + 2;
            (*ntok)++;
        } else if (doc[lx.pos + 1] == '/') {
            size_t st = lx.pos + 2, e = st;
            while (name_char((unsigned char)doc[e])) e++;
            if (doc[e] != '>') { lx.err = "malformed end tag"; goto fail; }
            if (depth == 0) { lx.err = "unexpected end tag"; goto fail; }
            if (strlen(stack[depth - 1]) != e - st || strncmp(stack[depth - 1], doc + st, e - st)) {
                lx.err = "mismatched end tag"; goto fail;
            }
            printf("  END %s\n", stack[--depth]);
            lx.pos = e + 1;
            (*ntok)++;
        } else {
            size_t st = lx.pos + 1, e = st;
            while (name_char((unsigned char)doc[e])) e++;
            if (e == st) { lx.err = "missing tag name"; goto fail; }
            if (depth == 0 && seen_root) { lx.err = "multiple roots"; goto fail; }
            char name[16];
            snprintf(name, sizeof name, "%.*s", (int)(e - st), doc + st);
            printf("  START %s", name);
            for (;;) {
                while (doc[e] == ' ' || doc[e] == '\n' || doc[e] == '\t') e++;
                if (doc[e] == '>' || (doc[e] == '/' && doc[e + 1] == '>') || !doc[e]) break;
                size_t as = e;
                while (name_char((unsigned char)doc[e])) e++;
                if (e == as || doc[e] != '=' || (doc[e + 1] != '"' && doc[e + 1] != '\'')) {
                    lx.err = "malformed attribute"; putchar('\n'); goto fail;
                }
                char q = doc[e + 1];
                size_t vs = e + 2, ve = vs;
                while (doc[ve] && doc[ve] != q) ve++;
                if (!doc[ve]) { lx.err = "unterminated attribute"; putchar('\n'); goto fail; }
                char *v = decode_entities(&lx, doc + vs, ve - vs);
                if (!v) { putchar('\n'); goto fail; }
                printf(" %.*s=", (int)(e - as), doc + as);
                print_esc(v);
                free(v);
                e = ve + 1;
            }
            int selfclose = doc[e] == '/';
            if (!doc[e]) { lx.err = "unterminated tag"; putchar('\n'); goto fail; }
            e += selfclose ? 2 : 1;
            if (selfclose) printf(" /");
            putchar('\n');
            (*ntok)++;
            if (!selfclose) {
                if (depth >= 8) { lx.err = "nesting too deep"; goto fail; }
                snprintf(stack[depth++], sizeof stack[0], "%s", name);
            }
            seen_root = 1;
            lx.pos = e;
        }
    }
    if (depth) { lx.err = "unclosed element"; goto fail; }
    if (!seen_root) { lx.err = "no root element"; goto fail; }
    return 0;
fail:
    printf("  ERROR at %zu: %s\n", lx.pos, lx.err);
    return 1;
}

int main(void) {
    static const char *docs[] = {
        ("<?xml version=\"1.0\"?>\n<!-- cfg -->\n<root id='7' name=\"a &amp; b\">\n  <item k=\"&lt;x&gt;\">Tom &amp; Jerry &#65;&#x42;&#x20AC;</item>\n"
        "  <empty/>\n  <![CDATA[raw <b> & stuff]]>\n</root>\n"),
        "<a><b></a></b>",
        "<a x=\"1\" x2=y></a>",
        "<a>&bogus;</a>",
        "<a>1 &lt 2</a>",
        "<a/><b/>",
        "<a><b>text</b>",
        "text<a/>",
        "<a>&#1114112;</a>",
        "<r a='it&apos;s'>&#x1F600;</r>"
    };
    int good = 0;
    for (int i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof name, "x%d.xml", i);
        wfile(name, docs[i], strlen(docs[i]));
        size_t len;
        unsigned char *raw = rfile(name, &len);
        char *txt = malloc(len + 1);
        memcpy(txt, raw, len);
        txt[len] = 0;
        printf("doc %d (%zu bytes)\n", i, len);
        int nt;
        int rc = tokenize(txt, &nt);
        if (rc == 0) { printf("  well-formed, %d tokens\n", nt); good++; }
        free(txt); free(raw);
        remove(name);
    }
    CHECK(good == 2);
    return 0;
}
