/*
 * title: TOML subset parser with typed values and tables
 * topic: io_files
 * covers: toml, tables, arrays, integers with underscores, strings, booleans, typed lookups, error lines
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


enum { V_INT, V_BOOL, V_STR, V_ARR, V_FLOAT };

typedef struct {
    char path[48];   /* table.key */
    int type;
    long long i;
    double f;
    char s[64];
    int arr_n;
    long long arr[8];
} Entry;

typedef struct {
    Entry e[40];
    int n;
    char errs[8][64];
    int nerr;
} Doc;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static void err(Doc *d, int line, const char *msg) {
    if (d->nerr < 8) snprintf(d->errs[d->nerr], sizeof d->errs[0], "line %d: %s", line, msg);
    d->nerr++;
}

static int parse_int(const char *s, long long *out) {
    int neg = 0;
    long long v = 0;
    int digits = 0, last_us = 1;
    if (*s == '+' || *s == '-') { neg = *s == '-'; s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'o' || s[1] == 'b')) {
        int base = s[1] == 'x' ? 16 : s[1] == 'o' ? 8 : 2;
        for (s += 2; *s; s++) {
            int d = *s >= '0' && *s <= '9' ? *s - '0'
                  : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
                  : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
            if (*s == '_') continue;
            if (d < 0 || d >= base) return 0;
            v = v * base + d;
            digits++;
        }
        *out = v;
        return digits > 0;
    }
    for (; *s; s++) {
        if (*s == '_') { if (last_us) return 0; last_us = 1; continue; }
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (*s - '0');
        digits++;
        last_us = 0;
    }
    if (last_us || !digits) return 0;
    *out = neg ? -v : v;
    return 1;
}

static int parse_value(Doc *d, Entry *e, char *v, int line) {
    if (*v == '"') {
        size_t l = strlen(v);
        if (l < 2 || v[l - 1] != '"') { err(d, line, "unterminated string"); return 0; }
        v[l - 1] = 0;
        size_t o = 0;
        for (char *c = v + 1; *c && o + 1 < sizeof e->s; c++) {
            if (*c == '\\' && c[1]) {
                c++;
                e->s[o++] = *c == 'n' ? '\n' : *c == 't' ? '\t' : *c;
            } else e->s[o++] = *c;
        }
        e->s[o] = 0;
        e->type = V_STR;
        return 1;
    }
    if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0) {
        e->type = V_BOOL;
        e->i = v[0] == 't';
        return 1;
    }
    if (*v == '[') {
        size_t l = strlen(v);
        if (v[l - 1] != ']') { err(d, line, "unterminated array"); return 0; }
        v[l - 1] = 0;
        e->type = V_ARR;
        e->arr_n = 0;
        char *tok = v + 1;
        while (*tok) {
            char *comma = strchr(tok, ',');
            if (comma) *comma = 0;
            char *item = trim(tok);
            if (*item) {
                if (e->arr_n >= 8) { err(d, line, "array too long"); return 0; }
                if (!parse_int(item, &e->arr[e->arr_n])) { err(d, line, "bad array element"); return 0; }
                e->arr_n++;
            }
            if (!comma) break;
            tok = comma + 1;
        }
        return 1;
    }
    if (parse_int(v, &e->i)) { e->type = V_INT; return 1; }
    if (strchr(v, '.')) {
        char *end;
        double f = strtod(v, &end);
        if (*end == 0) { e->type = V_FLOAT; e->f = f; return 1; }
    }
    err(d, line, "unrecognized value");
    return 0;
}

