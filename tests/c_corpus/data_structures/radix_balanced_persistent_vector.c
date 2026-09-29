/*
 * title: Persistent radix-balanced vector with tail
 * topic: data_structures
 * covers: radix balanced tree, persistent vector, path copying, tail buffer, structural sharing, push/pop/set/get by bit slicing
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BITS 2
#define BF 4
#define MASK 3

typedef struct N { struct N *kid[BF]; int val[BF]; } N; /* internal nodes use kid[], leaves use val[] */
typedef struct { int size, shift; N *root, *tail; } Vec; /* tail is a leaf-sized buffer holding size - tailoff values */

static N *pool[2000000]; static long npool, live_alloc;
static N *mk(void) { N *n = calloc(1, sizeof *n); pool[npool++] = n; live_alloc++; return n; }
static N *copy(const N *n) { N *c = mk(); memcpy(c, n, sizeof *c); return c; }

static unsigned long long rs = 0x2AD1C0DEULL * 20011;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int tail_off(int size) { return size < BF ? 0 : ((size - 1) >> BITS) << BITS; }
static Vec empty(void) { Vec v; v.size = 0; v.shift = BITS; v.root = mk(); v.tail = mk(); return v; }

static const N *leaf_for(const Vec *v, int i) {
    if (i >= tail_off(v->size)) return v->tail;
    const N *n = v->root;
    for (int level = v->shift; level > 0; level -= BITS) n = n->kid[(i >> level) & MASK];
    return n;
}
static int get(const Vec *v, int i) { check(i >= 0 && i < v->size, "index"); return leaf_for(v, i)->val[i & MASK]; }
static N *new_path(int level, N *leaf) {
    if (level == 0) return leaf;
    N *n = mk(); n->kid[0] = new_path(level - BITS, leaf);
    return n;
}
static N *push_tail(int size, int level, const N *parent, N *leaf) {
    int sub = ((size - 1) >> level) & MASK;
    N *n = copy(parent);
    if (level == BITS) n->kid[sub] = leaf;
    else n->kid[sub] = parent->kid[sub] ? push_tail(size, level - BITS, parent->kid[sub], leaf) : new_path(level - BITS, leaf);
    return n;
}
static Vec push(Vec v, int x) {
    Vec r = v;
    if (v.size - tail_off(v.size) < BF) {
        r.tail = copy(v.tail); r.tail->val[v.size & MASK] = x; r.size++;
        return r;
    }
    N *newroot;
    int shift = v.shift;
    if ((v.size >> BITS) > (1 << shift)) { /* root overflow: add a level */
        newroot = mk(); newroot->kid[0] = v.root; newroot->kid[1] = new_path(shift, v.tail); shift += BITS;
    } else newroot = push_tail(v.size, shift, v.root, v.tail);
    r.root = newroot; r.shift = shift;
    r.tail = mk(); r.tail->val[0] = x; r.size++;
    return r;
}
static N *assoc_node(int level, const N *n, int i, int x) {
    N *c = copy(n);
    if (level == 0) c->val[i & MASK] = x;
    else c->kid[(i >> level) & MASK] = assoc_node(level - BITS, n->kid[(i >> level) & MASK], i, x);
    return c;
}
static Vec set(Vec v, int i, int x) {
    check(i >= 0 && i < v.size, "set index");
    Vec r = v;
    if (i >= tail_off(v.size)) { r.tail = copy(v.tail); r.tail->val[i & MASK] = x; }
    else r.root = assoc_node(v.shift, v.root, i, x);
    return r;
}
static N *pop_tail(int size, int level, const N *node) {
    int sub = ((size - 2) >> level) & MASK;
    if (level > BITS) {
        N *nc = pop_tail(size, level - BITS, node->kid[sub]);
        if (!nc && sub == 0) return NULL;
        N *c = copy(node); c->kid[sub] = nc; return c;
    }
    if (sub == 0) return NULL;
    N *c = copy(node); c->kid[sub] = NULL; return c;
}
static Vec pop(Vec v) {
    check(v.size > 0, "pop empty");
    if (v.size == 1) return empty();
    Vec r = v;
    if (v.size - tail_off(v.size) > 1) { r.tail = copy(v.tail); r.size--; return r; }
    const N *newtail = leaf_for(&v, v.size - 2);
    N *nr = pop_tail(v.size, v.shift, v.root);
    int shift = v.shift;
    if (!nr) nr = mk();
    if (shift > BITS && !nr->kid[1]) { nr = nr->kid[0]; shift -= BITS; }
    r.root = nr; r.shift = shift; r.tail = (N *)newtail; r.size--;
    return r;
}
static int depth(const Vec *v) { return v->shift / BITS; }

