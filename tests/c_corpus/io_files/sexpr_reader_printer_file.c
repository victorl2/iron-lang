/*
 * title: S-expression reader and printer with quoting
 * topic: io_files
 * covers: s-expressions, symbols, strings, integers, dotted pairs, quote sugar, comments, canonical form, random trees
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

enum { S_NIL, S_INT, S_SYM, S_STR, S_PAIR };

typedef struct S {
    int t;
    long v;
    char *str;
    struct S *car, *cdr;
} S;

static S *mk(int t) {
    S *s = calloc(1, sizeof *s);
    if (!s) fail("oom");
    s->t = t;
    return s;
}
static S *cons(S *a, S *d) { S *s = mk(S_PAIR); s->car = a; s->cdr = d; return s; }
static S *sym(const char *n) { S *s = mk(S_SYM); s->str = malloc(strlen(n) + 1); strcpy(s->str, n); return s; }
static S *num(long v) { S *s = mk(S_INT); s->v = v; return s; }
static S *str(const char *n) { S *s = mk(S_STR); s->str = malloc(strlen(n) + 1); strcpy(s->str, n); return s; }
static void sfree(S *s) {
    while (s) {
        S *next = s->cdr;
        if (s->t == S_PAIR) sfree(s->car);
        free(s->str);
        free(s);
        s = next;
    }
}

typedef struct { const char *p; const char *err; } R;

static void skip(R *r) {
    for (;;) {
        while (*r->p == ' ' || *r->p == '\n' || *r->p == '\t') r->p++;
        if (*r->p == ';') { while (*r->p && *r->p != '\n') r->p++; }
        else break;
    }
}

static int atom_char(int c) { return c && !strchr(" \n\t()\";'.", c); }

static S *read_expr(R *r);

static S *read_list(R *r) {
    S *head = NULL, **tail = &head;
    for (;;) {
        skip(r);
        if (*r->p == ')') { r->p++; return head ? head : mk(S_NIL); }
        if (!*r->p) { r->err = "missing ')'"; sfree(head); return NULL; }
        if (*r->p == '.' && !atom_char((unsigned char)r->p[1]) && head) {
            r->p++;
            S *last = read_expr(r);
            if (!last) { sfree(head); return NULL; }
            skip(r);
            if (*r->p != ')') { r->err = "expected ')' after dotted tail"; sfree(last); sfree(head); return NULL; }
            r->p++;
            *tail = last;
            return head;
        }
        S *e = read_expr(r);
        if (!e) { sfree(head); return NULL; }
        S *cell = cons(e, NULL);
        *tail = cell;
        tail = &cell->cdr;
        if (!head) head = cell;
    }
}

static S *read_expr(R *r) {
    skip(r);
    char c = *r->p;
    if (!c) { r->err = "unexpected end"; return NULL; }
    if (c == ')') { r->err = "unexpected ')'"; return NULL; }
    if (c == '(') { r->p++; return read_list(r); }
    if (c == '\'') {
        r->p++;
        S *q = read_expr(r);
        if (!q) return NULL;
        return cons(sym("quote"), cons(q, NULL));
    }
    if (c == '"') {
        Buf b = {0};
        r->p++;
        while (*r->p && *r->p != '"') {
            if (*r->p == '\\' && r->p[1]) { r->p++; bbyte(&b, *r->p == 'n' ? '\n' : (unsigned char)*r->p); }
            else bbyte(&b, (unsigned char)*r->p);
            r->p++;
        }
        if (!*r->p) { r->err = "unterminated string"; bfree(&b); return NULL; }
        r->p++;
        bbyte(&b, 0);
        S *s = str((char *)b.p);
        bfree(&b);
        return s;
    }
    const char *st = r->p;
    while (atom_char((unsigned char)*r->p)) r->p++;
    if (r->p == st) { r->err = "unexpected character"; return NULL; }
    char tmp[48];
    snprintf(tmp, sizeof tmp, "%.*s", (int)(r->p - st), st);
    char *end;
    long v = strtol(tmp, &end, 10);
    if (*end == 0 && (tmp[0] != '-' || tmp[1])) return num(v);
    if (strcmp(tmp, "nil") == 0) return mk(S_NIL);
    return sym(tmp);
}

static void print(Buf *b, const S *s) {
    char t[32];
    switch (s->t) {
    case S_NIL: bstr(b, "()"); break;
    case S_INT: snprintf(t, sizeof t, "%ld", s->v); bstr(b, t); break;
    case S_SYM: bstr(b, s->str); break;
    case S_STR:
        bbyte(b, '"');
        for (const char *c = s->str; *c; c++) {
            if (*c == '"' || *c == '\\') bbyte(b, '\\');
            if (*c == '\n') bstr(b, "\\n"); else bbyte(b, (unsigned char)*c);
        }
        bbyte(b, '"');
        break;
    default:
        if (s->car->t == S_SYM && !strcmp(s->car->str, "quote") && s->cdr && s->cdr->t == S_PAIR && !s->cdr->cdr) {
            bbyte(b, '\'');
            print(b, s->cdr->car);
            break;
        }
        bbyte(b, '(');
        for (;;) {
            print(b, s->car);
            if (!s->cdr) break;
            if (s->cdr->t != S_PAIR) { bstr(b, " . "); print(b, s->cdr); break; }
            bbyte(b, ' ');
            s = s->cdr;
        }
        bbyte(b, ')');
    }
}

static S *rand_tree(int d) {
    uint32_t k = rnd() % (d > 0 ? 6u : 3u);
    switch (k) {
    case 0: return num((long)(rnd() % 2001) - 1000);
    case 1: { static const char *n[] = {"foo", "bar-baz", "+", "x1", "<=", "lambda"}; return sym(n[rnd() % 6]); }
    case 2: { static const char *n[] = {"hi", "a b", "q\"uote", "nl\nx", ""}; return str(n[rnd() % 5]); }
    default: {
        uint32_t len = 1 + rnd() % 4;
        S *head = NULL, **tail = &head;
        for (uint32_t i = 0; i < len; i++) {
            S *c = cons(rand_tree(d - 1), NULL);
            *tail = c;
            tail = &c->cdr;
        }
        if (rnd() % 5 == 0) *tail = num((long)(rnd() % 10));   /* improper list */
        return head;
    }
    }
}

