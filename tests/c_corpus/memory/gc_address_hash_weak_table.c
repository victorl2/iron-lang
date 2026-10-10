/*
 * title: Address-keyed weak table in a moving collector
 * topic: memory
 * covers: identity hash on object addresses, keys that move at every copy, weak entries dropped when the key is not forwarded, stale index detection and lazy rehash, open addressing with linear probing, probe counting, table contents checked against the shadow model after each collection
 * deps: libc
 */
#define SEED 0x5EED0022ULL
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*@RNG*/
/* Deterministic splitmix64 generator so every platform sees the same run. */
static uint64_t rng_s = SEED;
static inline uint32_t rnd(void) {
    uint64_t z = (rng_s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (uint32_t)(z >> 32);
}
static inline uint32_t rnd_n(uint32_t n) {
    uint32_t r = rnd();
    return r % n;
}

/*@ENDRNG*/
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__);    \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

/*
 * Shadow model: an object graph keyed by immutable object ids. The mutator
 * updates it in lockstep with the real heap, and the checker computes the
 * brute-force reachable set from it after every collection.
 */
#ifndef M_IDS
#define M_IDS 4096
#endif
#ifndef M_F
#define M_F 2
#endif
#ifndef M_R
#define M_R 8
#endif
typedef struct {
    int edge[M_IDS][M_F];
    int root[M_R + 1]; /* last slot is the allocation temp root */
    int nids;
} Model;
static Model M;

static inline void model_reset(void) {
    M.nids = 0;
    for (int i = 0; i < M_IDS; i++)
        for (int k = 0; k < M_F; k++)
            M.edge[i][k] = -1;
    for (int i = 0; i <= M_R; i++)
        M.root[i] = -1;
}
static inline int model_new(void) {
    CHECK(M.nids < M_IDS);
    return M.nids++;
}
/* Brute-force oracle: iterative DFS over the shadow graph. */
static inline int model_reach(unsigned char *seen) {
    static int stack[M_IDS * M_F + M_R + 2];
    int sp = 0, count = 0;
    memset(seen, 0, (size_t)M_IDS);
    for (int i = 0; i <= M_R; i++)
        if (M.root[i] >= 0)
            stack[sp++] = M.root[i];
    while (sp > 0) {
        int id = stack[--sp];
        if (seen[id])
            continue;
        seen[id] = 1;
        count++;
        for (int k = 0; k < M_F; k++)
            if (M.edge[id][k] >= 0 && !seen[M.edge[id][k]])
                stack[sp++] = M.edge[id][k];
    }
    return count;
}
static long g_checks;
/*@VERIFY*/
/* present[id] != 0 when the heap still holds object id. exact: no floating garbage. */
static inline int model_verify(const unsigned char *present, int exact) {
    static unsigned char reach[M_IDS];
    int n = model_reach(reach);
    int extra = 0;
    for (int id = 0; id < M.nids; id++) {
        if (reach[id] && !present[id]) {
            fprintf(stderr, "live object %d lost\n", id);
            exit(1);
        }
        if (!reach[id] && present[id]) {
            if (exact) {
                fprintf(stderr, "garbage object %d survived\n", id);
                exit(1);
            }
            extra++;
        }
    }
    (void)extra;
    g_checks++;
    return n;
}

/*@ENDVERIFY*/
/*@DRIVE*/
typedef struct {
    void (*alloc_root)(int r, int id); /* allocate object id, store its ref in root r */
    int (*root)(int r);
    int (*get)(int ref, int k);
    void (*set)(int ref, int k, int tgt); /* includes any write barrier */
    void (*setroot)(int r, int ref);
    int (*idof)(int ref);
    void (*collect)(void); /* run a collection, then verify against the model */
    void (*tick)(void);    /* optional per-step hook (incremental work) */
} Ops;

static inline int walk(const Ops *o, int r, int depth, int *mid_out) {
    int ref = o->root(r), mid = M.root[r];
    if (ref < 0) {
        *mid_out = -1;
        return -1;
    }
    for (int d = 0; d < depth; d++) {
        int k = (int)rnd_n(M_F);
        int nx = o->get(ref, k);
        CHECK(nx < 0 ? M.edge[mid][k] < 0 : o->idof(nx) == M.edge[mid][k]);
        if (nx < 0)
            break;
        ref = nx;
        mid = M.edge[mid][k];
    }
    CHECK(o->idof(ref) == mid);
    *mid_out = mid;
    return ref;
}