/* count distinct nodes reachable from a version, and shared nodes between two versions */
static N *seen[200000]; static int nseen;
static void mark(const N *n, int level) {
    for (int i = 0; i < nseen; i++) if (seen[i] == n) return;
    seen[nseen++] = (N *)n;
    if (level > 0) for (int i = 0; i < BF; i++) if (n->kid[i]) mark(n->kid[i], level - BITS);
}
static int reachable(const Vec *v) { nseen = 0; mark(v->root, v->shift); mark(v->tail, 0); return nseen; }

int main(void) {
    printf("depth by size:");
    { Vec v = empty(); int marks[] = { 4, 5, 20, 21, 68, 69, 260, 261, 1028, 1029, 4100, 4101 }; int mi = 0;
      for (int i = 1; i <= 4101; i++) { v = push(v, i); if (i == marks[mi]) { printf(" %d:%d", i, depth(&v)); mi++; } }
      printf("\n"); }
    static int model[6000]; int nm = 0;
    Vec cur = empty();
    static Vec snaps[10]; static int snapn[10]; static int snapdata[10][6000]; int ns = 0;
    long pushes = 0, pops = 0, sets = 0, gets = 0; long alloc_push = 0, alloc_set = 0;
    for (int step = 0; step < 20000; step++) {
        unsigned op = rnd() % 10;
        if (op < 5 && nm < 5000) { long a = live_alloc; int v = (int)(rnd() % 100000); cur = push(cur, v); model[nm++] = v; pushes++; alloc_push += live_alloc - a; }
        else if (op < 7 && nm > 0) { cur = pop(cur); nm--; pops++; }
        else if (op < 8 && nm > 0) { long a = live_alloc; int i = (int)(rnd() % (unsigned)nm), v = (int)(rnd() % 100000); cur = set(cur, i, v); model[i] = v; sets++; alloc_set += live_alloc - a; }
        else if (nm > 0) { int i = (int)(rnd() % (unsigned)nm); check(get(&cur, i) == model[i], "get"); gets++; }
        check(cur.size == nm, "size");
        if (step % 2000 == 1000 && ns < 10) { snaps[ns] = cur; snapn[ns] = nm; memcpy(snapdata[ns], model, sizeof(int) * (size_t)nm); ns++; }
        if (step % 1000 == 999) for (int i = 0; i < nm; i++) check(get(&cur, i) == model[i], "full comparison");
    }
    for (int s = 0; s < ns; s++) {
        check(snaps[s].size == snapn[s], "snapshot size");
        for (int i = 0; i < snapn[s]; i++) check(get(&snaps[s], i) == snapdata[s][i], "snapshot content unchanged");
    }
    printf("pushes %ld pops %ld sets %ld gets %ld, final size %d depth %d\n", pushes, pops, sets, gets, nm, depth(&cur));
    printf("avg nodes allocated per push %ld.%02ld, per set %ld.%02ld\n", alloc_push / pushes, alloc_push * 100 / pushes % 100, alloc_set / sets, alloc_set * 100 / sets % 100);
    /* structural sharing between a version and an updated copy */
    Vec base = empty();
    for (int i = 0; i < 3000; i++) base = push(base, i);
    Vec upd = set(base, 1234, -1);
    int nb = reachable(&base);
    nseen = 0; mark(base.root, base.shift); mark(base.tail, 0);
    int total_both;
    { int first = nseen; mark(upd.root, upd.shift); mark(upd.tail, 0); total_both = nseen; (void)first; }
    printf("3000-element vector: %d nodes reachable; one set adds %d new nodes (path length %d)\n", nb, total_both - nb, depth(&base) + 1);
    check(total_both - nb == depth(&base) + 1 || total_both - nb == depth(&base), "set copies exactly one root-to-leaf path");
    printf("snapshots verified: %d, total nodes ever allocated %ld\n", ns, npool);
    for (long i = 0; i < npool; i++) free(pool[i]);
    return 0;
}
