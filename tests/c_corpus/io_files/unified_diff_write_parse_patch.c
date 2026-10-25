/*
 * title: Unified diff generator, parser and patch applier over files
 * topic: io_files
 * covers: unified diff, LCS line diff, hunk headers with context, no newline at end of file marker, fuzz-free patch apply, reverse patch, mismatch detection
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

enum { MAXL = 64 };
typedef struct { char *l[MAXL]; int n; int no_eol; } Lines;   /* no_eol: last line lacks trailing newline */

static void split(const char *text, Lines *o) {
    o->n = 0; o->no_eol = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (!e) o->no_eol = 1;
        CHECK(o->n < MAXL);
        o->l[o->n] = malloc(l + 1);
        memcpy(o->l[o->n], p, l);
        o->l[o->n][l] = 0;
        o->n++;
        p += l + (e ? 1 : 0);
    }
}
static void lines_free(Lines *l) { for (int i = 0; i < l->n; i++) free(l->l[i]); l->n = 0; }

static void join(const Lines *l, Buf *o) {
    for (int i = 0; i < l->n; i++) {
        bstr(o, l->l[i]);
        if (i + 1 < l->n || !l->no_eol) bbyte(o, '\n');
    }
}

/* edit script via LCS table: ops ' ', '-', '+' */
typedef struct { char op; int a, b; } Op;

static int diff_ops(const Lines *a, const Lines *b, Op *ops) {
    static int lcs[MAXL + 1][MAXL + 1];
    for (int i = a->n; i >= 0; i--)
        for (int j = b->n; j >= 0; j--) {
            if (i == a->n || j == b->n) lcs[i][j] = 0;
            else if (!strcmp(a->l[i], b->l[j])) lcs[i][j] = lcs[i + 1][j + 1] + 1;
            else lcs[i][j] = lcs[i + 1][j] >= lcs[i][j + 1] ? lcs[i + 1][j] : lcs[i][j + 1];
        }
    int i = 0, j = 0, k = 0;
    while (i < a->n || j < b->n) {
        if (i < a->n && j < b->n && !strcmp(a->l[i], b->l[j])) { ops[k++] = (Op){' ', i, j}; i++; j++; }
        else if (i < a->n && (j == b->n || lcs[i + 1][j] >= lcs[i][j + 1])) { ops[k++] = (Op){'-', i, j}; i++; }
        else { ops[k++] = (Op){'+', i, j}; j++; }
    }
    return k;
}

static int write_unified(Buf *o, const Lines *a, const Lines *b, int ctx) {
    Op ops[2 * MAXL + 2];
    int n = diff_ops(a, b, ops);
    int hunks = 0;
    bstr(o, "--- a/file.txt\n+++ b/file.txt\n");
    int i = 0;
    while (i < n) {
        while (i < n && ops[i].op == ' ') i++;
        if (i >= n) break;
        /* hunk covers from ctx lines before the first change to ctx after the last, merging close changes */
        int start = i - ctx < 0 ? 0 : i - ctx;
        int end = i, last_change = i;
        while (end < n) {
            if (ops[end].op != ' ') last_change = end;
            if (end - last_change > 2 * ctx) break;
            end++;
        }
        end = last_change + 1 + ctx;
        if (end > n) end = n;
        int as = 0, bs = 0, ac = 0, bc = 0;
        for (int k = start; k < end; k++) {
            if (k == start) { as = ops[k].a; bs = ops[k].b; }
            if (ops[k].op != '+') ac++;
            if (ops[k].op != '-') bc++;
        }
        char h[80];
        snprintf(h, sizeof h, "@@ -%d,%d +%d,%d @@\n", ac ? as + 1 : as, ac, bc ? bs + 1 : bs, bc);
        bstr(o, h);
        for (int k = start; k < end; k++) {
            bbyte(o, (unsigned char)ops[k].op);
            const char *text = ops[k].op == '+' ? b->l[ops[k].b] : a->l[ops[k].a];
            bstr(o, text);
            bbyte(o, '\n');
            int is_last_a = ops[k].op != '+' && ops[k].a == a->n - 1 && a->no_eol;
            int is_last_b = ops[k].op != '-' && ops[k].b == b->n - 1 && b->no_eol;
            if ((ops[k].op == '-' && is_last_a) || (ops[k].op == '+' && is_last_b) || (ops[k].op == ' ' && (is_last_a || is_last_b)))
                bstr(o, "\\ No newline at end of file\n");
        }
        hunks++;
        i = end;
    }
    return hunks;
}

