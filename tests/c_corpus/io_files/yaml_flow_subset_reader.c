/*
 * title: YAML flow-style subset reader and emitter
 * topic: io_files
 * covers: yaml flow sequences, flow mappings, plain and quoted scalars, nesting, canonical re-emit, errors
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

/* Parse YAML flow syntax ([a, b], {k: v}) from a file, emit canonical JSON-like form. */
typedef struct Y {
    int kind;             /* 0 scalar, 1 seq, 2 map */
    int quoted;
    char *text;           /* scalar text */
    struct Y **kid;       /* seq items or map values */
    char **key;
    int n;
} Y;

typedef struct { const char *s; int pos; const char *err; } P;

static Y *ynew(int kind) {
    Y *y = calloc(1, sizeof *y);
    if (!y) fail("oom");
    y->kind = kind;
    return y;
}
static void yfree(Y *y) {
    if (!y) return;
    for (int i = 0; i < y->n; i++) { yfree(y->kid[i]); if (y->key) free(y->key[i]); }
    free(y->kid); free(y->key); free(y->text); free(y);
}
static void yadd(Y *c, char *key, Y *v) {
    c->kid = realloc(c->kid, (size_t)(c->n + 1) * sizeof *c->kid);
    c->key = realloc(c->key, (size_t)(c->n + 1) * sizeof *c->key);
    c->kid[c->n] = v;
    c->key[c->n] = key;
    c->n++;
}
static void ws(P *p) { while (p->s[p->pos] == ' ' || p->s[p->pos] == '\n' || p->s[p->pos] == '\t') p->pos++; }

