/*
 * title: Splay-tree rope for text editing
 * topic: data_structures
 * covers: rope, splay tree, split and merge by index, lazy reversal, insert/delete/substring, character model cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC 20000

typedef struct SN {
    struct SN *l, *r, *p;
    int size, rev;
    unsigned char ch;
} SN;

static unsigned long long rs = 0x5B1A70ULL * 4093;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int sz(const SN *x) { return x ? x->size : 0; }
static void pull(SN *x) { x->size = 1 + sz(x->l) + sz(x->r); if (x->l) x->l->p = x; if (x->r) x->r->p = x; }
static void push(SN *x) {
    if (x->rev) {
        SN *t = x->l; x->l = x->r; x->r = t;
        if (x->l) x->l->rev ^= 1;
        if (x->r) x->r->rev ^= 1;
        x->rev = 0;
    }
}
static void rotate(SN *x) {
    SN *p = x->p, *g = p->p;
    if (g) { if (g->l == p) g->l = x; else g->r = x; }
    x->p = g;
    if (p->l == x) { p->l = x->r; if (x->r) x->r->p = p; x->r = p; }
    else { p->r = x->l; if (x->l) x->l->p = p; x->l = p; }
    p->p = x;
    pull(p); pull(x);
    x->p = g;
}
static void push_path(SN *x) { if (x->p) push_path(x->p); push(x); }
static SN *splay(SN *x) {
    push_path(x);
    while (x->p) {
        SN *p = x->p, *g = p->p;
        if (g) rotate(((g->l == p) == (p->l == x)) ? p : x);
        rotate(x);
    }
    return x;
}
/* node holding index k (0-based) splayed to the root */
static SN *find_kth(SN *root, int k) {
    SN *x = root;
    for (;;) {
        push(x);
        int ls = sz(x->l);
        if (k < ls) x = x->l;
        else if (k == ls) return splay(x);
        else { k -= ls + 1; x = x->r; }
    }
}
/* split first k characters into *a, rest into *b */
static void split(SN *root, int k, SN **a, SN **b) {
    if (!root) { *a = *b = NULL; return; }
    if (k <= 0) { *a = NULL; *b = root; return; }
    if (k >= root->size) { *a = root; *b = NULL; return; }
    SN *x = find_kth(root, k);
    *a = x->l; if (x->l) x->l->p = NULL;
    x->l = NULL; pull(x); x->p = NULL;
    *b = x;
}
static SN *merge(SN *a, SN *b) {
    if (!a) return b;
    if (!b) return a;
    SN *m = find_kth(a, a->size - 1);
    m->r = b; b->p = m; pull(m); m->p = NULL;
    return m;
}
static SN *build(const char *s, int n, SN *parent) { /* balanced build */
    if (n <= 0) return NULL;
    int mid = n / 2;
    SN *x = calloc(1, sizeof *x);
    x->ch = (unsigned char)s[mid]; x->p = parent;
    x->l = build(s, mid, x); x->r = build(s + mid + 1, n - mid - 1, x);
    pull(x); x->p = parent;
    return x;
}
static void destroy(SN *x) { if (x) { destroy(x->l); destroy(x->r); free(x); } }
static void flatten(SN *x, char *out, int *n) {
    if (!x) return;
    push(x);
    flatten(x->l, out, n); out[(*n)++] = (char)x->ch; flatten(x->r, out, n);
}
static int depth(const SN *x) { if (!x) return 0; int a = depth(x->l), b = depth(x->r); return 1 + (a > b ? a : b); }
static void verify(const SN *x, const SN *parent) {
    if (!x) return;
    check(x->p == parent, "parent pointer");
    check(x->size == 1 + sz(x->l) + sz(x->r), "size field");
    verify(x->l, x); verify(x->r, x);
}

