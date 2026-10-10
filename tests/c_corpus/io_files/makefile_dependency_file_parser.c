/*
 * title: Makefile-style dependency file parser with topological build order
 * topic: io_files
 * covers: make rules, line continuations, variable expansion, multiple targets, .PHONY, order-only prerequisites, cycle detection, build plan
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

enum { MAXT = 24, MAXD = 10 };

typedef struct {
    char name[32];
    int deps[MAXD], ndeps;
    int order_only[MAXD], nord;
    int phony, has_rule, defined_line;
    int state;      /* 0 white, 1 grey, 2 black */
} Target;

typedef struct { Target t[MAXT]; int n; char var[8][2][48]; int nvar; int cycles; int warnings; } Graph;

static int tid(Graph *g, const char *name) {
    for (int i = 0; i < g->n; i++) if (!strcmp(g->t[i].name, name)) return i;
    CHECK(g->n < MAXT);
    memset(&g->t[g->n], 0, sizeof g->t[0]);
    snprintf(g->t[g->n].name, sizeof g->t[0].name, "%s", name);
    return g->n++;
}

/* expand $(NAME) and $@-free variable refs into dst */
static void expand(const Graph *g, const char *s, char *dst, size_t cap) {
    size_t o = 0;
    while (*s && o + 1 < cap) {
        if (s[0] == '$' && s[1] == '(') {
            const char *close = strchr(s, ')');
            if (close) {
                size_t nl = (size_t)(close - s - 2);
                for (int i = 0; i < g->nvar; i++)
                    if (strlen(g->var[i][0]) == nl && !strncmp(g->var[i][0], s + 2, nl)) {
                        for (const char *v = g->var[i][1]; *v && o + 1 < cap; v++) dst[o++] = *v;
                    }
                s = close + 1;
                continue;
            }
        }
        dst[o++] = *s++;
    }
    dst[o] = 0;
}

static void parse(Graph *g, const char *text) {
    int lineno = 0;
    char logical[400];
    while (*text) {
        /* join continuation lines ending in backslash */
        size_t o = 0;
        int start = lineno + 1;
        for (;;) {
            const char *e = strchr(text, '\n');
            size_t l = e ? (size_t)(e - text) : strlen(text);
            lineno++;
            int cont = l && text[l - 1] == '\\';
            size_t take = cont ? l - 1 : l;
            if (o && o + 1 < sizeof logical) logical[o++] = ' ';
            for (size_t i = 0; i < take && o + 1 < sizeof logical; i++) logical[o++] = text[i];
            text += l + (e ? 1 : 0);
            if (!cont || !*text) break;
        }
        logical[o] = 0;
        if (logical[0] == '\t') continue;   /* recipe line: not needed for the dependency graph */
        char *hash = strchr(logical, '#');
        if (hash) *hash = 0;
        char *s = logical;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;
        char *colon = strchr(s, ':');
        char *eq = strchr(s, '=');
        if (eq && (!colon || eq < colon)) {
            /* variable assignment NAME = value */
            char *k = s, *ke = eq;
            while (ke > k && ke[-1] == ' ') ke--;
            *ke = 0;
            char *v = eq + 1;
            while (*v == ' ') v++;
            size_t l = strlen(v);
            while (l && v[l - 1] == ' ') v[--l] = 0;
            CHECK(g->nvar < 8);
            snprintf(g->var[g->nvar][0], 48, "%s", k);
            expand(g, v, g->var[g->nvar][1], 48);
            g->nvar++;
            continue;
        }
        if (!colon) { g->warnings++; continue; }
        *colon = 0;
        char lhs[200], rhs[300];
        expand(g, s, lhs, sizeof lhs);
        expand(g, colon + 1, rhs, sizeof rhs);
        if (!strcmp(lhs, ".PHONY")) {
            for (char *tok = strtok(rhs, " \t"); tok; tok = strtok(NULL, " \t")) g->t[tid(g, tok)].phony = 1;
            continue;
        }
        int tg[6], ntg = 0;
        for (char *tok = strtok(lhs, " \t"); tok && ntg < 6; tok = strtok(NULL, " \t")) tg[ntg++] = tid(g, tok);
        int deps[MAXD], nd = 0, ord[MAXD], no = 0, after_bar = 0;
        for (char *tok = strtok(rhs, " \t"); tok; tok = strtok(NULL, " \t")) {
            if (!strcmp(tok, "|")) { after_bar = 1; continue; }
            int id = tid(g, tok);
            if (after_bar) { CHECK(no < MAXD); ord[no++] = id; }
            else { CHECK(nd < MAXD); deps[nd++] = id; }
        }
        for (int i = 0; i < ntg; i++) {
            Target *t = &g->t[tg[i]];
            if (t->has_rule && t->ndeps + nd > MAXD) { g->warnings++; continue; }
            t->has_rule = 1;
            t->defined_line = start;
            for (int k = 0; k < nd; k++) t->deps[t->ndeps++] = deps[k];
            for (int k = 0; k < no; k++) t->order_only[t->nord++] = ord[k];
        }
    }
}

