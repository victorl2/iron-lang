/*
 * title: JSON tree parser, pretty printer and random round trip
 * topic: io_files
 * covers: json, recursive descent, pretty print, canonical compact, depth limit, generated documents
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

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct J {
    int t;
    double num;
    char *s;
    struct J **kids;
    char **keys;
    int n;
} J;

typedef struct { const char *p; const char *end; int depth; const char *err; } Parser;

static J *jnew(int t) {
    J *j = calloc(1, sizeof *j);
    if (!j) fail("oom");
    j->t = t;
    return j;
}
static void jfree(J *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) {
        jfree(j->kids[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->kids); free(j->keys); free(j->s); free(j);
}
static void jadd(J *c, const char *key, J *v) {
    c->kids = realloc(c->kids, (size_t)(c->n + 1) * sizeof *c->kids);
    if (c->t == J_OBJ) {
        c->keys = realloc(c->keys, (size_t)(c->n + 1) * sizeof *c->keys);
        size_t k = strlen(key);
        c->keys[c->n] = malloc(k + 1);
        memcpy(c->keys[c->n], key, k + 1);
    }
    c->kids[c->n++] = v;
}

static void skip_ws(Parser *p) {
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')) p->p++;
}

static J *parse_value(Parser *p);

static char *parse_string(Parser *p) {
    Buf b = {0};
    p->p++; /* opening quote */
    while (p->p < p->end && *p->p != '"') {
        unsigned char c = (unsigned char)*p->p++;
        if (c < 0x20) { p->err = "control char in string"; bfree(&b); return NULL; }
        if (c == '\\') {
            if (p->p >= p->end) { p->err = "dangling escape"; bfree(&b); return NULL; }
            c = (unsigned char)*p->p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '/': case '\\': case '"': break;
            case 'u': {
                unsigned cp = 0;
                if (p->end - p->p < 4) { p->err = "short unicode escape"; bfree(&b); return NULL; }
                for (int i = 0; i < 4; i++) {
                    char h = *p->p++;
                    unsigned d = h >= '0' && h <= '9' ? (unsigned)(h - '0')
                               : h >= 'a' && h <= 'f' ? (unsigned)(h - 'a' + 10)
                               : h >= 'A' && h <= 'F' ? (unsigned)(h - 'A' + 10) : 16u;
                    if (d > 15) { p->err = "bad hex digit"; bfree(&b); return NULL; }
                    cp = cp * 16 + d;
                }
                if (cp < 0x80) bbyte(&b, cp);
                else if (cp < 0x800) { bbyte(&b, 0xC0 | (cp >> 6)); bbyte(&b, 0x80 | (cp & 0x3F)); }
                else { bbyte(&b, 0xE0 | (cp >> 12)); bbyte(&b, 0x80 | ((cp >> 6) & 0x3F)); bbyte(&b, 0x80 | (cp & 0x3F)); }
                continue;
            }
            default: p->err = "bad escape"; bfree(&b); return NULL;
            }
        }
        bbyte(&b, c);
    }
    if (p->p >= p->end) { p->err = "unterminated string"; bfree(&b); return NULL; }
    p->p++;
    bbyte(&b, 0);
    return (char *)b.p;
}

