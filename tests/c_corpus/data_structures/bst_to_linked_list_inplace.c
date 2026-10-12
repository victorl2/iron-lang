/*
 * title: In-place conversions between BSTs and linked lists
 * topic: data_structures
 * covers: BST to circular doubly linked list in place, list back to balanced BST, preorder flatten to right-only list, pointer reuse, forward and backward list walks, sorted-array model
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
/* reference model: sorted array of unique keys */
int mod[MAXN], mn;
int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
int mhas(int k) { int p = mfind(k); return p < mn && mod[p] == k; }
int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}
int no, outk[MAXN];

typedef struct N {
    int k;
    struct N *l, *r;      /* tree children, or prev/next when used as a list */
} N;

static N *insert(N *t, int k) {
    N **p = &t;
    while (*p) {
        if (k == (*p)->k) return t;
        p = k < (*p)->k ? &(*p)->l : &(*p)->r;
    }
    N *n = xmalloc(sizeof *n);
    n->k = k; n->l = n->r = NULL;
    *p = n;
    return t;
}
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int count(N *t) { return t ? 1 + count(t->l) + count(t->r) : 0; }
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static void preorder(N *t, int *o, int *n) { if (!t) return; o[(*n)++] = t->k; preorder(t->l, o, n); preorder(t->r, o, n); }

/* BST -> circular doubly linked list (l = prev, r = next), returns the smallest node */
static N *tail_g;
static N *to_dll_rec(N *t) {
    if (!t) return NULL;
    N *head = to_dll_rec(t->l);
    if (tail_g) { tail_g->r = t; t->l = tail_g; }
    else head = t;
    tail_g = t;
    N *h2 = to_dll_rec(t->r);
    (void)h2;
    return head ? head : t;
}
static N *bst_to_circular_dll(N *root) {
    if (!root) return NULL;
    tail_g = NULL;
    N *head = to_dll_rec(root);
    /* to_dll_rec returns leftmost only for the leftmost chain; recompute from list */
    while (head->l) head = head->l;
    head->l = tail_g;
    tail_g->r = head;
    return head;
}
/* sorted doubly linked list (non-circular view via count) -> height-balanced BST, consuming nodes in order */
static N *dll_to_bst(N **cursor, int n) {
    if (n <= 0) return NULL;
    N *left = dll_to_bst(cursor, n / 2);
    N *root = *cursor;
    *cursor = root->r;
    root->l = left;
    root->r = dll_to_bst(cursor, n - n / 2 - 1);
    return root;
}
/* flatten to a right-only list in preorder, in place, without recursion or a stack */
static void flatten_preorder(N *root) {
    N *cur = root;
    while (cur) {
        if (cur->l) {
            N *p = cur->l;
            while (p->r) p = p->r;
            p->r = cur->r;
            cur->r = cur->l;
            cur->l = NULL;
        }
        cur = cur->r;
    }
}
static void freelist(N *head, int n) {
    for (int i = 0; i < n; i++) { N *nx = head->r; free(head); head = nx; }
}

int main(void) {
    static int pre[MAXN];
    for (int round = 0; round < 40; round++) {
        int n = (int)(rnd() % 200) + 1;
        N *t = NULL;
        mn = 0;
        for (int i = 0; i < n; i++) {
            int k = (int)(rnd() % 1000);
            t = insert(t, k); minsert(k);
        }
        int hb = height(t), cnt = count(t);
        check(cnt == mn, "count");
        N *head = bst_to_circular_dll(t);
        /* forward walk */
        N *x = head;
        for (int i = 0; i < mn; i++) { check(x->k == mod[i], "forward"); x = x->r; }
        check(x == head, "circular forward");
        x = head->l;
        for (int i = mn - 1; i >= 0; i--) { check(x->k == mod[i], "backward"); x = x->l; }
        check(x == head->l->r->l, "circular backward");
        /* open the circle and rebuild */
        N *tail = head->l;
        tail->r = NULL;
        N *cursor = head;
        N *b = dll_to_bst(&cursor, mn);
        check(cursor == NULL, "consumed all");
        no = 0; inorder(b);
        check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "rebuilt inorder");
        int minh = 0;
        for (int v = mn; v > 0; v >>= 1) minh++;
        check(height(b) == minh, "rebuilt tree has minimum height");
        if (round < 4) printf("round %d: n=%d height before=%d after=%d\n", round, mn, hb, height(b));
        /* flatten preorder */
        int want[MAXN], wn = 0;
        preorder(b, want, &wn);
        flatten_preorder(b);
        int k = 0;
        for (N *y = b; y; y = y->r) { check(!y->l, "no left child after flatten"); pre[k++] = y->k; }
        check(k == mn && !memcmp(pre, want, sizeof(int) * (size_t)mn), "flatten equals preorder");
        freelist(b, mn);
    }
    printf("40 rounds of tree -> circular list -> balanced tree -> preorder list verified\n");
    /* tiny cases */
    N *one = insert(NULL, 7);
    N *h = bst_to_circular_dll(one);
    printf("single node list: self-linked=%d\n", h->l == h && h->r == h);
    free(one);
    return 0;
}
