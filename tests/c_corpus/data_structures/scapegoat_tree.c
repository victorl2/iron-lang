/*
 * title: Scapegoat tree with weight-based rebuilding
 * topic: data_structures
 * covers: scapegoat tree, alpha-weight-balance, depth-triggered rebuild, flatten and rebuild subtree, lazy global rebuild on delete, sorted-array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
static inline unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static inline void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
static inline void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}
/* reference model: sorted array of unique keys */
static int mod[MAXN], mn;
static inline int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
static inline int mhas(int k) { int p = mfind(k); return p < mn && mod[p] == k; }
static inline int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
static inline int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}
static int no, outk[MAXN];

typedef struct N {
    int k, size;
    struct N *l, *r;
} N;

static int total, maxsz;
static int pending;
static long rebuilds, rebuilt_nodes;
static N *buf[MAXN];
static int bn;

static int sz(N *t) { return t ? t->size : 0; }
static void upd(N *t) { t->size = 1 + sz(t->l) + sz(t->r); }
/* alpha = 3/4: number of shrink steps by factor 3/4 needed to reach 1 */
static int alpha_h(int n) {
    int h = 0;
    long x = n;
    while (x > 1) { x = x * 3 / 4; h++; }
    return h;
}
static void flatten(N *t) {
    if (!t) return;
    flatten(t->l);
    buf[bn++] = t;
    flatten(t->r);
}
static N *build(int lo, int hi) {
    if (lo >= hi) return NULL;
    int mid = (lo + hi) / 2;
    N *t = buf[mid];
    t->l = build(lo, mid);
    t->r = build(mid + 1, hi);
    upd(t);
    return t;
}
static N *rebuild(N *t) {
    bn = 0;
    flatten(t);
    rebuilds++;
    rebuilt_nodes += bn;
    return build(0, bn);
}
static N *ins(N *t, int k, int depth, int *added) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->size = 1; n->l = n->r = NULL;
        *added = 1;
        total++;
        if (total > maxsz) maxsz = total;
        if (depth > alpha_h(total)) pending = 1;
        return n;
    }
    if (k < t->k) t->l = ins(t->l, k, depth + 1, added);
    else if (k > t->k) t->r = ins(t->r, k, depth + 1, added);
    else return t;
    upd(t);
    if (pending) {
        N *c = k < t->k ? t->l : t->r;
        if (c->size * 4 > t->size * 3) {
            pending = 0;
            return rebuild(t);
        }
    }
    return t;
}
static N *del(N *t, int k, int *removed) {
    if (!t) return NULL;
    if (k < t->k) t->l = del(t->l, k, removed);
    else if (k > t->k) t->r = del(t->r, k, removed);
    else {
        *removed = 1;
        if (!t->l || !t->r) {
            N *c = t->l ? t->l : t->r;
            free(t);
            return c;
        }
        N *m = t->r;
        while (m->l) m = m->l;
        t->k = m->k;
        int dummy = 0;
        t->r = del(t->r, m->k, &dummy);
    }
    upd(t);
    return t;
}
static int insert(N **root, int k) {
    int added = 0;
    pending = 0;
    *root = ins(*root, k, 0, &added);
    pending = 0;
    return added;
}
static int delete(N **root, int k) {
    int removed = 0;
    *root = del(*root, k, &removed);
    if (removed) {
        total--;
        if (total * 4 <= maxsz * 3) {
            *root = rebuild(*root);
            maxsz = total;
        }
    }
    return removed;
}
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    int c = 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
    check(c == t->size, "size field");
    return c;
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    int ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 500; i++) {
        insert(&root, i); minsert(i);
        check(verify(root, -1, 100000) == mn && total == mn, "asc size");
        check(height(root) <= alpha_h(maxsz) + 2, "asc height bound");
    }
    printf("ascending 500: height=%d (log2 bound %d) rebuilds=%ld nodes rebuilt=%ld\n",
           height(root), alpha_h(500), rebuilds, rebuilt_nodes);
    for (int i = 0; i < 400; i++) {
        check(delete(&root, i), "del asc"); mdelete(i);
        check(verify(root, -1, 100000) == mn && total == mn, "del size");
    }
    printf("after deleting 400: size=%d height=%d maxsz=%d rebuilds=%ld\n", mn, height(root), maxsz, rebuilds);
    for (int op = 0; op < 5000; op++) {
        int k = (int)(rnd() % 700);
        if (rnd() % 2) {
            int a = insert(&root, k);
            check(a == minsert(k), "insert result"); ins_n += a;
        } else {
            int a = delete(&root, k);
            check(a == mdelete(k), "delete result"); del_n += a;
        }
        check(verify(root, -1, 100000) == mn && total == mn, "size");
        check(has(root, k) == mhas(k), "search");
        int h = height(root);
        check(h <= alpha_h(maxsz) + 2, "height bound");
        if (h > maxh) maxh = h;
        if (op % 10 == 0) {
            no = 0; inorder(root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
        }
    }
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d\n", ins_n, del_n, mn, maxh);
    printf("rebuilds=%ld nodes rebuilt=%ld\n", rebuilds, rebuilt_nodes);
    freet(root);
    return 0;
}
