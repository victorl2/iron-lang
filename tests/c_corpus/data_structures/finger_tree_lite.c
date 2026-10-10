/*
 * title: Persistent 2-3 finger tree with size measure
 * topic: data_structures
 * covers: finger tree, persistence, digits and spine, push/pop at both ends, concatenation, split at index, indexed lookup
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct E E;
struct E { int size, arity, val; E *c[3]; }; /* arity 0 = leaf element, 2 or 3 = node */
typedef struct { int n; E *e[4]; } Dg;
typedef struct FT FT;
struct FT { int kind, size; E *one; Dg pr, sf; FT *mid; }; /* kind: 0 empty, 1 single, 2 deep */

/* arena: everything is persistent, so nothing is freed until the end */
static void *pool[1 << 20];
static int npool;
static long allocs;
static void *ar(size_t n) { void *p = calloc(1, n); pool[npool++] = p; allocs++; return p; }

static unsigned long long rs = 0xF1A6E47EEULL * 31;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static FT EMPTY;
static E *leaf(int v) { E *e = ar(sizeof *e); e->size = 1; e->val = v; return e; }
static E *node2(E *a, E *b) { E *e = ar(sizeof *e); e->arity = 2; e->c[0] = a; e->c[1] = b; e->size = a->size + b->size; return e; }
static E *node3(E *a, E *b, E *c) { E *e = ar(sizeof *e); e->arity = 3; e->c[0] = a; e->c[1] = b; e->c[2] = c; e->size = a->size + b->size + c->size; return e; }
static int dsize(const Dg *d) { int s = 0; for (int i = 0; i < d->n; i++) s += d->e[i]->size; return s; }
static FT *one(E *e) { FT *t = ar(sizeof *t); t->kind = 1; t->one = e; t->size = e->size; return t; }
static FT *deep(Dg pr, FT *mid, Dg sf) {
    FT *t = ar(sizeof *t); t->kind = 2; t->pr = pr; t->mid = mid; t->sf = sf;
    t->size = dsize(&pr) + mid->size + dsize(&sf);
    return t;
}
static Dg dg_of_node(const E *n) { Dg d; d.n = n->arity; for (int i = 0; i < n->arity; i++) d.e[i] = n->c[i]; return d; }

static FT *push_front(FT *t, E *x) {
    if (t->kind == 0) return one(x);
    if (t->kind == 1) { Dg a = { 1, { x } }, b = { 1, { t->one } }; return deep(a, &EMPTY, b); }
    if (t->pr.n < 4) {
        Dg p; p.n = t->pr.n + 1; p.e[0] = x; for (int i = 0; i < t->pr.n; i++) p.e[i + 1] = t->pr.e[i];
        return deep(p, t->mid, t->sf);
    }
    Dg p = { 2, { x, t->pr.e[0] } };
    return deep(p, push_front(t->mid, node3(t->pr.e[1], t->pr.e[2], t->pr.e[3])), t->sf);
}
static FT *push_back(FT *t, E *x) {
    if (t->kind == 0) return one(x);
    if (t->kind == 1) { Dg a = { 1, { t->one } }, b = { 1, { x } }; return deep(a, &EMPTY, b); }
    if (t->sf.n < 4) {
        Dg s = t->sf; s.e[s.n++] = x;
        return deep(t->pr, t->mid, s);
    }
    Dg s = { 2, { t->sf.e[3], x } };
    return deep(t->pr, push_back(t->mid, node3(t->sf.e[0], t->sf.e[1], t->sf.e[2])), s);
}
static FT *from_digit(const Dg *d) { FT *t = &EMPTY; for (int i = 0; i < d->n; i++) t = push_back(t, d->e[i]); return t; }