static int plan[MAXT], nplan;

static void visit(Graph *g, int i, int *stack, int depth) {
    Target *t = &g->t[i];
    if (t->state == 2) return;
    if (t->state == 1) {
        g->cycles++;
        printf("cycle:");
        int k = depth - 1;
        while (k >= 0 && stack[k] != i) k--;
        for (; k < depth; k++) printf(" %s ->", g->t[stack[k]].name);
        printf(" %s\n", t->name);
        return;
    }
    t->state = 1;
    stack[depth] = i;
    for (int k = 0; k < t->ndeps; k++) visit(g, t->deps[k], stack, depth + 1);
    for (int k = 0; k < t->nord; k++) visit(g, t->order_only[k], stack, depth + 1);
    t->state = 2;
    plan[nplan++] = i;
}

int main(void) {
    const char *mk =
        "# demo makefile\n"
        "CC = gcc\n"
        "OBJS = main.o util.o\n"
        "LIBS = libfoo.a\n"
        "\n"
        ".PHONY: all clean test\n"
        "all: prog docs\n"
        "prog: $(OBJS) $(LIBS)\n"
        "\t$(CC) -o prog $(OBJS)   # recipe lines are ignored\n"
        "main.o util.o: common.h | builddir\n"
        "main.o: main.c \\\n"
        "        config.h \\\n"
        "        util.h\n"
        "util.o: util.c util.h\n"
        "libfoo.a: foo1.o foo2.o\n"
        "foo1.o foo2.o: foo.h\n"
        "docs: README.txt\n"
        "test: prog\n"
        "builddir:\n"
        "\tmkdir builddir\n"
        "this line is not valid\n"
        "clean:\n";
    wfile("Makefile", mk, strlen(mk));
    size_t n;
    unsigned char *raw = rfile("Makefile", &n);
    char *text = malloc(n + 1);
    memcpy(text, raw, n);
    text[n] = 0;
    Graph *g = calloc(1, sizeof *g);
    parse(g, text);
    printf("targets/names: %d, variables: %d, warnings: %d\n", g->n, g->nvar, g->warnings);
    for (int i = 0; i < g->n; i++) {
        Target *t = &g->t[i];
        printf("%-10s%s%s deps:", t->name, t->phony ? " [phony]" : "", t->has_rule ? "" : " [source]");
        for (int k = 0; k < t->ndeps; k++) printf(" %s", g->t[t->deps[k]].name);
        if (t->nord) { printf(" |"); for (int k = 0; k < t->nord; k++) printf(" %s", g->t[t->order_only[k]].name); }
        putchar('\n');
    }
    int stack[MAXT + 1];
    visit(g, tid(g, "all"), stack, 0);
    printf("build order (%d):", nplan);
    for (int i = 0; i < nplan; i++) printf(" %s", g->t[plan[i]].name);
    putchar('\n');
    CHECK(g->cycles == 0);
    /* every target appears after all its prerequisites */
    int pos[MAXT];
    for (int i = 0; i < MAXT; i++) pos[i] = -1;
    for (int i = 0; i < nplan; i++) pos[plan[i]] = i;
    for (int i = 0; i < nplan; i++) {
        Target *t = &g->t[plan[i]];
        for (int k = 0; k < t->ndeps; k++) CHECK(pos[t->deps[k]] >= 0 && pos[t->deps[k]] < i);
        for (int k = 0; k < t->nord; k++) CHECK(pos[t->order_only[k]] >= 0 && pos[t->order_only[k]] < i);
    }
    /* second file with a cycle */
    const char *cyc = "a: b\nb: c d\nc: a\nd:\ne: e\n";
    Graph *g2 = calloc(1, sizeof *g2);
    parse(g2, cyc);
    nplan = 0;
    visit(g2, tid(g2, "a"), stack, 0);
    visit(g2, tid(g2, "e"), stack, 0);
    printf("cycles found: %d, partial order:", g2->cycles);
    for (int i = 0; i < nplan; i++) printf(" %s", g2->t[plan[i]].name);
    putchar('\n');
    CHECK(g2->cycles == 2);
    free(g); free(g2); free(text); free(raw);
    remove("Makefile");
    return 0;
}
