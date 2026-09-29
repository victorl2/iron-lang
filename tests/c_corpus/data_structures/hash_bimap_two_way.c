/*
 * title: Bidirectional map with two hash indexes over shared entries
 * topic: data_structures
 * covers: bimap, one-to-one invariant, displacement on conflict, shared entry pool with free list, dual indexing
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
enum { CAP = 512, TAB = 1024, NIL = -1 };
typedef struct {
    uint32_t key, val;
    int next_k, next_v; /* chain links in the two indexes, also free-list link in next_k */
    int used;
} Entry;
typedef struct {
    Entry e[CAP];
    int by_key[TAB], by_val[TAB];
    int free_head, n;
    long displaced;
} BiMap;

static void bm_init(BiMap *m) {
    for (int i = 0; i < TAB; i++)
        m->by_key[i] = m->by_val[i] = NIL;
    for (int i = 0; i < CAP; i++) {
        m->e[i].used = 0;
        m->e[i].next_k = i + 1 < CAP ? i + 1 : NIL;
    }
    m->free_head = 0;
    m->n = 0;
    m->displaced = 0;
}

static int find_k(const BiMap *m, uint32_t k) {
    for (int i = m->by_key[mix32(k) & (TAB - 1)]; i != NIL; i = m->e[i].next_k)
        if (m->e[i].key == k)
            return i;
    return NIL;
}
static int find_v(const BiMap *m, uint32_t v) {
    for (int i = m->by_val[mix32(v ^ 0x5bd1e995u) & (TAB - 1)]; i != NIL; i = m->e[i].next_v)
        if (m->e[i].val == v)
            return i;
    return NIL;
}

static void unlink_entry(BiMap *m, int id) {
    int *pp = &m->by_key[mix32(m->e[id].key) & (TAB - 1)];
    while (*pp != id)
        pp = &m->e[*pp].next_k;
    *pp = m->e[id].next_k;
    pp = &m->by_val[mix32(m->e[id].val ^ 0x5bd1e995u) & (TAB - 1)];
    while (*pp != id)
        pp = &m->e[*pp].next_v;
    *pp = m->e[id].next_v;
    m->e[id].used = 0;
    m->e[id].next_k = m->free_head;
    m->free_head = id;
    m->n--;
}

/* insert (k,v); evicts any pair that shares k or v. Returns number of pairs displaced (0..2). */
static int bm_put(BiMap *m, uint32_t k, uint32_t v) {
    int displaced = 0;
    int a = find_k(m, k);
    if (a != NIL) {
        if (m->e[a].val == v)
            return 0; /* identical pair already present */
        unlink_entry(m, a);
        displaced++;
    }
    int b = find_v(m, v);
    if (b != NIL) {
        unlink_entry(m, b);
        displaced++;
    }
    check(m->free_head != NIL, "pool exhausted");
    int id = m->free_head;
    m->free_head = m->e[id].next_k;
    m->e[id].key = k;
    m->e[id].val = v;
    m->e[id].used = 1;
    size_t hk = mix32(k) & (TAB - 1), hv = mix32(v ^ 0x5bd1e995u) & (TAB - 1);
    m->e[id].next_k = m->by_key[hk];
    m->by_key[hk] = id;
    m->e[id].next_v = m->by_val[hv];
    m->by_val[hv] = id;
    m->n++;
    m->displaced += displaced;
    return displaced;
}

int main(void) {
    static BiMap m;
    bm_init(&m);
    static uint32_t rk[CAP], rv[CAP];
    int rn = 0;
    long puts = 0, noops = 0, dels = 0, disp_hist[3] = {0};
    for (int step = 0; step < 40000; step++) {
        uint32_t k = (uint32_t)(rnd() % 300), v = (uint32_t)(rnd() % 300);
        int op = (int)(rnd() % 10);
        if (op < 6 && rn < CAP - 2) {
            int d = bm_put(&m, k, v);
            /* reference: remove pairs sharing k or v, then append */
            int same = 0, rd = 0;
            for (int i = 0; i < rn; i++)
                if (rk[i] == k && rv[i] == v)
                    same = 1;
            if (!same) {
                for (int i = 0; i < rn;) {
                    if (rk[i] == k || rv[i] == v) {
                        rk[i] = rk[rn - 1];
                        rv[i] = rv[rn - 1];
                        rn--;
                        rd++;
                    } else
                        i++;
                }
                rk[rn] = k;
                rv[rn++] = v;
            }
            check(d == rd, "displacement count");
            disp_hist[d]++;
            puts++;
            noops += same;
        } else if (op < 8) { /* delete by key */
            int id = find_k(&m, k), ri = -1;
            for (int i = 0; i < rn; i++)
                if (rk[i] == k)
                    ri = i;
            check((id != NIL) == (ri >= 0), "del by key presence");
            if (id != NIL) {
                unlink_entry(&m, id);
                rk[ri] = rk[rn - 1];
                rv[ri] = rv[rn - 1];
                rn--;
                dels++;
            }
        } else { /* both directions must agree */
            int ik = find_k(&m, k), iv = find_v(&m, v), rik = -1, riv = -1;
            for (int i = 0; i < rn; i++) {
                if (rk[i] == k)
                    rik = i;
                if (rv[i] == v)
                    riv = i;
            }
            check((ik != NIL) == (rik >= 0) && (iv != NIL) == (riv >= 0), "lookup presence");
            if (ik != NIL)
                check(m.e[ik].val == rv[rik], "forward");
            if (iv != NIL)
                check(m.e[iv].key == rk[riv], "backward");
        }
        check(m.n == rn, "size");
    }
    /* invariant: forward then backward returns the same key for every entry */
    for (int i = 0; i < CAP; i++)
        if (m.e[i].used)
            check(find_v(&m, m.e[i].val) == i && find_k(&m, m.e[i].key) == i, "one-to-one");
    printf("puts=%ld identical no-ops=%ld deletes=%ld pairs=%d/%d\n", puts, noops, dels, m.n, CAP);
    printf("displacement histogram (0,1,2 pairs evicted per put): %ld %ld %ld\n", disp_hist[0], disp_hist[1], disp_hist[2]);
    printf("total displaced=%ld\n", m.displaced);
    return 0;
}