static FT *view_front(FT *t, E **x);
static FT *view_back(FT *t, E **x);
static FT *deepL(Dg pr, FT *mid, Dg sf) {
    if (pr.n > 0) return deep(pr, mid, sf);
    if (mid->kind == 0) return from_digit(&sf);
    E *n; FT *m2 = view_front(mid, &n);
    return deep(dg_of_node(n), m2, sf);
}
static FT *deepR(Dg pr, FT *mid, Dg sf) {
    if (sf.n > 0) return deep(pr, mid, sf);
    if (mid->kind == 0) return from_digit(&pr);
    E *n; FT *m2 = view_back(mid, &n);
    return deep(pr, m2, dg_of_node(n));
}
static FT *view_front(FT *t, E **x) {
    check(t->kind != 0, "view_front on empty");
    if (t->kind == 1) { *x = t->one; return &EMPTY; }
    *x = t->pr.e[0];
    Dg p; p.n = t->pr.n - 1; for (int i = 0; i < p.n; i++) p.e[i] = t->pr.e[i + 1];
    return deepL(p, t->mid, t->sf);
}
static FT *view_back(FT *t, E **x) {
    check(t->kind != 0, "view_back on empty");
    if (t->kind == 1) { *x = t->one; return &EMPTY; }
    *x = t->sf.e[t->sf.n - 1];
    Dg s = t->sf; s.n--;
    return deepR(t->pr, t->mid, s);
}
/* concatenation */
static FT *app3(FT *a, E **ts, int n, FT *b) {
    if (a->kind == 0) { for (int i = n - 1; i >= 0; i--) b = push_front(b, ts[i]); return b; }
    if (b->kind == 0) { for (int i = 0; i < n; i++) a = push_back(a, ts[i]); return a; }
    if (a->kind == 1) return push_front(app3(&EMPTY, ts, n, b), a->one);
    if (b->kind == 1) return push_back(app3(a, ts, n, &EMPTY), b->one);
    E *list[12]; int m = 0;
    for (int i = 0; i < a->sf.n; i++) list[m++] = a->sf.e[i];
    for (int i = 0; i < n; i++) list[m++] = ts[i];
    for (int i = 0; i < b->pr.n; i++) list[m++] = b->pr.e[i];
    E *nodes[4]; int nn = 0, k = 0;
    while (m - k > 0) {
        int left = m - k;
        if (left == 2) { nodes[nn++] = node2(list[k], list[k + 1]); k += 2; }
        else if (left == 3) { nodes[nn++] = node3(list[k], list[k + 1], list[k + 2]); k += 3; }
        else if (left == 4) { nodes[nn++] = node2(list[k], list[k + 1]); nodes[nn++] = node2(list[k + 2], list[k + 3]); k += 4; }
        else { nodes[nn++] = node3(list[k], list[k + 1], list[k + 2]); k += 3; }
    }
    return deep(a->pr, app3(a->mid, nodes, nn, b->mid), b->sf);
}
static FT *concat(FT *a, FT *b) { return app3(a, NULL, 0, b); }