static Y *scalar(P *p, const char *stops) {
    Buf b = {0};
    Y *y = ynew(0);
    ws(p);
    if (p->s[p->pos] == '"' || p->s[p->pos] == '\'') {
        char q = p->s[p->pos++];
        for (;;) {
            char c = p->s[p->pos];
            if (!c) { p->err = "unterminated quoted scalar"; bfree(&b); yfree(y); return NULL; }
            p->pos++;
            if (c == q) {
                if (q == '\'' && p->s[p->pos] == '\'') { bbyte(&b, '\''); p->pos++; continue; }
                break;
            }
            if (q == '"' && c == '\\') {
                char e = p->s[p->pos++];
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            bbyte(&b, (unsigned char)c);
        }
        y->quoted = 1;
    } else {
        while (p->s[p->pos] && !strchr(stops, p->s[p->pos])) bbyte(&b, (unsigned char)p->s[p->pos++]);
        while (b.n && (b.p[b.n - 1] == ' ' || b.p[b.n - 1] == '\t' || b.p[b.n - 1] == '\n')) b.n--;
        if (b.n == 0) { p->err = "empty scalar"; bfree(&b); yfree(y); return NULL; }
    }
    bbyte(&b, 0);
    y->text = (char *)b.p;
    return y;
}

static Y *node(P *p, const char *stops);

static Y *node(P *p, const char *stops) {
    ws(p);
    char c = p->s[p->pos];
    if (c == '[' || c == '{') {
        Y *y = ynew(c == '[' ? 1 : 2);
        char close = c == '[' ? ']' : '}';
        p->pos++;
        ws(p);
        if (p->s[p->pos] == close) { p->pos++; return y; }
        for (;;) {
            char *key = NULL;
            if (c == '{') {
                Y *k = scalar(p, ":,}");
                if (!k) { yfree(y); return NULL; }
                key = k->text;
                k->text = NULL;
                yfree(k);
                ws(p);
                if (p->s[p->pos] != ':') { p->err = "expected ':' in mapping"; free(key); yfree(y); return NULL; }
                p->pos++;
            }
            Y *v = node(p, ",]}");
            if (!v) { free(key); yfree(y); return NULL; }
            yadd(y, key, v);
            ws(p);
            if (p->s[p->pos] == ',') { p->pos++; ws(p); if (p->s[p->pos] == close) { p->pos++; return y; } continue; }
            if (p->s[p->pos] == close) { p->pos++; return y; }
            p->err = "expected ',' or closing bracket";
            yfree(y);
            return NULL;
        }
    }
    return scalar(p, stops);
}

static int is_number(const char *s) {
    if (*s == '-') s++;
    if (!*s) return 0;
    int dot = 0;
    for (; *s; s++) {
        if (*s == '.' && !dot) dot = 1;
        else if (*s < '0' || *s > '9') return 0;
    }
    return 1;
}

static void emit(Buf *b, const Y *y) {
    if (y->kind == 0) {
        if (!y->quoted && (is_number(y->text) || strcmp(y->text, "true") == 0 || strcmp(y->text, "false") == 0))
            bstr(b, y->text);
        else if (!y->quoted && (strcmp(y->text, "~") == 0 || strcmp(y->text, "null") == 0)) bstr(b, "null");
        else {
            bbyte(b, '"');
            for (const char *c = y->text; *c; c++) {
                if (*c == '"' || *c == '\\') bbyte(b, '\\');
                if (*c == '\n') bstr(b, "\\n"); else bbyte(b, (unsigned char)*c);
            }
            bbyte(b, '"');
        }
        return;
    }
    bbyte(b, y->kind == 1 ? '[' : '{');
    for (int i = 0; i < y->n; i++) {
        if (i) bstr(b, ", ");
        if (y->kind == 2) { bbyte(b, '"'); bstr(b, y->key[i]); bstr(b, "\": "); }
        emit(b, y->kid[i]);
    }
    bbyte(b, y->kind == 1 ? ']' : '}');
}

static int depth(const Y *y) {
    int d = 0;
    for (int i = 0; i < y->n; i++) { int k = depth(y->kid[i]); if (k > d) d = k; }
    return d + (y->kind != 0);
}

int main(void) {
    static const char *docs[] = {
        "[1, 2.5, -3, true, ~, hello world]",
        "{name: Iron, tags: [a, b, [c, d]], nested: {x: 1, y: {z: deep}}}",
        "{ \"quoted key\": 'it''s', esc: \"tab\\there\", trailing: [1, 2, ], empty: [], e2: {} }",
        "[ {a: 1}, {a: 2, b: [true, false]},\n  plain text with: no colon issue ]",
        "[1, 2",
        "{a 1}",
        "{a: [1, 2}",
        "['open, 2]",
        "[, 1]"
    };
    int ok = 0, failed = 0;
    for (int i = 0; i < 9; i++) {
        char name[16];
        snprintf(name, sizeof name, "d%d.yml", i);
        wfile(name, docs[i], strlen(docs[i]));
        size_t len;
        unsigned char *raw = rfile(name, &len);
        char *txt = malloc(len + 1);
        memcpy(txt, raw, len);
        txt[len] = 0;
        P p = {txt, 0, NULL};
        Y *y = node(&p, "");
        if (y) {
            ws(&p);
            if (txt[p.pos]) { p.err = "trailing content"; yfree(y); y = NULL; }
        }
        if (y) {
            Buf b = {0};
            emit(&b, y);
            bbyte(&b, 0);
            printf("doc %d depth %d: %s\n", i, depth(y), (char *)b.p);
            /* re-parse the emitted text: flow syntax is a JSON superset, so it must stay stable */
            P p2 = {(char *)b.p, 0, NULL};
            Y *y2 = node(&p2, "");
            CHECK(y2);
            Buf b2 = {0};
            emit(&b2, y2);
            bbyte(&b2, 0);
            CHECK(strcmp((char *)b.p, (char *)b2.p) == 0);
            bfree(&b); bfree(&b2); yfree(y2); yfree(y);
            ok++;
        } else {
            printf("doc %d error at offset %d: %s\n", i, p.pos, p.err);
            failed++;
        }
        free(txt); free(raw);
        remove(name);
    }
    CHECK(ok == 4 && failed == 5);
    printf("ok=%d failed=%d\n", ok, failed);
    return 0;
}
