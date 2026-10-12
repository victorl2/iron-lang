/*
 * title: Interval tree on an AVL tree with max-endpoint augmentation
 * topic: data_structures
 * covers: interval tree, augmented AVL, subtree max endpoint, overlap search, all-overlaps with pruning, stabbing queries, delete by identity, brute-force cross-check
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

typedef struct { int lo, hi, id; } Iv;

typedef struct N {
    Iv iv;
    int h, max;
    struct N *l, *r;
} N;

static long visited;
static int ht(N *t) { return t ? t->h : 0; }
static int mx(N *t) { return t ? t->max : -1000000; }
static void upd(N *t) {
    int a = ht(t->l), b = ht(t->r);
    t->h = 1 + (a > b ? a : b);
    int m = t->iv.hi;
    if (mx(t->l) > m) m = mx(t->l);
    if (mx(t->r) > m) m = mx(t->r);
    t->max = m;
}
static int cmp_iv(const Iv *a, const Iv *b) {
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    if (a->hi != b->hi) return a->hi < b->hi ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}
static int cmp_qsort(const void *x, const void *y) { return cmp_iv((const Iv *)x, (const Iv *)y); }
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
static N *ins(N *t, Iv iv) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->iv = iv; n->l = n->r = NULL;
        upd(n);
        return n;
    }
    if (cmp_iv(&iv, &t->iv) < 0) t->l = ins(t->l, iv);
    else t->r = ins(t->r, iv);
    return bal(t);
}
static N *del(N *t, Iv iv, int *removed) {
    if (!t) return NULL;
    int c = cmp_iv(&iv, &t->iv);
    if (c < 0) t->l = del(t->l, iv, removed);
    else if (c > 0) t->r = del(t->r, iv, removed);
    else {
        *removed = 1;
        if (!t->l || !t->r) {
            N *ch = t->l ? t->l : t->r;
            free(t);
            return ch;
        }
        N *m = t->r;
        while (m->l) m = m->l;
        t->iv = m->iv;
        int d = 0;
        t->r = del(t->r, m->iv, &d);
    }
    return bal(t);
}
static int overlaps(const Iv *a, int lo, int hi) { return a->lo <= hi && lo <= a->hi; }
/* CLRS: any interval overlapping [lo,hi] */
static N *find_any(N *t, int lo, int hi) {
    while (t && !overlaps(&t->iv, lo, hi)) {
        visited++;
        if (t->l && t->l->max >= lo) t = t->l;
        else t = t->r;
    }
    return t;
}
static int all_overlaps(N *t, int lo, int hi, Iv *out, int n) {
    if (!t || t->max < lo) return n;
    visited++;
    n = all_overlaps(t->l, lo, hi, out, n);
    if (overlaps(&t->iv, lo, hi)) out[n++] = t->iv;
    if (t->iv.lo <= hi) n = all_overlaps(t->r, lo, hi, out, n);
    return n;
}
static int verify(N *t, const Iv *lo, const Iv *hi) {
    if (!t) return 0;
    if (lo) check(cmp_iv(lo, &t->iv) < 0, "order low");
    if (hi) check(cmp_iv(&t->iv, hi) < 0, "order high");
    int c = 1 + verify(t->l, lo, &t->iv) + verify(t->r, &t->iv, hi);
    int a = ht(t->l), b = ht(t->r);
    check(t->h == 1 + (a > b ? a : b) && a - b >= -1 && a - b <= 1, "avl");
    int m = t->iv.hi;
    if (mx(t->l) > m) m = mx(t->l);
    if (mx(t->r) > m) m = mx(t->r);
    check(t->max == m, "max field");
    return c;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    static Iv live[MAXN], got[MAXN], want[MAXN];
    int nl = 0, next_id = 1;
    N *root = NULL;
    long any_hits = 0, any_miss = 0, overlaps_total = 0, stab_total = 0, vis_any = 0;
    int ins_n = 0, del_n = 0;
    for (int op = 0; op < 3000; op++) {
        unsigned c = rnd() % 10;
        if (c < 5 || nl < 5) {
            Iv iv;
            iv.lo = (int)(rnd() % 1000);
            iv.hi = iv.lo + (int)(rnd() % 40);
            iv.id = next_id++;
            root = ins(root, iv);
            live[nl++] = iv;
            ins_n++;
        } else if (c < 8) {
            int i = (int)(rnd() % (unsigned)nl), removed = 0;
            root = del(root, live[i], &removed);
            check(removed, "delete by identity");
            live[i] = live[--nl];
            del_n++;
        }
        check(verify(root, NULL, NULL) == nl, "size");
        /* overlap queries */
        int lo = (int)(rnd() % 1050), hi = lo + (int)(rnd() % 30);
        visited = 0;
        N *a = find_any(root, lo, hi);
        vis_any += visited;
        int brute_any = 0;
        for (int i = 0; i < nl; i++) if (overlaps(&live[i], lo, hi)) brute_any = 1;
        check((a != NULL) == brute_any, "find_any agrees with brute force");
        if (a) { check(overlaps(&a->iv, lo, hi), "found interval overlaps"); any_hits++; } else any_miss++;
        int n = all_overlaps(root, lo, hi, got, 0), m = 0;
        for (int i = 0; i < nl; i++) if (overlaps(&live[i], lo, hi)) want[m++] = live[i];
        qsort(want, (size_t)m, sizeof(Iv), cmp_qsort);
        check(n == m && !memcmp(got, want, sizeof(Iv) * (size_t)n), "all overlaps");
        overlaps_total += n;
        /* stabbing query at a point */
        int pt = (int)(rnd() % 1050);
        n = all_overlaps(root, pt, pt, got, 0);
        m = 0;
        for (int i = 0; i < nl; i++) if (live[i].lo <= pt && pt <= live[i].hi) m++;
        check(n == m, "stabbing count");
        stab_total += n;
    }
    printf("inserts=%d deletes=%d live=%d height=%d\n", ins_n, del_n, nl, ht(root));
    printf("find_any hits=%ld misses=%ld avg visited x100=%ld\n", any_hits, any_miss, vis_any * 100 / (any_hits + any_miss));
    printf("total reported overlaps=%ld stabbing results=%ld\n", overlaps_total, stab_total);
    freet(root);
    return 0;
}