/* split: returns left part, element containing index i, right part */
static void split_digit(const Dg *d, int i, Dg *l, E **x, Dg *r) {
    l->n = r->n = 0;
    int k = 0;
    while (k < d->n && i >= d->e[k]->size) { i -= d->e[k]->size; l->e[l->n++] = d->e[k]; k++; }
    check(k < d->n, "index inside digit");
    *x = d->e[k];
    for (k++; k < d->n; k++) r->e[r->n++] = d->e[k];
}
static void split_tree(int i, FT *t, FT **L, E **x, FT **R) {
    if (t->kind == 1) { *L = &EMPTY; *x = t->one; *R = &EMPTY; return; }
    int spr = dsize(&t->pr), msz = t->mid->size;
    Dg l, r;
    if (i < spr) {
        split_digit(&t->pr, i, &l, x, &r);
        *L = from_digit(&l); *R = deepL(r, t->mid, t->sf);
    } else if (i < spr + msz) {
        FT *ml, *mr; E *xs;
        split_tree(i - spr, t->mid, &ml, &xs, &mr);
        Dg nd = dg_of_node(xs);
        split_digit(&nd, i - spr - ml->size, &l, x, &r);
        *L = deepR(t->pr, ml, l); *R = deepL(r, mr, t->sf);
    } else {
        split_digit(&t->sf, i - spr - msz, &l, x, &r);
        *L = deepR(t->pr, t->mid, l); *R = from_digit(&r);
    }
}
static void split_at(FT *t, int i, FT **L, FT **R) {
    if (i <= 0) { *L = &EMPTY; *R = t; return; }
    if (i >= t->size) { *L = t; *R = &EMPTY; return; }
    E *x; FT *l, *r;
    split_tree(i, t, &l, &x, &r);
    *L = l; *R = push_front(r, x);
}
static E *find(const FT *t, int i, int *off) {
    if (t->kind == 1) { *off = i; return t->one; }
    int spr = dsize(&t->pr);
    const Dg *d = NULL;
    if (i < spr) d = &t->pr;
    else if (i < spr + t->mid->size) {
        int o; E *n = find(t->mid, i - spr, &o);
        for (int k = 0; k < n->arity; k++) { if (o < n->c[k]->size) { *off = o; return n->c[k]; } o -= n->c[k]->size; }
        check(0, "descend node");
    } else { d = &t->sf; i -= spr + t->mid->size; }
    for (int k = 0; k < d->n; k++) { if (i < d->e[k]->size) { *off = i; return d->e[k]; } i -= d->e[k]->size; }
    check(0, "digit lookup");
    return NULL;
}
static int at(const FT *t, int i) {
    int off; E *e = find(t, i, &off);
    check(e->arity == 0, "lookup ends at a leaf");
    return e->val;
}
static void emit(const E *e, int *out, int *n) {
    if (!e->arity) { out[(*n)++] = e->val; return; }
    for (int i = 0; i < e->arity; i++) emit(e->c[i], out, n);
}
static int to_array(const FT *t, int *out) {
    int n = 0;
    if (t->kind == 0) return 0;
    if (t->kind == 1) { emit(t->one, out, &n); return n; }
    for (int i = 0; i < t->pr.n; i++) emit(t->pr.e[i], out, &n);
    /* the middle tree holds nodes: flatten recursively */
    n += to_array(t->mid, out + n);
    for (int i = 0; i < t->sf.n; i++) emit(t->sf.e[i], out, &n);
    return n;
}
static int spine_depth(const FT *t) { return t->kind == 2 ? 1 + spine_depth(t->mid) : (t->kind == 1 ? 1 : 0); }
static int verify_e(const E *e) {
    if (!e->arity) return e->size == 1;
    int s = 0;
    for (int i = 0; i < e->arity; i++) { if (!verify_e(e->c[i])) return 0; s += e->c[i]->size; }
    return (e->arity == 2 || e->arity == 3) && s == e->size;
}
static void verify(const FT *t) {
    if (t->kind == 0) { check(t->size == 0, "empty size"); return; }
    if (t->kind == 1) { check(verify_e(t->one) && t->size == t->one->size, "single"); return; }
    check(t->pr.n >= 1 && t->pr.n <= 4 && t->sf.n >= 1 && t->sf.n <= 4, "digit sizes 1..4");
    int s = 0;
    for (int i = 0; i < t->pr.n; i++) { check(verify_e(t->pr.e[i]), "prefix element"); s += t->pr.e[i]->size; }
    for (int i = 0; i < t->sf.n; i++) { check(verify_e(t->sf.e[i]), "suffix element"); s += t->sf.e[i]->size; }
    verify(t->mid);
    check(t->size == s + t->mid->size, "deep size");
}

#define MAXLEN 4000
typedef struct { FT *t; int a[MAXLEN]; int n; } Model;

