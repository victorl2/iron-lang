/*
 * title: N-ary tree as left-child right-sibling
 * topic: data_structures
 * covers: left-child right-sibling, n-ary tree, subtree removal, level order, parent-array cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 200

typedef struct LN {
    int id;
    struct LN *child, *sib;
} LN;

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static LN *nodes[MAXN];  /* id -> node or NULL */
static int parent[MAXN]; /* reference model: parent id, -1 root, -2 absent */
static int order_key[MAXN]; /* insertion sequence number, siblings ordered by it in model */

static LN *find(LN *root, int id) {
    if (!root) return NULL;
    if (root->id == id) return root;
    LN *r = find(root->child, id);
    return r ? r : find(root->sib, id);
}
/* append as last child */
static void add_child(LN *p, LN *c) {
    if (!p->child) { p->child = c; return; }
    LN *s = p->child;
    while (s->sib) s = s->sib;
    s->sib = c;
}
static void free_all(LN *n) {
    while (n) { LN *next = n->sib; free_all(n->child); free(n); n = next; }
}
/* detach node id from tree; returns detached (with own children) */
static LN *detach(LN *p, int id) {
    LN **link = &p->child;
    while (*link && (*link)->id != id) link = &(*link)->sib;
    LN *x = *link;
    if (x) { *link = x->sib; x->sib = NULL; }
    return x;
}
static void clear_ids(const LN *n) {
    for (; n; n = n->sib) { nodes[n->id] = NULL; parent[n->id] = -2; clear_ids(n->child); }
}
static void preorder(const LN *n, int *out, int *k) {
    for (; n; n = n->sib) { out[(*k)++] = n->id; preorder(n->child, out, k); }
}
static void postorder(const LN *n, int *out, int *k) {
    for (; n; n = n->sib) { postorder(n->child, out, k); out[(*k)++] = n->id; }
}
static int height(const LN *n) { /* number of nodes on longest path from n, counting siblings' subtrees */
    int best = 0;
    for (const LN *c = n->child; c; c = c->sib) { int h = height(c); if (h > best) best = h; }
    return best + 1;
}
static int subsize(const LN *n) { int c = 1; for (const LN *k = n->child; k; k = k->sib) c += subsize(k); return c; }
static int degree(const LN *n) { int d = 0; for (const LN *c = n->child; c; c = c->sib) d++; return d; }

/* reference: preorder by scanning the parent array (children in insertion order) */
static void ref_pre(int id, int *out, int *k) {
    out[(*k)++] = id;
    /* children in ascending order_key */
    int kids[MAXN], nk = 0;
    for (int i = 0; i < MAXN; i++) if (parent[i] == id) kids[nk++] = i;
    for (int i = 1; i < nk; i++) { int x = kids[i], j = i - 1; while (j >= 0 && order_key[kids[j]] > order_key[x]) { kids[j + 1] = kids[j]; j--; } kids[j + 1] = x; }
    for (int i = 0; i < nk; i++) ref_pre(kids[i], out, k);
}
static int ref_height(int id) {
    int best = 0;
    for (int i = 0; i < MAXN; i++) if (parent[i] == id) { int h = ref_height(i); if (h > best) best = h; }
    return best + 1;
}

/* binary-tree view: left = first child, right = next sibling; verify count/leaves */
static int bin_leaves(const LN *n) { /* nodes with no children in the n-ary sense = null left */
    int c = 0;
    for (; n; n = n->sib) c += n->child ? bin_leaves(n->child) : 1;
    return c;
}

static void level_order(const LN *root, int *out, int *k) {
    const LN *q[MAXN]; int h = 0, t = 0;
    q[t++] = root;
    while (h < t) {
        const LN *n = q[h++];
        out[(*k)++] = n->id;
        for (const LN *c = n->child; c; c = c->sib) q[t++] = c;
    }
}

int main(void) {
    LN *root = calloc(1, sizeof *root);
    root->id = 0;
    for (int i = 0; i < MAXN; i++) { nodes[i] = NULL; parent[i] = -2; order_key[i] = 0; }
    nodes[0] = root; parent[0] = -1;
    int seq = 1, ops_add = 0, ops_del = 0;
    for (int step = 0; step < 600; step++) {
        int alive[MAXN], na = 0;
        for (int i = 0; i < MAXN; i++) if (nodes[i]) alive[na++] = i;
        int next_id = 0;
        while (next_id < MAXN && nodes[next_id]) next_id++;
        if ((rnd() % 3 != 0 || na < 30) && next_id < MAXN) {
            int p = alive[rnd() % (unsigned)na];
            LN *c = calloc(1, sizeof *c);
            c->id = next_id;
            add_child(nodes[p], c);
            nodes[next_id] = c; parent[next_id] = p; order_key[next_id] = seq++;
            ops_add++;
        } else if (na > 1) {
            int v = alive[1 + rnd() % (unsigned)(na - 1)];
            for (int tries = 0; tries < 20 && subsize(nodes[v]) > 6; tries++) v = alive[1 + rnd() % (unsigned)(na - 1)];
            LN *sub = detach(nodes[parent[v]], v);
            check(sub != NULL, "detach finds node");
            clear_ids(sub);
            free_all(sub);
            ops_del++;
        }
        if (step % 100 == 99) {
            int a[MAXN], b[MAXN], ka = 0, kb = 0;
            preorder(root, a, &ka);
            ref_pre(0, b, &kb);
            check(ka == kb && memcmp(a, b, sizeof(int) * (size_t)ka) == 0, "preorder matches parent-array model");
            check(height(root) == ref_height(0), "height matches");
            int lv[MAXN], po[MAXN], kl = 0, kp = 0;
            level_order(root, lv, &kl);
            postorder(root, po, &kp);
            check(kl == ka && kp == ka, "traversals visit all nodes");
            int sum = 0, sp = 0, sl = 0;
            for (int i = 0; i < ka; i++) { sum += a[i]; sp += po[i]; sl += lv[i]; }
            check(sum == sp && sum == sl, "same node sets");
            /* leaf count via binary view equals nodes with no children */
            int leaves = 0;
            for (int i = 0; i < MAXN; i++) if (nodes[i] && !nodes[i]->child) leaves++;
            check(bin_leaves(root) == leaves, "leaf count in binary view");
            int maxdeg = 0;
            for (int i = 0; i < MAXN; i++) if (nodes[i] && degree(nodes[i]) > maxdeg) maxdeg = degree(nodes[i]);
            printf("step %3d: nodes %3d height %2d leaves %3d max-degree %2d root-degree %d\n", step + 1, ka, height(root), leaves, maxdeg, degree(root));
            if (step == 99) {
                printf("  pre:");
                for (int i = 0; i < ka && i < 20; i++) printf(" %d", a[i]);
                printf("\n  lvl:");
                for (int i = 0; i < kl && i < 20; i++) printf(" %d", lv[i]);
                printf("\n  post:");
                for (int i = 0; i < kp && i < 20; i++) printf(" %d", po[i]);
                printf("\n");
            }
        }
    }
    check(find(root, 0) == root, "find root");
    printf("adds %d deletes %d\n", ops_add, ops_del);
    free_all(root);
    return 0;
}