/* apply a unified diff (forward or reversed) to src; returns 0 on success or an error text */
static const char *apply_patch(const char *patch, const Lines *src, Lines *dst, int reverse, int *hunks_applied) {
    dst->n = 0; dst->no_eol = 0;
    char last_op = ' ';
    *hunks_applied = 0;
    int pos = 0;    /* next source line to copy */
    const char *p = strstr(patch, "@@");
    while (p && *p == '@') {
        int as, ac, bs, bc;
        if (sscanf(p, "@@ -%d,%d +%d,%d @@", &as, &ac, &bs, &bc) != 4) return "bad hunk header";
        if (reverse) { int t = as; as = bs; bs = t; t = ac; ac = bc; bc = t; }
        (void)bs; (void)bc;
        int start = ac ? as - 1 : as;
        if (start < pos) return "overlapping hunks";
        while (pos < start) { CHECK(dst->n < MAXL); dst->l[dst->n++] = strdup(src->l[pos]); pos++; }
        p = strchr(p, '\n') + 1;
        while (*p && *p != '@') {
            char op = *p;
            const char *e = strchr(p, '\n');
            size_t l = (size_t)(e - p) - 1;
            char tmp[128];
            memcpy(tmp, p + 1, l);
            tmp[l] = 0;
            p = e + 1;
            if (op == '\\') { if (last_op == '+' || last_op == ' ') dst->no_eol = 1; continue; }
            if (reverse) op = op == '+' ? '-' : op == '-' ? '+' : op;
            last_op = op;
            if (op == ' ' || op == '-') {
                if (pos >= src->n || strcmp(src->l[pos], tmp)) { lines_free(dst); return "context mismatch"; }
                if (op == ' ') { dst->l[dst->n++] = strdup(tmp); }
                pos++;
            } else if (op == '+') { CHECK(dst->n < MAXL); dst->l[dst->n++] = strdup(tmp); }
            else { lines_free(dst); return "bad hunk line"; }
        }
        (*hunks_applied)++;
    }
    if (pos < src->n) dst->no_eol = src->no_eol;    /* the untouched tail carries the source's ending */
    while (pos < src->n) { dst->l[dst->n++] = strdup(src->l[pos]); pos++; }
    return NULL;
}

int main(void) {
    static const char *words[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima"};
    /* original: 30 lines; edited: a mix of deletions, insertions and replacements */
    Buf ta = {0}, tb = {0};
    char line[40];
    for (int i = 0; i < 30; i++) { snprintf(line, sizeof line, "%s %d\n", words[i % 12], i); bstr(&ta, line); }
    bbyte(&ta, 0);
    for (int i = 0; i < 30; i++) {
        uint32_t r = rnd() % 10;
        if (r == 0) continue;                                       /* delete */
        if (r == 1) { snprintf(line, sizeof line, "%s %d\n", words[i % 12], i); bstr(&tb, line); snprintf(line, sizeof line, "inserted after %d\n", i); bstr(&tb, line); continue; }
        if (r == 2) { snprintf(line, sizeof line, "%s %d CHANGED\n", words[i % 12], i); bstr(&tb, line); continue; }
        snprintf(line, sizeof line, "%s %d\n", words[i % 12], i);
        bstr(&tb, line);
    }
    bstr(&tb, "final line without newline");
    bbyte(&tb, 0);
    wfile("a.txt", ta.p, strlen((char *)ta.p));
    wfile("b.txt", tb.p, strlen((char *)tb.p));
    size_t na, nb;
    unsigned char *ra = rfile("a.txt", &na), *rb = rfile("b.txt", &nb);
    char *sa = malloc(na + 1), *sb = malloc(nb + 1);
    memcpy(sa, ra, na); sa[na] = 0;
    memcpy(sb, rb, nb); sb[nb] = 0;
    Lines A, B;
    split(sa, &A); split(sb, &B);
    printf("a: %d lines, b: %d lines, b has no final newline: %d\n", A.n, B.n, B.no_eol);
    for (int ctx = 0; ctx <= 3; ctx += (ctx == 0 ? 1 : 2)) {
        Buf d = {0};
        int hunks = write_unified(&d, &A, &B, ctx);
        bbyte(&d, 0);
        wfile("ab.patch", d.p, strlen((char *)d.p));
        size_t np;
        unsigned char *rp = rfile("ab.patch", &np);
        char *ps = malloc(np + 1);
        memcpy(ps, rp, np); ps[np] = 0;
        Lines out, back;
        int applied;
        const char *e = apply_patch(ps, &A, &out, 0, &applied);
        CHECK(e == NULL && applied == hunks);
        Buf jo = {0};
        join(&out, &jo);
        CHECK(jo.n == nb && !memcmp(jo.p, sb, nb));
        e = apply_patch(ps, &B, &back, 1, &applied);
        CHECK(e == NULL);
        Buf jb = {0};
        join(&back, &jb);
        CHECK(jb.n == na && !memcmp(jb.p, sa, na));
        int plus = 0, minus = 0;
        for (const char *q = ps; *q; q = strchr(q, '\n') + 1) {
            if (q[0] == '+' && q[1] != '+') plus++;
            if (q[0] == '-' && q[1] != '-') minus++;
        }
        printf("context %d: %d hunks, patch %zu bytes, +%d -%d; forward and reverse apply ok\n", ctx, hunks, np, plus, minus);
        if (ctx == 1) printf("%s", ps);
        /* a patch must refuse to apply to a modified target */
        if (ctx == 1) {
            Lines mod;
            split(sa, &mod);
            free(mod.l[10]);
            mod.l[10] = strdup("tampered line");
            Lines o2;
            e = apply_patch(ps, &mod, &o2, 0, &applied);
            printf("patch against modified file: %s\n", e ? e : "applied?!");
            lines_free(&mod);
            if (!e) lines_free(&o2);
        }
        lines_free(&out); lines_free(&back);
        free(rp); free(ps); bfree(&jo); bfree(&jb); bfree(&d);
    }
    lines_free(&A); lines_free(&B);
    free(ra); free(rb); free(sa); free(sb); bfree(&ta); bfree(&tb);
    remove("a.txt"); remove("b.txt"); remove("ab.patch");
    return 0;
}