/* Random mutator: allocate, link, share (cycles), unlink, drop roots. */
static inline void drive(const Ops *o, int steps, int every) {
    for (int s = 1; s <= steps; s++) {
        unsigned c = rnd_n(100);
        if (c < 40) {
            int id = model_new(), T = M_R;
            o->alloc_root(T, id);
            M.root[T] = id;
            int r = (int)rnd_n(M_R);
            unsigned mode = rnd_n(10);
            int mid;
            int p = walk(o, r, (int)rnd_n(9), &mid);
            if (p < 0 || mode < 2) {
                o->setroot(r, o->root(T));
                M.root[r] = id;
            } else {
                int k = (int)rnd_n(M_F);
                o->set(p, k, o->root(T));
                M.edge[mid][k] = id;
            }
            o->setroot(T, -1);
            M.root[T] = -1;
        } else if (c < 70) {
            int ra = (int)rnd_n(M_R), rb = (int)rnd_n(M_R);
            int ma, mb;
            int pa = walk(o, ra, (int)rnd_n(4), &ma);
            int pb = walk(o, rb, (int)rnd_n(4), &mb);
            if (pa >= 0 && pb >= 0) {
                int k = (int)rnd_n(M_F);
                o->set(pa, k, pb);
                M.edge[ma][k] = mb;
            }
        } else if (c < 78) {
            int r = (int)rnd_n(M_R), mid;
            int p = walk(o, r, (int)rnd_n(4), &mid);
            if (p >= 0) {
                int k = (int)rnd_n(M_F);
                o->set(p, k, -1);
                M.edge[mid][k] = -1;
            }
        } else {
            int r = (int)rnd_n(M_R);
            if (rnd_n(3) == 0) {
                o->setroot(r, -1);
                M.root[r] = -1;
            } else {
                int r2 = (int)rnd_n(M_R), mid;
                int p = walk(o, r2, (int)rnd_n(3), &mid);
                o->setroot(r, p);
                M.root[r] = mid;
            }
        }
        if (o->tick)
            o->tick();
        if (s % every == 0)
            o->collect();
    }
    o->collect();
}
/*@ENDDRIVE*/


#define CAP 110
#define TSIZE 128
#define MAXE 96

typedef struct {
    int id;
    int f[M_F];
    int fwd;
} Obj;

static Obj space[2][CAP];
static int cur, top;
static Obj *from, *to;
static int tfree;
static int roots[M_R + 1];

/* Weak identity table: entries are (key address, value); idx is the open-addressing index. */
static int ekey[MAXE], eval_[MAXE], nent;
static int idx[TSIZE]; /* entry position + 1, 0 when empty */
static int stale;
static unsigned char registered[M_IDS];
static int ncoll, n_puts, n_gets, probes, rehashes, weak_dropped, stale_hits, stale_misses, mismatches_checked;

static unsigned hash_addr(int a) { return ((unsigned)a * 2654435761u) >> 25; } /* 7 bits */

static int index_find(int key) {
    unsigned h = hash_addr(key);
    for (int i = 0; i < TSIZE; i++) {
        int slot = (int)((h + (unsigned)i) % TSIZE);
        probes++;
        if (idx[slot] == 0)
            return -1;
        if (idx[slot] - 1 < nent && ekey[idx[slot] - 1] == key)
            return idx[slot] - 1;
    }
    return -1;
}
static void index_insert(int pos) {
    unsigned h = hash_addr(ekey[pos]);
    for (int i = 0; i < TSIZE; i++) {
        int slot = (int)((h + (unsigned)i) % TSIZE);
        if (idx[slot] == 0) {
            idx[slot] = pos + 1;
            return;
        }
    }
    CHECK(0);
}
static void ensure_index(void) {
    if (!stale)
        return;
    memset(idx, 0, sizeof idx);
    for (int i = 0; i < nent; i++)
        index_insert(i);
    stale = 0;
    rehashes++;
}
static int table_get(int key) {
    ensure_index();
    n_gets++;
    int p = index_find(key);
    return p < 0 ? -1 : eval_[p];
}
static void table_put(int key, int val) {
    ensure_index();
    n_puts++;
    int p = index_find(key);
    if (p >= 0) {
        eval_[p] = val;
        return;
    }
    CHECK(nent < MAXE);
    ekey[nent] = key;
    eval_[nent] = val;
    index_insert(nent);
    nent++;
}

