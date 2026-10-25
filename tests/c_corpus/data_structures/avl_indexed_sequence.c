/*
 * title: AVL-backed indexable sequence with prefix sums
 * topic: data_structures
 * covers: size-augmented AVL tree, implicit keys by position, insert at index, erase at index, random access, sum augmentation, prefix-sum lower bound descent, array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}

typedef struct N {
    int v, h, size;
    long sum;
    struct N *l, *r;
} N;

static int ht(N *t) { return t ? t->h : 0; }
static int sz(N *t) { return t ? t->size : 0; }
static long sm(N *t) { return t ? t->sum : 0; }
static void upd(N *t) {
    int a = ht(t->l), b = ht(t->r);
    t->h = 1 + (a > b ? a : b);
    t->size = 1 + sz(t->l) + sz(t->r);
    t->sum = t->v + sm(t->l) + sm(t->r);
}
static N *rot_l(N *x) { N *y = x->r; x->r = y->l; y->l = x; upd(x); upd(y); return y; }
static N *rot_r(N *y) { N *x = y->l; y->l = x->r; x->r = y; upd(y); upd(x); return x; }
static N *bal(N *t) {
    upd(t);
    int bf = ht(t->l) - ht(t->r);
    if (bf > 1) {
        if (ht(t->l->l) < ht(t->l->r)) t->l = rot_l(t->l);
        return rot_r(t);
    }
    if (bf < -1) {
        if (ht(t->r->r) < ht(t->r->l)) t->r = rot_r(t->r);
        return rot_l(t);
    }
    return t;
}
static N *insert_at(N *t, int i, int v) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->v = v; n->l = n->r = NULL;
        upd(n);
        return n;
    }
    int ls = sz(t->l);
    if (i <= ls) t->l = insert_at(t->l, i, v);
    else t->r = insert_at(t->r, i - ls - 1, v);
    return bal(t);
}
static N *erase_at(N *t, int i, int *out) {
    int ls = sz(t->l);
    if (i < ls) t->l = erase_at(t->l, i, out);
    else if (i > ls) t->r = erase_at(t->r, i - ls - 1, out);
    else {
        *out = t->v;
        if (!t->l || !t->r) {
            N *c = t->l ? t->l : t->r;
            free(t);
            return c;
        }
        int dummy;
        N *m = t->r;
        while (m->l) m = m->l;
        t->v = m->v;
        t->r = erase_at(t->r, 0, &dummy);
    }
    return bal(t);
}
static int get(N *t, int i) {
    for (;;) {
        int ls = sz(t->l);
        if (i < ls) t = t->l;
        else if (i == ls) return t->v;
        else { i -= ls + 1; t = t->r; }
    }
}
static N *set(N *t, int i, int v) {
    int ls = sz(t->l);
    if (i < ls) t->l = set(t->l, i, v);
    else if (i == ls) t->v = v;
    else t->r = set(t->r, i - ls - 1, v);
    upd(t);
    return t;
}
/* sum of the first i elements */
static long prefix(N *t, int i) {
    long s = 0;
    while (t && i > 0) {
        int ls = sz(t->l);
        if (i <= ls) t = t->l;
        else { s += sm(t->l) + t->v; i -= ls + 1; t = t->r; }
    }
    return s;
}
/* smallest index whose inclusive prefix sum reaches target (values are non-negative), or -1 */
static int first_reaching(N *t, long target) {
    if (sm(t) < target) return -1;
    int base = 0;
    while (t) {
        if (sm(t->l) >= target) t = t->l;
        else {
            target -= sm(t->l);
            if (t->v >= target) return base + sz(t->l);
            target -= t->v;
            base += sz(t->l) + 1;
            t = t->r;
        }
    }
    return -1;
}
static int verify(N *t) {
    if (!t) return 0;
    int c = 1 + verify(t->l) + verify(t->r);
    int a = ht(t->l), b = ht(t->r);
    check(t->h == 1 + (a > b ? a : b), "height");
    check(a - b >= -1 && a - b <= 1, "balance");
    check(t->size == c, "size");
    check(t->sum == t->v + sm(t->l) + sm(t->r), "sum");
    return c;
}
static int cnt;
static int flat[MAXN];
static void inorder(N *t) { if (!t) return; inorder(t->l); flat[cnt++] = t->v; inorder(t->r); }
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    static int seq[MAXN];
    int n = 0;
    long ops = 0, queries = 0;
    int maxh = 0;
    for (int op = 0; op < 6000; op++) {
        unsigned c = rnd() % 10;
        if ((c < 5 && n < 1500) || n == 0) {
            int i = (int)(rnd() % (unsigned)(n + 1)), v = (int)(rnd() % 100);
            if (op % 7 == 0) i = 0;            /* push_front */
            else if (op % 7 == 1) i = n;       /* push_back */
            root = insert_at(root, i, v);
            memmove(seq + i + 1, seq + i, (size_t)(n - i) * sizeof(int));
            seq[i] = v; n++;
        } else if (c < 8) {
            int i = (int)(rnd() % (unsigned)n), out;
            root = erase_at(root, i, &out);
            check(out == seq[i], "erased value");
            memmove(seq + i, seq + i + 1, (size_t)(n - i - 1) * sizeof(int));
            n--;
        } else {
            int i = (int)(rnd() % (unsigned)n), v = (int)(rnd() % 100);
            root = set(root, i, v);
            seq[i] = v;
        }
        ops++;
        check(verify(root) == n, "size");
        if (n) {
            int i = (int)(rnd() % (unsigned)n);
            check(get(root, i) == seq[i], "get");
            int a = (int)(rnd() % (unsigned)(n + 1)), b = (int)(rnd() % (unsigned)(n + 1));
            if (a > b) { int t = a; a = b; b = t; }
            long want = 0;
            for (int j = a; j < b; j++) want += seq[j];
            check(prefix(root, b) - prefix(root, a) == want, "range sum");
            long total = prefix(root, n);
            long target = 1 + (long)(rnd() % (unsigned long)(total + 1));
            int idx = first_reaching(root, target);
            long acc = 0;
            int want_idx = -1;
            for (int j = 0; j < n; j++) { acc += seq[j]; if (acc >= target) { want_idx = j; break; } }
            check(idx == want_idx, "first index reaching prefix target");
            queries += 3;
        }
        if (ht(root) > maxh) maxh = ht(root);
        if (op % 50 == 0) {
            cnt = 0; inorder(root);
            check(cnt == n && !memcmp(flat, seq, sizeof(int) * (size_t)n), "contents");
        }
    }
    cnt = 0; inorder(root);
    check(cnt == n && !memcmp(flat, seq, sizeof(int) * (size_t)n), "final contents");
    printf("ops=%ld queries=%ld final length=%d height=%d max height=%d total=%ld\n", ops, queries, n, ht(root), maxh, prefix(root, n));
    printf("first five:");
    for (int i = 0; i < 5 && i < n; i++) printf(" %d", get(root, i));
    printf("\n");
    /* pure push_front / push_back give the same balanced height class */
    freet(root); root = NULL;
    for (int i = 0; i < 1000; i++) root = insert_at(root, 0, i);
    printf("1000 push_front: height=%d front=%d back=%d\n", ht(root), get(root, 0), get(root, 999));
    check(verify(root) == 1000, "push_front size");
    freet(root);
    return 0;
}