static J *parse_value(Parser *p) {
    skip_ws(p);
    if (p->p >= p->end) { p->err = "unexpected end"; return NULL; }
    char c = *p->p;
    if (c == '{' || c == '[') {
        if (++p->depth > 20) { p->err = "too deep"; return NULL; }
        J *cont = jnew(c == '{' ? J_OBJ : J_ARR);
        char close = c == '{' ? '}' : ']';
        p->p++;
        skip_ws(p);
        if (p->p < p->end && *p->p == close) { p->p++; p->depth--; return cont; }
        for (;;) {
            char *key = NULL;
            skip_ws(p);
            if (c == '{') {
                if (p->p >= p->end || *p->p != '"') { p->err = "expected key"; jfree(cont); return NULL; }
                key = parse_string(p);
                if (!key) { jfree(cont); return NULL; }
                skip_ws(p);
                if (p->p >= p->end || *p->p != ':') { p->err = "expected colon"; free(key); jfree(cont); return NULL; }
                p->p++;
            }
            J *v = parse_value(p);
            if (!v) { free(key); jfree(cont); return NULL; }
            jadd(cont, key ? key : "", v);
            free(key);
            skip_ws(p);
            if (p->p < p->end && *p->p == ',') { p->p++; continue; }
            if (p->p < p->end && *p->p == close) { p->p++; break; }
            p->err = "expected comma or close";
            jfree(cont);
            return NULL;
        }
        p->depth--;
        return cont;
    }
    if (c == '"') {
        char *s = parse_string(p);
        if (!s) return NULL;
        J *j = jnew(J_STR);
        j->s = s;
        return j;
    }
    if (strncmp(p->p, "true", 4) == 0) { p->p += 4; return jnew(J_TRUE); }
    if (strncmp(p->p, "false", 5) == 0) { p->p += 5; return jnew(J_FALSE); }
    if (strncmp(p->p, "null", 4) == 0) { p->p += 4; return jnew(J_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *q = p->p;
        if (*q == '-') q++;
        if (q >= p->end || *q < '0' || *q > '9') { p->err = "bad number"; return NULL; }
        if (*q == '0' && q + 1 < p->end && q[1] >= '0' && q[1] <= '9') { p->err = "leading zero"; return NULL; }
        while (q < p->end && *q >= '0' && *q <= '9') q++;
        if (q < p->end && *q == '.') {
            q++;
            if (q >= p->end || *q < '0' || *q > '9') { p->err = "bad fraction"; return NULL; }
            while (q < p->end && *q >= '0' && *q <= '9') q++;
        }
        if (q < p->end && (*q == 'e' || *q == 'E')) {
            q++;
            if (q < p->end && (*q == '+' || *q == '-')) q++;
            if (q >= p->end || *q < '0' || *q > '9') { p->err = "bad exponent"; return NULL; }
            while (q < p->end && *q >= '0' && *q <= '9') q++;
        }
        char tmp[64];
        size_t k = (size_t)(q - p->p);
        if (k >= sizeof tmp) { p->err = "number too long"; return NULL; }
        memcpy(tmp, p->p, k);
        tmp[k] = 0;
        J *j = jnew(J_NUM);
        j->num = strtod(tmp, NULL);
        p->p = q;
        return j;
    }
    p->err = "unexpected character";
    return NULL;
}

static J *parse_doc(const char *s, const char **err) {
    Parser p = {s, s + strlen(s), 0, NULL};
    J *j = parse_value(&p);
    if (j) {
        skip_ws(&p);
        if (p.p != p.end) { jfree(j); j = NULL; p.err = "trailing garbage"; }
    }
    *err = p.err;
    return j;
}

static void put_str(Buf *b, const char *s) {
    bbyte(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        switch (c) {
        case '"': bstr(b, "\\\""); break;
        case '\\': bstr(b, "\\\\"); break;
        case '\n': bstr(b, "\\n"); break;
        case '\t': bstr(b, "\\t"); break;
        case '\r': bstr(b, "\\r"); break;
        default:
            if (c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", c); bstr(b, esc); }
            else bbyte(b, c);
        }
    }
    bbyte(b, '"');
}

static void emit(Buf *b, const J *j, int indent, int level) {
    char tmp[40];
    switch (j->t) {
    case J_NULL: bstr(b, "null"); break;
    case J_FALSE: bstr(b, "false"); break;
    case J_TRUE: bstr(b, "true"); break;
    case J_NUM:
        if (j->num == (double)(long long)j->num && j->num > -1e15 && j->num < 1e15)
            snprintf(tmp, sizeof tmp, "%lld", (long long)j->num);
        else
            snprintf(tmp, sizeof tmp, "%.9g", j->num);
        bstr(b, tmp);
        break;
    case J_STR: put_str(b, j->s); break;
    default: {
        char open = j->t == J_OBJ ? '{' : '[', close = j->t == J_OBJ ? '}' : ']';
        bbyte(b, (unsigned char)open);
        for (int i = 0; i < j->n; i++) {
            if (i) bbyte(b, ',');
            if (indent) { bbyte(b, '\n'); for (int k = 0; k < (level + 1) * indent; k++) bbyte(b, ' '); }
            if (j->t == J_OBJ) { put_str(b, j->keys[i]); bstr(b, indent ? ": " : ":"); }
            emit(b, j->kids[i], indent, level + 1);
        }
        if (indent && j->n) { bbyte(b, '\n'); for (int k = 0; k < level * indent; k++) bbyte(b, ' '); }
        bbyte(b, (unsigned char)close);
    }
    }
}

static char *render(const J *j, int indent) {
    Buf b = {0};
    emit(&b, j, indent, 0);
    bbyte(&b, 0);
    return (char *)b.p;
}

static J *gen(int depth) {
    uint32_t k = rnd() % (depth > 0 ? 8u : 5u);
    J *j;
    switch (k) {
    case 0: return jnew(J_NULL);
    case 1: return jnew(rnd() & 1 ? J_TRUE : J_FALSE);
    case 2: j = jnew(J_NUM); j->num = (double)((int)(rnd() % 20001) - 10000); return j;
    case 3: j = jnew(J_NUM); j->num = (double)((int)(rnd() % 2001) - 1000) / 8.0; return j;
    case 4: {
        static const char *w[] = {"alpha", "be\"ta", "ga\\mma", "line\nbreak", "tab\t", "", "x y", "\001ctl"};
        j = jnew(J_STR);
        const char *s = w[rnd() % 8];
        j->s = malloc(strlen(s) + 1);
        strcpy(j->s, s);
        return j;
    }
    case 5: case 6: {
        j = jnew(J_ARR);
        uint32_t n = rnd() % 4;
        for (uint32_t i = 0; i < n; i++) jadd(j, "", gen(depth - 1));
        return j;
    }
    default: {
        j = jnew(J_OBJ);
        uint32_t n = 1 + rnd() % 3;
        for (uint32_t i = 0; i < n; i++) {
            char key[16];
            snprintf(key, sizeof key, "k%u", i);
            jadd(j, key, gen(depth - 1));
        }
        return j;
    }
    }
}

static int count_nodes(const J *j) {
    int c = 1;
    for (int i = 0; i < j->n; i++) c += count_nodes(j->kids[i]);
    return c;
}

int main(void) {
    const char *err;
    const char *sample = " {\"name\": \"iron\", \"tags\": [\"a\", \"b\\n\", []], \"n\": -12.5e1, \"ok\": true, \"nil\": null, \"o\": {}} ";
    J *j = parse_doc(sample, &err);
    CHECK(j);
    char *pretty = render(j, 2);
    printf("%s\n", pretty);
    char *compact = render(j, 0);
    printf("%s\n", compact);
    jfree(j);
    free(pretty);
    free(compact);

    int total_nodes = 0, total_bytes = 0;
    for (int i = 0; i < 40; i++) {
        J *g = jnew(J_ARR);
        for (int k = 0; k < 4; k++) jadd(g, "", gen(4));
        char *c1 = render(g, 0);
        char *p1 = render(g, 2);
        wfile("doc.json", p1, strlen(p1));
        size_t len;
        unsigned char *back = rfile("doc.json", &len);
        char *txt = malloc(len + 1);
        memcpy(txt, back, len);
        txt[len] = 0;
        J *h = parse_doc(txt, &err);
        CHECK(h);
        char *c2 = render(h, 0);
        CHECK(strcmp(c1, c2) == 0);
        char *p2 = render(h, 2);
        CHECK(strcmp(p1, p2) == 0);
        total_nodes += count_nodes(g);
        total_bytes += (int)strlen(c1);
        if (i < 3) printf("doc %d: %s\n", i, c1);
        free(c1); free(c2); free(p1); free(p2); free(back); free(txt);
        jfree(g); jfree(h);
    }
    remove("doc.json");
    printf("round trips ok: nodes=%d compact bytes=%d\n", total_nodes, total_bytes);

    static const char *bad[] = {
        "", "[1,]", "{\"a\" 1}", "[01]", "\"abc", "{\"a\":1} x", "[1 2]", "-", "1.", "[1e]",
        "{1:2}", "\"a\\qb\"", "[\"\t\"]", "nul", "[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]"
    };
    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
        J *r = parse_doc(bad[i], &err);
        CHECK(r == NULL);
        printf("reject %2d: %s\n", i, err);
    }
    return 0;
}
