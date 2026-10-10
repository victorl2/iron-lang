/*
 * title: INI file to nested JSON converter
 * topic: io_files
 * covers: ini, sections, dotted section names, comments, type inference, json output, quoted values
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

/* Convert INI text (written to a file and read back) to nested JSON with type inference. */
typedef struct Node {
    char *key;
    char *val;      /* NULL for objects */
    int is_obj;
    struct Node **kid;
    int n;
} Node;

static Node *nnew(const char *key) {
    Node *n = calloc(1, sizeof *n);
    n->key = malloc(strlen(key) + 1);
    strcpy(n->key, key);
    return n;
}

static void nfree(Node *n) {
    for (int i = 0; i < n->n; i++) nfree(n->kid[i]);
    free(n->kid); free(n->key); free(n->val); free(n);
}

static Node *child(Node *p, const char *key, int obj) {
    for (int i = 0; i < p->n; i++)
        if (strcmp(p->kid[i]->key, key) == 0) return p->kid[i];
    p->kid = realloc(p->kid, (size_t)(p->n + 1) * sizeof *p->kid);
    Node *c = nnew(key);
    c->is_obj = obj;
    p->kid[p->n++] = c;
    return c;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

/* Find or create the object for a dotted path like "db.pool.limits". */
static Node *section_node(Node *root, char *path) {
    Node *cur = root;
    char *save = path;
    for (;;) {
        char *dot = strchr(save, '.');
        if (dot) *dot = 0;
        cur = child(cur, save, 1);
        if (!dot) break;
        save = dot + 1;
    }
    return cur;
}

static int parse_ini(const char *text, Node *root, int *warnings) {
    Node *cur = root;
    const char *p = text;
    int lineno = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = malloc(len + 1);
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (e ? 1 : 0);
        lineno++;
        char *t = trim(line);
        if (*t == 0 || *t == ';' || *t == '#') { free(line); continue; }
        if (*t == '[') {
            char *close = strchr(t, ']');
            if (!close || *trim(close + 1) != 0) { (*warnings)++; free(line); continue; }
            *close = 0;
            cur = section_node(root, trim(t + 1));
        } else {
            char *eq = strchr(t, '=');
            if (!eq) { (*warnings)++; free(line); continue; }
            *eq = 0;
            char *k = trim(t), *v = trim(eq + 1);
            size_t vl = strlen(v);
            if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
                /* quoted: keep as string marker \x01 prefix */
                v[vl - 1] = 0;
                Node *c = child(cur, k, 0);
                free(c->val);
                c->val = malloc(vl + 2);
                c->val[0] = '\1';
                strcpy(c->val + 1, v + 1);
            } else {
                char *c2 = strpbrk(v, ";#");
                if (c2 && c2 > v && (c2[-1] == ' ' || c2[-1] == '\t')) { *c2 = 0; v = trim(v); }
                Node *c = child(cur, k, 0);
                free(c->val);
                c->val = malloc(strlen(v) + 1);
                strcpy(c->val, v);
            }
        }
        free(line);
    }
    (void)lineno;
    return 0;
}

static int is_int(const char *s) {
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}
static int is_float(const char *s) {
    int dot = 0, dig = 0;
    if (*s == '-') s++;
    for (; *s; s++) {
        if (*s == '.' && !dot) dot = 1;
        else if (*s >= '0' && *s <= '9') dig++;
        else return 0;
    }
    return dot && dig;
}

static void jstr(Buf *b, const char *s) {
    bbyte(b, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') bbyte(b, '\\');
        bbyte(b, (unsigned char)*s);
    }
    bbyte(b, '"');
}

static void pad(Buf *b, int n) { for (int i = 0; i < n; i++) bstr(b, "  "); }

static void emit(Buf *b, const Node *n, int lvl) {
    bstr(b, "{\n");
    for (int i = 0; i < n->n; i++) {
        const Node *c = n->kid[i];
        pad(b, lvl + 1);
        jstr(b, c->key);
        bstr(b, ": ");
        if (c->is_obj) emit(b, c, lvl + 1);
        else if (c->val[0] == '\1') jstr(b, c->val + 1);
        else if (is_int(c->val) || is_float(c->val)) bstr(b, c->val);
        else if (strcmp(c->val, "true") == 0 || strcmp(c->val, "yes") == 0 || strcmp(c->val, "on") == 0) bstr(b, "true");
        else if (strcmp(c->val, "false") == 0 || strcmp(c->val, "no") == 0 || strcmp(c->val, "off") == 0) bstr(b, "false");
        else if (*c->val == 0) bstr(b, "null");
        else jstr(b, c->val);
        bstr(b, i + 1 < n->n ? ",\n" : "\n");
    }
    pad(b, lvl);
    bstr(b, "}");
}

int main(void) {
    const char *ini =
        "; global settings\n"
        "name = iron-server\n"
        "debug = yes\n"
        "\n"
        "[server]\n"
        "host = 127.0.0.1   ; loopback only\n"
        "port=8080\n"
        "ratio = 0.75\n"
        "banner = \"  hello; not a comment  \"\n"
        "[server.tls]\n"
        "enabled = off\n"
        "cert =\n"
        "[db.pool.limits]\r\n"
        "min = 2\r\n"
        "max = -1\r\n"
        "# trailing comment\n"
        "[server]\n"
        "workers = 4\n"
        "this line has no equals\n"
        "[broken\n"
        "version = 1.2.3\n";
    wfile("app.ini", ini, strlen(ini));
    size_t len;
    unsigned char *raw = rfile("app.ini", &len);
    char *text = malloc(len + 1);
    memcpy(text, raw, len);
    text[len] = 0;
    Node *root = nnew("root");
    root->is_obj = 1;
    int warnings = 0;
    parse_ini(text, root, &warnings);
    Buf out = {0};
    emit(&out, root, 0);
    bbyte(&out, '\n');
    fwrite(out.p, 1, out.n, stdout);
    printf("warnings: %d\n", warnings);
    /* checks */
    Node *srv = child(root, "server", 1);
    CHECK(srv->n == 7);
    CHECK(strcmp(child(srv, "host", 0)->val, "127.0.0.1") == 0);
    CHECK(strcmp(child(srv, "banner", 0)->val + 1, "  hello; not a comment  ") == 0);
    CHECK(child(child(child(root, "db", 1), "pool", 1), "limits", 1)->n == 2);
    CHECK(warnings == 2);
    nfree(root);
    bfree(&out);
    free(text); free(raw);
    remove("app.ini");
    return 0;
}
