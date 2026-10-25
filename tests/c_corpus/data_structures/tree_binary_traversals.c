/*
 * title: Plain binary tree traversals, mirror and serialization
 * topic: data_structures
 * covers: binary tree, pre/in/post/level order, recursive and iterative traversal, height, mirror, serialize/deserialize, level-order insert and delete-by-deepest
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 512

typedef struct N {
    int v;
    struct N *l, *r;
} N;

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}

static N *mk(int v) {
    N *n = malloc(sizeof *n);
    if (!n) exit(2);
    n->v = v; n->l = n->r = NULL;
    return n;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static int cnt(N *t) { return t ? 1 + cnt(t->l) + cnt(t->r) : 0; }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}

static int rn;
static int *ro;
static void pre_r(N *t) { if (!t) return; ro[rn++] = t->v; pre_r(t->l); pre_r(t->r); }
static void in_r(N *t) { if (!t) return; in_r(t->l); ro[rn++] = t->v; in_r(t->r); }
static void post_r(N *t) { if (!t) return; post_r(t->l); post_r(t->r); ro[rn++] = t->v; }

static int pre_i(N *t, int *o) {
    N *st[MAXN]; int sp = 0, n = 0;
    if (t) st[sp++] = t;
    while (sp) {
        N *x = st[--sp];
        o[n++] = x->v;
        if (x->r) st[sp++] = x->r;
        if (x->l) st[sp++] = x->l;
    }
    return n;
}
static int in_i(N *t, int *o) {
    N *st[MAXN]; int sp = 0, n = 0;
    N *c = t;
    while (c || sp) {
        while (c) { st[sp++] = c; c = c->l; }
        c = st[--sp];
        o[n++] = c->v;
        c = c->r;
    }
    return n;
}
static int post_i(N *t, int *o) {
    N *st[MAXN]; int sp = 0, n = 0;
    N *c = t, *last = NULL;
    while (c || sp) {
        if (c) { st[sp++] = c; c = c->l; }
        else {
            N *top = st[sp - 1];
            if (top->r && last != top->r) c = top->r;
            else { o[n++] = top->v; last = top; sp--; }
        }
    }
    return n;
}
static int level_i(N *t, int *o) {
    N *q[MAXN]; int h = 0, tl = 0, n = 0;
    if (t) q[tl++] = t;
    while (h < tl) {
        N *x = q[h++];
        o[n++] = x->v;
        if (x->l) q[tl++] = x->l;
        if (x->r) q[tl++] = x->r;
    }
    return n;
}
/* zigzag: level order alternating direction, writes level widths too */
static int zigzag(N *t, int *o) {
    N *cur[MAXN], *nxt[MAXN];
    int cn = 0, n = 0, dir = 0;
    if (t) cur[cn++] = t;
    while (cn) {
        int nn = 0;
        for (int i = 0; i < cn; i++) {
            N *x = dir ? cur[cn - 1 - i] : cur[i];
            o[n++] = x->v;
        }
        for (int i = 0; i < cn; i++) {
            if (cur[i]->l) nxt[nn++] = cur[i]->l;
            if (cur[i]->r) nxt[nn++] = cur[i]->r;
        }
        memcpy(cur, nxt, sizeof(N *) * (size_t)nn);
        cn = nn;
        dir ^= 1;
    }
    return n;
}

static N *build_random(int n) {
    if (n == 0) return NULL;
    int left = (int)(rnd() % (unsigned)n);
    N *t = mk((int)(rnd() % 1000));
    t->l = build_random(left);
    t->r = build_random(n - 1 - left);
    return t;
}
static N *mirror(N *t) {
    if (!t) return NULL;
    N *l = mirror(t->l), *r = mirror(t->r);
    t->l = r; t->r = l;
    return t;
}
static void mirror_iter(N *t) {
    N *q[MAXN]; int h = 0, tl = 0;
    if (t) q[tl++] = t;
    while (h < tl) {
        N *x = q[h++], *tmp = x->l;
        x->l = x->r; x->r = tmp;
        if (x->l) q[tl++] = x->l;
        if (x->r) q[tl++] = x->r;
    }
}
static int same(N *a, N *b) {
    if (!a || !b) return a == b;
    return a->v == b->v && same(a->l, b->l) && same(a->r, b->r);
}
static int ser(N *t, char *b, int pos) {
    if (!t) { b[pos++] = '#'; b[pos++] = ','; return pos; }
    pos += snprintf(b + pos, 16, "%d,", t->v);
    pos = ser(t->l, b, pos);
    return ser(t->r, b, pos);
}
static N *deser(const char *b, int *pos) {
    if (b[*pos] == '#') { *pos += 2; return NULL; }
    int v = 0;
    while (b[*pos] != ',') { v = v * 10 + (b[*pos] - '0'); (*pos)++; }
    (*pos)++;
    N *n = mk(v);
    n->l = deser(b, pos);
    n->r = deser(b, pos);
    return n;
}

