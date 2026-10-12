/*
 * title: Persistent hash array mapped trie
 * topic: data_structures
 * covers: hamt, bitmap-compressed nodes, popcount indexing, path copying, hash collision nodes, canonical compaction on delete, persistence
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LEVELS 7 /* 7 levels x 5 bits cover 32 bits (last level uses 2); beyond that: collision node */

typedef struct Node Node;
typedef struct { Node *sub; int key, val; } Entry;
struct Node { uint32_t bitmap; int n, collision; Entry e[]; };

static Node **pool; static long npool, cap;
static Node *newnode(int n, uint32_t bitmap, int collision) {
    Node *x = calloc(1, sizeof(Node) + sizeof(Entry) * (size_t)(n ? n : 1));
    x->n = n; x->bitmap = bitmap; x->collision = collision;
    if (npool == cap) { cap = cap ? cap * 2 : 1024; pool = realloc(pool, sizeof(Node *) * (size_t)cap); }
    pool[npool++] = x;
    return x;
}
static uint64_t rs = 0xAA17ULL * 999331;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static uint32_t mixh(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
/* keys >= 1000000 hash by key mod 300, so distinct keys 300 apart collide completely */
static uint32_t hash_of(int key) { return key >= 1000000 ? mixh((uint32_t)(key % 300) + 77u) : mixh((uint32_t)key); }
static int popcnt(uint32_t x) { int c = 0; while (x) { x &= x - 1; c++; } return c; }
static uint32_t chunk(uint32_t h, int level) { return (h >> (5 * level)) & 31u; }
static int index_of(const Node *n, uint32_t bit) { return popcnt(n->bitmap & (bit - 1)); }

static int lookup(const Node *n, int key, int *val) {
    uint32_t h = hash_of(key);
    for (int level = 0;; level++) {
        if (n->collision) { for (int i = 0; i < n->n; i++) if (n->e[i].key == key) { *val = n->e[i].val; return 1; } return 0; }
        uint32_t bit = 1u << chunk(h, level);
        if (!(n->bitmap & bit)) return 0;
        const Entry *e = &n->e[index_of(n, bit)];
        if (e->sub) { n = e->sub; continue; }
        if (e->key == key) { *val = e->val; return 1; }
        return 0;
    }
}
static Node *with_entry(const Node *n, int idx, Entry e) { Node *c = newnode(n->n, n->bitmap, n->collision); memcpy(c->e, n->e, sizeof(Entry) * (size_t)n->n); c->e[idx] = e; return c; }
static Node *merge2(int k1, int v1, int k2, int v2, int level) {
    if (level >= LEVELS) {
        Node *c = newnode(2, 0, 1);
        c->e[0].key = k1; c->e[0].val = v1; c->e[1].key = k2; c->e[1].val = v2;
        return c;
    }
    uint32_t c1 = chunk(hash_of(k1), level), c2 = chunk(hash_of(k2), level);
    if (c1 == c2) {
        Node *n = newnode(1, 1u << c1, 0);
        n->e[0].sub = merge2(k1, v1, k2, v2, level + 1);
        return n;
    }
    Node *n = newnode(2, (1u << c1) | (1u << c2), 0);
    Entry a = { NULL, k1, v1 }, b = { NULL, k2, v2 };
    if (c1 < c2) { n->e[0] = a; n->e[1] = b; } else { n->e[0] = b; n->e[1] = a; }
    return n;
}
static Node *insert(const Node *n, int level, uint32_t h, int key, int val, int *added) {
    if (n->collision) {
        for (int i = 0; i < n->n; i++) if (n->e[i].key == key) { Entry e = { NULL, key, val }; return with_entry(n, i, e); }
        Node *c = newnode(n->n + 1, 0, 1);
        memcpy(c->e, n->e, sizeof(Entry) * (size_t)n->n);
        c->e[n->n].key = key; c->e[n->n].val = val; *added = 1;
        return c;
    }
    uint32_t bit = 1u << chunk(h, level);
    int idx = index_of(n, bit);
    if (!(n->bitmap & bit)) {
        Node *c = newnode(n->n + 1, n->bitmap | bit, 0);
        memcpy(c->e, n->e, sizeof(Entry) * (size_t)idx);
        c->e[idx].key = key; c->e[idx].val = val;
        memcpy(c->e + idx + 1, n->e + idx, sizeof(Entry) * (size_t)(n->n - idx));
        *added = 1;
        return c;
    }
    const Entry *old = &n->e[idx];
    if (old->sub) { Entry e = { insert(old->sub, level + 1, h, key, val, added), 0, 0 }; return with_entry(n, idx, e); }
    if (old->key == key) { Entry e = { NULL, key, val }; return with_entry(n, idx, e); }
    Entry e = { merge2(old->key, old->val, key, val, level + 1), 0, 0 };
    *added = 1;
    return with_entry(n, idx, e);
}
/* returns the new node (the same pointer when the key is absent) */
static Node *erase(const Node *n, int level, uint32_t h, int key, int *removed) {
    if (n->collision) {
        int at = -1; for (int i = 0; i < n->n; i++) if (n->e[i].key == key) at = i;
        if (at < 0) return (Node *)n;
        *removed = 1;
        Node *c = newnode(n->n - 1, 0, 1);
        for (int i = 0, j = 0; i < n->n; i++) if (i != at) c->e[j++] = n->e[i];
        return c;
    }
    uint32_t bit = 1u << chunk(h, level);
    if (!(n->bitmap & bit)) return (Node *)n;
    int idx = index_of(n, bit);
    const Entry *old = &n->e[idx];
    if (old->sub) {
        Node *s = erase(old->sub, level + 1, h, key, removed);
        if (s == old->sub) return (Node *)n;
        Entry ne = { NULL, 0, 0 };
        if (s->n == 1 && !s->e[0].sub) ne = s->e[0]; /* pull a lone key-value pair up */
        else ne.sub = s;
        return with_entry(n, idx, ne);
    }
    if (old->key != key) return (Node *)n;
    *removed = 1;
    Node *c = newnode(n->n - 1, n->bitmap & ~bit, 0);
    memcpy(c->e, n->e, sizeof(Entry) * (size_t)idx);
    memcpy(c->e + idx, n->e + idx + 1, sizeof(Entry) * (size_t)(n->n - idx - 1));
    return c;
}

/* invariants: n == popcount(bitmap), no subtree consisting of a single key-value pair, collision nodes have >= 2 entries */
static int count_keys(const Node *n) { int c = 0; for (int i = 0; i < n->n; i++) c += n->e[i].sub ? count_keys(n->e[i].sub) : 1; return c; }
static int verify(const Node *n, int level, int *maxdepth, int *coll) {
    if (level > *maxdepth) *maxdepth = level;
    if (n->collision) { check(n->n >= 2, "collision node holds at least two keys"); (*coll)++; return n->n; }
    check(n->n == popcnt(n->bitmap), "entry count equals popcount");
    int total = 0;
    for (int i = 0; i < n->n; i++) {
        if (n->e[i].sub) {
            const Node *s = n->e[i].sub;
            check(!(s->n == 1 && !s->e[0].sub && !s->collision), "no sub-node with a single key-value pair");
            total += verify(s, level + 1, maxdepth, coll);
        } else total++;
    }
    return total;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

typedef struct { Node *root; int size; } Map;
static Map m_put(Map m, int k, int v) { int added = 0; Node *r = insert(m.root, 0, hash_of(k), k, v, &added); Map o = { r, m.size + added }; return o; }
static Map m_del(Map m, int k) { int rem = 0; Node *r = erase(m.root, 0, hash_of(k), k, &rem); Map o = { r, m.size - rem }; return o; }

#define DOM 2400 /* key domain: 0..1999 plain, 2000.. maps to 1000000 + 300*j + r for collisions */
static int key_of(int idx) { return idx < 2000 ? idx : 1000000 + (idx - 2000) / 4 + 300 * ((idx - 2000) % 4); }

int main(void) {
    static int refv[DOM]; static char refp[DOM];
    Map cur = { newnode(0, 0, 0), 0 };
    static Map snaps[6]; static int snapv[6][DOM]; static char snapp[6][DOM]; int ns = 0;
    long puts_ = 0, dels = 0, gets = 0, upd = 0;
    for (int step = 0; step < 30000; step++) {
        int idx = (int)(rnd() % DOM);
        if (step % 5000 < 300) idx = (int)(rnd() % 40); /* hot keys */
        int k = key_of(idx);
        unsigned op = rnd() % 10;
        if (op < 5) { int v = (int)(rnd() % 100000); if (refp[idx]) upd++; cur = m_put(cur, k, v); refv[idx] = v; refp[idx] = 1; puts_++; }
        else if (op < 8) { cur = m_del(cur, k); refp[idx] = 0; dels++; }
        else { int v = -1; int f = lookup(cur.root, k, &v); check(f == refp[idx] && (!f || v == refv[idx]), "lookup"); gets++; }
        if (step % 5000 == 2500 && ns < 6) { snaps[ns] = cur; memcpy(snapv[ns], refv, sizeof refv); memcpy(snapp[ns], refp, sizeof refp); ns++; }
    }
    int live = 0; for (int i = 0; i < DOM; i++) live += refp[i];
    check(cur.size == live && count_keys(cur.root) == live, "size");
    for (int i = 0; i < DOM; i++) { int v = -1; int f = lookup(cur.root, key_of(i), &v); check(f == refp[i] && (!f || v == refv[i]), "final content"); }
    int md = 0, coll = 0;
    check(verify(cur.root, 0, &md, &coll) == live, "verify count");
    for (int s = 0; s < ns; s++) {
        int c2 = 0, d2 = 0;
        verify(snaps[s].root, 0, &d2, &c2);
        for (int i = 0; i < DOM; i++) { int v = -1; int f = lookup(snaps[s].root, key_of(i), &v); check(f == snapp[s][i] && (!f || v == snapv[s][i]), "old version unchanged"); }
    }
    printf("puts %ld (updates %ld) deletes %ld gets %ld, live keys %d, max depth %d, collision nodes %d\n", puts_, upd, dels, gets, live, md, coll);
    printf("%d persistent snapshots verified, nodes allocated %ld\n", ns, npool);
    /* delete everything: the trie collapses back to an empty root */
    for (int i = 0; i < DOM; i++) if (refp[i]) cur = m_del(cur, key_of(i));
    check(cur.size == 0 && cur.root->n == 0, "empty after deleting all keys");
    /* insertion order does not change the canonical shape: same keys, two orders, same node structure counts */
    Map a = { newnode(0, 0, 0), 0 }, b = { newnode(0, 0, 0), 0 };
    int ks[500];
    for (int i = 0; i < 500; i++) ks[i] = key_of((int)(rnd() % DOM));
    for (int i = 0; i < 500; i++) a = m_put(a, ks[i], i);
    for (int i = 499; i >= 0; i--) b = m_put(b, ks[i], 0);
    int da = 0, db = 0, ca = 0, cb = 0;
    verify(a.root, 0, &da, &ca); verify(b.root, 0, &db, &cb);
    check(a.size == b.size && da == db && ca == cb, "shape is independent of insertion order");
    int sorted[500]; memcpy(sorted, ks, sizeof ks); qsort(sorted, 500, sizeof(int), cmp_int);
    int distinct = 0; for (int i = 0; i < 500; i++) if (i == 0 || sorted[i] != sorted[i - 1]) distinct++;
    check(distinct == a.size, "distinct key count");
    printf("500 keys in two insertion orders: %d distinct keys, depth %d and %d, collision nodes %d and %d\n", a.size, da, db, ca, cb);
    for (long i = 0; i < npool; i++) free(pool[i]);
    free(pool);
    return 0;
}