static int size(const S *s) {
    int n = 0;
    for (; s; s = s->cdr) { n++; if (s->t == S_PAIR) n += size(s->car) - 0; else break; }
    return n;
}

int main(void) {
    const char *src =
        "; a tiny program\n"
        "(define (fact n)\n"
        "  (if (<= n 1) 1 (* n (fact (- n 1)))))   ; recursion\n"
        "(print \"result:\\n\" 'done (1 . 2) (a b . c) nil '(x y) -17 - -)\n";
    wfile("prog.scm", src, strlen(src));
    size_t len;
    unsigned char *raw = rfile("prog.scm", &len);
    char *txt = malloc(len + 1);
    memcpy(txt, raw, len);
    txt[len] = 0;
    R r = {txt, NULL};
    int forms = 0;
    for (;;) {
        skip(&r);
        if (!*r.p) break;
        S *e = read_expr(&r);
        CHECK(e);
        Buf b = {0};
        print(&b, e);
        bbyte(&b, 0);
        printf("form %d: %s\n", ++forms, (char *)b.p);
        bfree(&b);
        sfree(e);
    }
    free(txt); free(raw);
    remove("prog.scm");

    int total = 0;
    for (int i = 0; i < 60; i++) {
        S *t = rand_tree(4);
        Buf a = {0}, c = {0};
        print(&a, t);
        bbyte(&a, 0);
        R r2 = {(char *)a.p, NULL};
        S *back = read_expr(&r2);
        CHECK(back);
        print(&c, back);
        bbyte(&c, 0);
        CHECK(strcmp((char *)a.p, (char *)c.p) == 0);
        total += size(t) > 0 ? (int)strlen((char *)a.p) : 0;
        if (i < 4) printf("tree %d: %s\n", i, (char *)a.p);
        sfree(t); sfree(back); bfree(&a); bfree(&c);
    }
    printf("random round trips ok, total chars=%d\n", total);
    static const char *bad[] = {"(a b", ")", "(a . )", "(a . b c)", "\"abc", "(. a)", "'"};
    for (int i = 0; i < 7; i++) {
        R rb = {bad[i], NULL};
        S *e = read_expr(&rb);
        if (e) { printf("bad %d parsed?!\n", i); sfree(e); exit(1); }
        printf("bad %d: %s\n", i, rb.err);
    }
    return 0;
}