/* complete-tree insert at first vacancy, delete by moving deepest node */
static void insert_lvl(N **root, int v) {
    if (!*root) { *root = mk(v); return; }
    N *q[MAXN]; int h = 0, tl = 0;
    q[tl++] = *root;
    while (h < tl) {
        N *x = q[h++];
        if (!x->l) { x->l = mk(v); return; }
        if (!x->r) { x->r = mk(v); return; }
        q[tl++] = x->l; q[tl++] = x->r;
    }
}
static int delete_val(N **root, int v) {
    if (!*root) return 0;
    N *q[MAXN], *par[MAXN]; int h = 0, tl = 0;
    N *target = NULL, *last = NULL, *lastpar = NULL;
    q[tl] = *root; par[tl++] = NULL;
    while (h < tl) {
        N *x = q[h], *p = par[h]; h++;
        if (!target && x->v == v) target = x;
        last = x; lastpar = p;
        if (x->l) { q[tl] = x->l; par[tl++] = x; }
        if (x->r) { q[tl] = x->r; par[tl++] = x; }
    }
    if (!target) return 0;
    target->v = last->v;
    if (!lastpar) *root = NULL;
    else if (lastpar->r == last) lastpar->r = NULL;
    else lastpar->l = NULL;
    free(last);
    return 1;
}

static void show(const char *name, const int *a, int n) {
    printf("%s:", name);
    for (int i = 0; i < n; i++) printf(" %d", a[i]);
    printf("\n");
}
static unsigned hashv(const int *a, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned)a[i]; h *= 16777619u; }
    return h;
}

int main(void) {
    static int A[MAXN], B[MAXN], C[MAXN];
    /* fixed small tree:      1
     *                      /   \
     *                     2     3
     *                    / \     \
     *                   4   5     6      */
    N *t = mk(1);
    t->l = mk(2); t->r = mk(3);
    t->l->l = mk(4); t->l->r = mk(5); t->r->r = mk(6);
    int n = cnt(t);
    ro = A;
    rn = 0; pre_r(t); check(pre_i(t, B) == n && !memcmp(A, B, sizeof(int) * (size_t)n), "pre");
    show("pre", A, n);
    rn = 0; in_r(t); check(in_i(t, B) == n && !memcmp(A, B, sizeof(int) * (size_t)n), "in");
    show("in", A, n);
    rn = 0; post_r(t); check(post_i(t, B) == n && !memcmp(A, B, sizeof(int) * (size_t)n), "post");
    show("post", A, n);
    n = level_i(t, A); show("level", A, n);
    n = zigzag(t, A); show("zigzag", A, n);
    printf("height=%d count=%d\n", height(t), cnt(t));
    char buf[4096];
    int len = ser(t, buf, 0);
    buf[len] = 0;
    printf("serial=%s\n", buf);
    freet(t);

    /* random shapes */
    long totalh = 0;
    for (int round = 0; round < 40; round++) {
        int sz = (int)(rnd() % 200) + 1;
        N *r = build_random(sz);
        check(cnt(r) == sz, "count");
        int m;
        rn = 0; ro = A; pre_r(r); m = pre_i(r, B); check(m == sz && !memcmp(A, B, sizeof(int) * (size_t)m), "rpre");
        rn = 0; in_r(r); m = in_i(r, B); check(m == sz && !memcmp(A, B, sizeof(int) * (size_t)m), "rin");
        rn = 0; post_r(r); m = post_i(r, B); check(m == sz && !memcmp(A, B, sizeof(int) * (size_t)m), "rpost");
        rn = 0; in_r(r);
        memcpy(C, A, sizeof(int) * (size_t)sz);
        len = ser(r, buf, 0);
        buf[len] = 0;
        int pos = 0;
        N *d = deser(buf, &pos);
        check(pos == len && same(r, d), "roundtrip");
        mirror(d);
        rn = 0; ro = B; in_r(d);
        for (int i = 0; i < sz; i++) check(B[i] == C[sz - 1 - i], "mirror is reversed inorder");
        mirror_iter(d);
        check(same(r, d), "double mirror");
        totalh += height(r);
        if (round < 3) {
            rn = 0; ro = A; pre_r(r);
            printf("random size=%d height=%d prehash=%u serlen=%d\n", sz, height(r), hashv(A, sz), len);
        }
        freet(d);
        freet(r);
    }
    printf("total height over 40 random trees=%ld\n", totalh);

    /* level-order insert and delete-by-deepest against an array model */
    N *root = NULL;
    int model[MAXN], mn = 0, dels = 0;
    for (int op = 0; op < 400; op++) {
        if (mn < 5 || (rnd() % 3) != 0) {
            int v = (int)(rnd() % 100);
            insert_lvl(&root, v);
            model[mn++] = v;
        } else {
            int v = model[rnd() % (unsigned)mn];
            int idx = 0;
            while (model[idx] != v) idx++;
            check(delete_val(&root, v), "delete found");
            model[idx] = model[mn - 1]; mn--;
            dels++;
        }
        check(delete_val(&root, 1000) == 0, "delete absent");
        int m = level_i(root, A);
        check(m == mn && !memcmp(A, model, sizeof(int) * (size_t)mn), "level order equals model");
        int h = 0;
        for (int x = mn; x > 0; x >>= 1) h++;
        check(height(root) == h, "complete height");
    }
    printf("after ops: count=%d deletes=%d height=%d\n", mn, dels, height(root));
    int m = level_i(root, A);
    printf("level hash=%u\n", hashv(A, m));
    freet(root);
    return 0;
}
