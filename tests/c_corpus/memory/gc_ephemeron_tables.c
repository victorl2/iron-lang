/*
 * title: Ephemeron table marking to a fixed point
 * topic: memory
 * covers: ephemerons (value reachable only if key reachable), iterative fixed-point marking, value-refers-to-key cycles, chained ephemerons, entry removal for dead keys, comparison against a strong-value table, brute-force oracle
 * deps: libc
 */
#define SEED 0x5EED0011ULL
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


#define CAP 170
#define MAXE 300

typedef struct {
    int id;
    int f[M_F];
    unsigned char used, mark;
} Obj;

typedef struct {
    int key, val;   /* heap slots */
    int kid, vid;   /* object ids, for the oracle */
} Eph;

static Obj heap[CAP];
static Eph eph[MAXE];
static int neph;
static int freelist;
static int roots[M_R + 1];
static const Ops *g_ops;
static int cycles, created, broken, eph_marked, iters_total, iters_max, swept, strong_extra, cycle_entries;

static int mark(int o) {
    if (o < 0 || heap[o].mark)
        return 0;
    heap[o].mark = 1;
    int n = 1;
    for (int k = 0; k < M_F; k++)
        n += mark(heap[o].f[k]);
    return n;
}

static void dfs_ids(int id, unsigned char *seen) {
    if (id < 0 || seen[id])
        return;
    seen[id] = 1;
    for (int k = 0; k < M_F; k++)
        dfs_ids(M.edge[id][k], seen);
}

/* Oracle: naive repeated passes over the entry list until nothing changes. */
static void oracle(unsigned char *seen, int *strong_only) {
    model_reach(seen);
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < neph; i++)
            if (seen[eph[i].kid] && !seen[eph[i].vid]) {
                dfs_ids(eph[i].vid, seen);
                changed = 1;
            }
    }
    /* what a table holding its values strongly would retain */
    unsigned char s2[M_IDS];
    model_reach(s2);
    for (int i = 0; i < neph; i++)
        dfs_ids(eph[i].vid, s2);
    int a = 0, b = 0;
    for (int id = 0; id < M.nids; id++) {
        a += seen[id];
        b += s2[id];
    }
    *strong_only = b - a;
}

static void gc(void) {
    unsigned char expect[M_IDS];
    int extra;
    oracle(expect, &extra);
    strong_extra += extra;
    for (int i = 0; i <= M_R; i++)
        mark(roots[i]);
    int iters = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        iters++;
        for (int i = 0; i < neph; i++)
            if (heap[eph[i].key].mark && !heap[eph[i].val].mark) {
                eph_marked += mark(eph[i].val);
                changed = 1;
            }
    }
    iters_total += iters;
    if (iters > iters_max)
        iters_max = iters;
    int w = 0;
    for (int i = 0; i < neph; i++) {
        if (heap[eph[i].key].mark)
            eph[w++] = eph[i];
        else
            broken++;
    }
    neph = w;
    freelist = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        if (heap[i].used && !heap[i].mark) {
            heap[i].used = 0;
            swept++;
        }
        heap[i].mark = 0;
        if (!heap[i].used) {
            heap[i].f[0] = freelist;
            freelist = i;
        }
    }
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < CAP; i++)
        if (heap[i].used)
            present[heap[i].id] = 1;
    for (int id = 0; id < M.nids; id++)
        CHECK(present[id] == expect[id]); /* exactly the ephemeron-reachable set */
    g_checks++;
    cycles++;
}

static void tick(void) {
    if (rnd_n(4) != 0 || neph >= MAXE - 1)
        return;
    int r1 = (int)rnd_n(M_R), r2 = (int)rnd_n(M_R), mk, mv;
    int pk = walk(g_ops, r1, 3, &mk);
    int pv = walk(g_ops, r2, 3, &mv);
    if (pk < 0 || pv < 0 || pk == pv)
        return;
    eph[neph].key = pk;
    eph[neph].val = pv;
    eph[neph].kid = mk;
    eph[neph].vid = mv;
    neph++;
    created++;
    if (rnd_n(2) == 0) { /* value points back at its key: a strong table could never free either */
        g_ops->set(pv, 0, pk);
        M.edge[mv][0] = mk;
        cycle_entries++;
    }
}

static void alloc_root(int r, int id) {
    if (freelist < 0)
        gc();
    CHECK(freelist >= 0);
    int s = freelist;
    freelist = heap[s].f[0];
    heap[s].used = 1;
    heap[s].mark = 0;
    heap[s].id = id;
    for (int k = 0; k < M_F; k++)
        heap[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return heap[ref].f[k]; }
static void set_f(int ref, int k, int t) { heap[ref].f[k] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return heap[ref].id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    freelist = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        heap[i].f[0] = freelist;
        freelist = i;
    }
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, tick};
    g_ops = &o;
    drive(&o, 1800, 137);
    int live = 0;
    for (int i = 0; i < CAP; i++)
        live += heap[i].used;
    printf("objects allocated: %d\n", M.nids);
    printf("collections: %d, swept: %d\n", cycles, swept);
    printf("ephemeron entries created: %d (value refers to key: %d), broken: %d, live at end: %d\n", created,
           cycle_entries, broken, neph);
    printf("objects kept only through ephemerons: %d\n", eph_marked);
    printf("fixed-point passes: %d total, %d max in one collection\n", iters_total, iters_max);
    printf("extra objects a strong-value table would have kept: %d\n", strong_extra);
    printf("live objects at end: %d\n", live);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
