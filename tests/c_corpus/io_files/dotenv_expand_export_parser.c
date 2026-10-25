/*
 * title: .env parser with quoting, export and variable expansion
 * topic: io_files
 * covers: dotenv, single/double quotes, escapes, ${VAR} expansion, defaults, cycles, export prefix
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

typedef struct {
    char key[24];
    char *raw;      /* value after quote processing, before expansion */
    int expand;     /* 0 for single-quoted values */
    char *val;      /* expanded */
    int state;      /* 0 unresolved, 1 resolving, 2 done, 3 cyclic */
} Var;

typedef struct { Var v[32]; int n; int errors; } Env;

static Var *lookup(Env *e, const char *k, size_t n) {
    for (int i = 0; i < e->n; i++)
        if (strlen(e->v[i].key) == n && strncmp(e->v[i].key, k, n) == 0) return &e->v[i];
    return NULL;
}

static char *dup(const char *s, size_t n) {
    char *d = malloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static int valid_key(const char *k) {
    if (!*k || (*k >= '0' && *k <= '9')) return 0;
    for (; *k; k++)
        if (!((*k >= 'A' && *k <= 'Z') || (*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '_')) return 0;
    return 1;
}

static void set(Env *e, const char *k, char *raw, int expand) {
    Var *v = lookup(e, k, strlen(k));
    if (!v) {
        CHECK(e->n < 32);
        v = &e->v[e->n++];
        memset(v, 0, sizeof *v);
        snprintf(v->key, sizeof v->key, "%s", k);
    } else free(v->raw);
    v->raw = raw;
    v->expand = expand;
}

static void parse_env(Env *e, const char *text) {
    while (*text) {
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : strlen(text);
        char *line = dup(text, len);
        text += len + (nl ? 1 : 0);
        char *s = line;
        while (*s == ' ') s++;
        if (*s == '#' || *s == 0) { free(line); continue; }
        if (strncmp(s, "export ", 7) == 0) s += 7;
        char *eq = strchr(s, '=');
        if (!eq) { e->errors++; free(line); continue; }
        *eq = 0;
        char *k = s + 0;
        char *ke = eq;
        while (ke > k && ke[-1] == ' ') *--ke = 0;
        if (!valid_key(k)) { e->errors++; free(line); continue; }
        char *v = eq + 1;
        while (*v == ' ') v++;
        Buf b = {0};
        int expand = 1;
        if (*v == '\'') {
            expand = 0;
            v++;
            while (*v && *v != '\'') bbyte(&b, (unsigned char)*v++);
        } else if (*v == '"') {
            v++;
            while (*v && *v != '"') {
                if (*v == '\\' && v[1]) {
                    v++;
                    bbyte(&b, *v == 'n' ? '\n' : *v == 't' ? '\t' : (unsigned char)*v);
                    v++;
                } else bbyte(&b, (unsigned char)*v++);
            }
        } else {
            char *h = strstr(v, " #");
            if (h) *h = 0;
            size_t l = strlen(v);
            while (l && v[l - 1] == ' ') v[--l] = 0;
            bput(&b, v, l);
        }
        bbyte(&b, 0);
        set(e, k, (char *)b.p, expand);
        free(line);
    }
}

static void resolve(Env *e, Var *v);

static char *expand_text(Env *e, const char *s, Var *self) {
    Buf b = {0};
    while (*s) {
        if (s[0] == '$' && s[1] == '{') {
            const char *close = strchr(s, '}');
            if (!close) { bbyte(&b, (unsigned char)*s++); continue; }
            const char *name = s + 2;
            const char *def = NULL;
            size_t nl = (size_t)(close - name);
            for (const char *c = name; c < close; c++)
                if (c[0] == ':' && c + 1 < close && c[1] == '-') { def = c + 2; nl = (size_t)(c - name); break; }
            Var *r = lookup(e, name, nl);
            (void)self;
            if (r) {
                resolve(e, r);
                if (r->state == 2 && (r->val[0] || !def)) bstr(&b, r->val);
                else if (def) bput(&b, def, (size_t)(close - def));
            } else if (def) bput(&b, def, (size_t)(close - def));
            s = close + 1;
        } else bbyte(&b, (unsigned char)*s++);
    }
    bbyte(&b, 0);
    return (char *)b.p;
}

static void resolve(Env *e, Var *v) {
    if (v->state == 2 || v->state == 3) return;
    if (v->state == 1) { v->state = 3; return; }
    v->state = 1;
    char *out = v->expand ? expand_text(e, v->raw, v) : dup(v->raw, strlen(v->raw));
    if (v->state == 3) { free(out); v->val = dup("", 0); return; }
    v->val = out;
    v->state = 2;
}

int main(void) {
    const char *env =
        "# sample env\n"
        "APP_NAME=iron\n"
        "export HOME_DIR=/srv/${APP_NAME}   # deployed here\n"
        "  LOG_DIR = ${HOME_DIR}/logs\n"
        "QUOTED=\"line1\\nline2\\t\\\"end\\\"\"\n"
        "LITERAL='${APP_NAME} stays $literal'\n"
        "WITH_DEFAULT=${MISSING:-fallback}/x\n"
        "EMPTY=\n"
        "EMPTY_DEFAULT=${EMPTY:-was empty}\n"
        "CYC_A=${CYC_B}a\n"
        "CYC_B=${CYC_A}b\n"
        "SELF=${SELF}\n"
        "9BAD=1\n"
        "no equals sign\n"
        "BAD-KEY=2\n"
        "APP_NAME=iron2\n"
        "URL=http://host:80/p?x=1#frag\n";
    wfile(".env", env, strlen(env));
    size_t len;
    unsigned char *raw = rfile(".env", &len);
    char *text = dup((char *)raw, len);
    Env *e = calloc(1, sizeof *e);
    parse_env(e, text);
    for (int i = 0; i < e->n; i++) resolve(e, &e->v[i]);
    for (int i = 0; i < e->n; i++) {
        Var *v = &e->v[i];
        printf("%-14s %s ", v->key, v->state == 2 ? "ok    " : "cyclic");
        putchar('<');
        for (const char *c = v->val; *c; c++) {
            if (*c == '\n') fputs("\\n", stdout);
            else if (*c == '\t') fputs("\\t", stdout);
            else putchar(*c);
        }
        puts(">");
    }
    printf("vars=%d errors=%d\n", e->n, e->errors);
    CHECK(strcmp(lookup(e, "LOG_DIR", 7)->val, "/srv/iron2/logs") == 0);
    CHECK(lookup(e, "CYC_A", 5)->state == 3 || lookup(e, "CYC_B", 5)->state == 3);
    CHECK(e->errors == 3);
    for (int i = 0; i < e->n; i++) { free(e->v[i].raw); free(e->v[i].val); }
    free(e); free(text); free(raw);
    remove(".env");
    return 0;
}