/* rope operations */
static SN *rope_insert(SN *r, int pos, const char *s) { SN *a, *b; split(r, pos, &a, &b); return merge(merge(a, build(s, (int)strlen(s), NULL)), b); }
static SN *rope_delete(SN *r, int pos, int len, char *removed) {
    SN *a, *b, *c;
    split(r, pos, &a, &b);
    split(b, len, &b, &c);
    if (removed) { int n = 0; flatten(b, removed, &n); removed[n] = 0; }
    destroy(b);
    return merge(a, c);
}
static SN *rope_reverse(SN *r, int pos, int len) {
    SN *a, *b, *c;
    split(r, pos, &a, &b); split(b, len, &b, &c);
    if (b) b->rev ^= 1;
    return merge(merge(a, b), c);
}
static SN *rope_substr(SN *r, int pos, int len, char *out) {
    SN *a, *b, *c;
    split(r, pos, &a, &b); split(b, len, &b, &c);
    int n = 0; flatten(b, out, &n); out[n] = 0;
    return merge(merge(a, b), c);
}

static char model[MAXC + 512];
static int mlen;

static void rand_text(char *o, int n) { for (int i = 0; i < n; i++) o[i] = (char)('a' + rnd() % 26); o[n] = 0; }

int main(void) {
    char init[] = "the quick brown fox jumps over the lazy dog";
    SN *r = build(init, (int)strlen(init), NULL);
    strcpy(model, init); mlen = (int)strlen(init);
    int ins = 0, del = 0, rev = 0, sub = 0, maxlen = mlen, maxdepth = 0;
    long moved = 0;
    for (int step = 0; step < 5000; step++) {
        unsigned op = rnd() % 10;
        if ((op < 4 || mlen < 20) && mlen < MAXC - 300) {
            int pos = (int)(rnd() % (unsigned)(mlen + 1)), n = 1 + (int)(rnd() % 40);
            if (step % 500 == 0) n = 200;
            char t[260]; rand_text(t, n);
            r = rope_insert(r, pos, t);
            memmove(model + pos + n, model + pos, (size_t)(mlen - pos)); memcpy(model + pos, t, (size_t)n); mlen += n;
            ins++; moved += n;
        } else if (op < 6) {
            int pos = (int)(rnd() % (unsigned)mlen), n = 1 + (int)(rnd() % 60);
            if (pos + n > mlen) n = mlen - pos;
            char got[100];
            r = rope_delete(r, pos, n, got);
            check((int)strlen(got) == n && memcmp(got, model + pos, (size_t)n) == 0, "deleted text");
            memmove(model + pos, model + pos + n, (size_t)(mlen - pos - n)); mlen -= n;
            del++; moved += n;
        } else if (op < 8) {
            int pos = (int)(rnd() % (unsigned)mlen), n = 1 + (int)(rnd() % 80);
            if (pos + n > mlen) n = mlen - pos;
            r = rope_reverse(r, pos, n);
            for (int i = 0, j = n - 1; i < j; i++, j--) { char t = model[pos + i]; model[pos + i] = model[pos + j]; model[pos + j] = t; }
            rev++;
        } else {
            int pos = (int)(rnd() % (unsigned)mlen), n = 1 + (int)(rnd() % 100);
            if (pos + n > mlen) n = mlen - pos;
            char got[128];
            r = rope_substr(r, pos, n, got);
            check(memcmp(got, model + pos, (size_t)n) == 0 && got[n] == 0, "substring");
            sub++;
        }
        check(r->size == mlen, "length");
        if (mlen > maxlen) maxlen = mlen;
        if (step % 250 == 249) {
            verify(r, NULL);
            static char flat[MAXC + 512]; int n = 0;
            flatten(r, flat, &n);
            check(n == mlen && memcmp(flat, model, (size_t)mlen) == 0, "whole text matches model");
            int d = depth(r); if (d > maxdepth) maxdepth = d;
            if (step % 1000 == 999) printf("step %4d: length %5d depth %3d prefix \"%.16s\"\n", step + 1, mlen, d, model);
        }
    }
    /* index access by splaying */
    long sum = 0;
    for (int i = 0; i < 300; i++) { int k = (int)(rnd() % (unsigned)mlen); r = find_kth(r, k); check(r->ch == (unsigned char)model[k], "kth character"); sum += r->ch; }
    printf("inserts %d deletes %d reverses %d substrings %d, chars touched %ld\n", ins, del, rev, sub, moved);
    printf("max length %d, final length %d, max sampled depth %d, kth checksum %ld\n", maxlen, mlen, maxdepth, sum);
    destroy(r);
    return 0;
}