static int copy(int old) {
    if (old < 0)
        return -1;
    if (from[old].fwd >= 0)
        return from[old].fwd;
    int n = tfree++;
    to[n] = from[old];
    to[n].fwd = -1;
    from[old].fwd = n;
    for (int k = 0; k < M_F; k++)
        to[n].f[k] = copy(from[old].f[k]);
    return n;
}

static void gc(void) {
    from = space[cur];
    to = space[1 - cur];
    tfree = 0;
    for (int i = 0; i <= M_R; i++)
        roots[i] = copy(roots[i]);
    /* weak processing: only entries whose key was forwarded survive, with the key rewritten */
    int w = 0;
    for (int i = 0; i < nent; i++) {
        int nk = from[ekey[i]].fwd;
        if (nk < 0) {
            weak_dropped++;
            continue;
        }
        ekey[w] = nk;
        eval_[w] = eval_[i];
        w++;
    }
    nent = w;
    /* the index still hashes the OLD addresses: measure how wrong it is before rebuilding */
    for (int i = 0; i < nent; i++) {
        int p = index_find(ekey[i]);
        if (p == i)
            stale_hits++;
        else
            stale_misses++;
    }
    stale = 1;
    cur = 1 - cur;
    top = tfree;
    ncoll++;
    unsigned char present[M_IDS], reach[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < top; i++)
        present[space[cur][i].id] = 1;
    model_verify(present, 1);
    model_reach(reach);
    /* the table holds exactly the registered objects that are still reachable */
    unsigned char in_table[M_IDS];
    memset(in_table, 0, sizeof in_table);
    for (int i = 0; i < nent; i++) {
        int id = space[cur][ekey[i]].id;
        CHECK(registered[id] && !in_table[id]);
        in_table[id] = 1;
        CHECK(eval_[i] == id * 7 + 1);
    }
    for (int id = 0; id < M.nids; id++) {
        CHECK(in_table[id] == (registered[id] && reach[id]));
        mismatches_checked++;
    }
}

static const Ops *tick_ops;

static void tick(void) {
    unsigned c = rnd_n(6);
    if (c >= 3)
        return;
    int r = (int)rnd_n(M_R), mid;
    int p = walk(tick_ops, r, (int)rnd_n(4), &mid);
    if (p < 0)
        return;
    if (c == 0 && nent < MAXE - 1) {
        table_put(p, mid * 7 + 1);
        registered[mid] = 1;
    } else {
        int v = table_get(p);
        CHECK(v == (registered[mid] ? mid * 7 + 1 : -1));
    }
}

static void alloc_root(int r, int idv) {
    if (top == CAP)
        gc();
    CHECK(top < CAP);
    Obj *o = &space[cur][top];
    o->id = idv;
    o->fwd = -1;
    for (int k = 0; k < M_F; k++)
        o->f[k] = -1;
    roots[r] = top++;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return space[cur][ref].f[k]; }
static void set_f(int ref, int k, int t) { space[cur][ref].f[k] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return space[cur][ref].id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, tick};
    tick_ops = &o;
    drive(&o, 2200, 300);
    printf("objects allocated: %d, collections: %d\n", M.nids, ncoll);
    printf("puts: %d, gets: %d, probes: %d\n", n_puts, n_gets, probes);
    printf("entries dropped because the key died: %d, live entries at end: %d\n", weak_dropped, nent);
    printf("stale index before rehash: %d entries found, %d missed\n", stale_hits, stale_misses);
    printf("rehashes: %d\n", rehashes);
    printf("table membership checks: %d\n", mismatches_checked);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