static void parse_toml(Doc *d, const char *text) {
    char table[32] = "";
    int line = 0;
    while (*text) {
        const char *nl = strchr(text, '\n');
        size_t l = nl ? (size_t)(nl - text) : strlen(text);
        char buf[160];
        if (l >= sizeof buf) l = sizeof buf - 1;
        memcpy(buf, text, l);
        buf[l] = 0;
        text += (nl ? (size_t)(nl - text) + 1 : strlen(text));
        line++;
        char *t = trim(buf);
        if (*t == 0 || *t == '#') continue;
        if (*t == '[') {
            char *c = strchr(t, ']');
            if (!c) { err(d, line, "bad table header"); continue; }
            *c = 0;
            snprintf(table, sizeof table, "%s", trim(t + 1));
            continue;
        }
        char *eq = strchr(t, '=');
        if (!eq) { err(d, line, "expected key = value"); continue; }
        *eq = 0;
        char *key = trim(t);
        char *val = trim(eq + 1);
        /* strip trailing comment when not inside a string */
        if (*val != '"') { char *h = strchr(val, '#'); if (h) { *h = 0; val = trim(val); } }
        Entry e;
        memset(&e, 0, sizeof e);
        if (table[0]) snprintf(e.path, sizeof e.path, "%s.%s", table, key);
        else snprintf(e.path, sizeof e.path, "%s", key);
        int dup = 0;
        for (int i = 0; i < d->n; i++) if (strcmp(d->e[i].path, e.path) == 0) dup = 1;
        if (dup) { err(d, line, "duplicate key"); continue; }
        if (parse_value(d, &e, val, line) && d->n < 40) d->e[d->n++] = e;
    }
}

static const Entry *find(const Doc *d, const char *path) {
    for (int i = 0; i < d->n; i++) if (strcmp(d->e[i].path, path) == 0) return &d->e[i];
    return NULL;
}

int main(void) {
    const char *toml =
        "# project manifest\n"
        "title = \"Iron \\\"Lang\\\" demo\"\n"
        "version = 12\n"
        "big = 1_000_000\n"
        "mask = 0xFF_00\n"
        "perm = 0o755\n"
        "flags = 0b1010_1010\n"
        "negative = -42\n"
        "\n"
        "[owner]\n"
        "name = \"Tom\\tP\"   \n"
        "active = true\n"
        "score = 98.5   # trailing comment\n"
        "\n"
        "[ports]\n"
        "list = [ 80, 443, 8_080 ]\n"
        "empty = []\n"
        "version = 3\n"
        "version = 4\n"
        "bad = 1__0\n"
        "oops = \"open\n"
        "arr = [1, x]\n"
        "novalue\n"
        "[database.pool]\n"
        "max = 0x10\n";
    wfile("cfg.toml", toml, strlen(toml));
    size_t len;
    unsigned char *raw = rfile("cfg.toml", &len);
    char *text = malloc(len + 1);
    memcpy(text, raw, len);
    text[len] = 0;
    Doc *d = calloc(1, sizeof *d);
    parse_toml(d, text);
    for (int i = 0; i < d->n; i++) {
        const Entry *e = &d->e[i];
        printf("%-14s ", e->path);
        switch (e->type) {
        case V_INT: printf("int   %lld\n", e->i); break;
        case V_BOOL: printf("bool  %s\n", e->i ? "true" : "false"); break;
        case V_FLOAT: printf("float %.2f\n", e->f); break;
        case V_STR: {
            printf("str   \"");
            for (const char *c = e->s; *c; c++) { if (*c == '\t') fputs("<TAB>", stdout); else putchar(*c); }
            puts("\"");
            break;
        }
        default:
            printf("array [");
            for (int k = 0; k < e->arr_n; k++) printf("%s%lld", k ? ", " : "", e->arr[k]);
            puts("]");
        }
    }
    for (int i = 0; i < d->nerr && i < 8; i++) printf("error: %s\n", d->errs[i]);
    CHECK(find(d, "mask")->i == 0xFF00);
    CHECK(find(d, "flags")->i == 170);
    CHECK(find(d, "ports.version") && find(d, "ports.version")->i == 3);
    CHECK(find(d, "database.pool.max")->i == 16);
    CHECK(find(d, "ports.list")->arr_n == 3);
    CHECK(d->nerr == 5);
    printf("entries=%d errors=%d\n", d->n, d->nerr);
    free(d); free(text); free(raw);
    remove("cfg.toml");
    return 0;
}