int main(void) {
    static Model m; /* live tree plus its array model */
    m.t = &EMPTY;
    static struct { FT *t; int *a; int n; } snap[12];
    int nsnap = 0, next = 1;
    long pf = 0, pb = 0, popf = 0, popb = 0, cat = 0, spl = 0, idx = 0;
    static int tmp[MAXLEN + 1];
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 16;
        if (op < 4 && m.n < MAXLEN - 200) { m.t = push_front(m.t, leaf(next)); memmove(m.a + 1, m.a, sizeof(int) * (size_t)m.n); m.a[0] = next++; m.n++; pf++; }
        else if (op < 8 && m.n < MAXLEN - 200) { m.t = push_back(m.t, leaf(next)); m.a[m.n++] = next++; pb++; }
        else if (op < 10 && m.n > 0) { E *x; m.t = view_front(m.t, &x); check(x->val == m.a[0], "pop front value"); memmove(m.a, m.a + 1, sizeof(int) * (size_t)(m.n - 1)); m.n--; popf++; }
        else if (op < 12 && m.n > 0) { E *x; m.t = view_back(m.t, &x); check(x->val == m.a[m.n - 1], "pop back value"); m.n--; popb++; }
        else if (op < 13 && m.n < MAXLEN / 2) { /* concat with a fresh random tree */
            int k = (int)(rnd() % 120); FT *o = &EMPTY; int ob[120];
            for (int i = 0; i < k; i++) { o = push_back(o, leaf(next)); ob[i] = next++; }
            if (rnd() & 1) { m.t = concat(m.t, o); memcpy(m.a + m.n, ob, sizeof(int) * (size_t)k); }
            else { m.t = concat(o, m.t); memmove(m.a + k, m.a, sizeof(int) * (size_t)m.n); memcpy(m.a, ob, sizeof(int) * (size_t)k); }
            m.n += k; cat++;
        } else if (op < 14 && m.n > 2) { /* split, check both halves, keep one */
            int i = (int)(rnd() % (unsigned)(m.n + 1));
            FT *l, *r; split_at(m.t, i, &l, &r);
            verify(l); verify(r);
            check(l->size == i && r->size == m.n - i, "split sizes");
            int nl = to_array(l, tmp);
            check(nl == i && memcmp(tmp, m.a, sizeof(int) * (size_t)i) == 0, "left half content");
            int nr = to_array(r, tmp);
            check(nr == m.n - i && memcmp(tmp, m.a + i, sizeof(int) * (size_t)(m.n - i)) == 0, "right half content");
            if (rnd() & 1) { m.t = l; m.n = i; }
            else { m.t = r; memmove(m.a, m.a + i, sizeof(int) * (size_t)(m.n - i)); m.n -= i; }
            spl++;
        } else if (m.n > 0) {
            int i = (int)(rnd() % (unsigned)m.n);
            check(at(m.t, i) == m.a[i], "indexed lookup"); idx++;
        }
        if (step % 500 == 250 && nsnap < 12) { snap[nsnap].t = m.t; snap[nsnap].a = malloc(sizeof(int) * (size_t)(m.n + 1)); memcpy(snap[nsnap].a, m.a, sizeof(int) * (size_t)m.n); snap[nsnap].n = m.n; nsnap++; }
        if (step % 400 == 399) {
            verify(m.t);
            check(m.t->size == m.n, "tracked size");
            int n2 = to_array(m.t, tmp);
            check(n2 == m.n && memcmp(tmp, m.a, sizeof(int) * (size_t)m.n) == 0, "content matches model");
            printf("step %4d: length %4d spine depth %2d nodes allocated %ld\n", step + 1, m.n, spine_depth(m.t), allocs);
        }
    }
    /* persistence: every snapshot still reads as it did when taken */
    for (int s = 0; s < nsnap; s++) {
        verify(snap[s].t);
        int n2 = to_array(snap[s].t, tmp);
        check(n2 == snap[s].n && memcmp(tmp, snap[s].a, sizeof(int) * (size_t)n2) == 0, "old version unchanged");
        free(snap[s].a);
    }
    printf("ops: push_front %ld push_back %ld pop_front %ld pop_back %ld concat %ld split %ld index %ld; %d old versions intact\n", pf, pb, popf, popb, cat, spl, idx, nsnap);
    for (int i = 0; i < npool; i++) free(pool[i]);
    return 0;
}
