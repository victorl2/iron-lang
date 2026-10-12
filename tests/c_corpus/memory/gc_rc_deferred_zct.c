/*
 * title: Deferred reference counting with a zero count table
 * topic: memory
 * covers: heap-only reference counts, stack references not counted, zero count table, reconciliation against roots, recursive release, refcount equals in-degree invariant, cyclic garbage leak measurement
 * deps: libc
 */
#define SEED 0x5EED000EULL
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


#define CAP 420
#define ZCT_LIMIT 24

typedef struct {
    int id;
    int f[M_F];
    int rc; /* counts references from heap fields only */
    unsigned char used, inzct;
} Obj;

static Obj heap[CAP];
static int freelist;
static int roots[M_R + 1];
static int zct[CAP * 2], nzct;
static unsigned char isroot[CAP];
static int reconciles, freed, kept_by_stack, zct_peak, rc_ops, saved_by_deferral;

static void zct_add(int o) {
    if (!heap[o].inzct) {
        heap[o].inzct = 1;
        zct[nzct++] = o;
        if (nzct > zct_peak)
            zct_peak = nzct;
    }
}

static void release(int o, int *keep, int *nkeep) {
    heap[o].used = 0;
    heap[o].inzct = 0;
    freed++;
    for (int k = 0; k < M_F; k++) {
        int c = heap[o].f[k];
        heap[o].f[k] = -1;
        if (c < 0)
            continue;
        rc_ops++;
        if (--heap[c].rc == 0 && heap[c].used) {
            if (isroot[c])
                keep[(*nkeep)++] = c;
            else
                release(c, keep, nkeep);
        }
    }
    heap[o].f[0] = freelist;
    freelist = o;
}

static void check_counts(void) {
    static int indeg[CAP];
    memset(indeg, 0, sizeof indeg);
    for (int i = 0; i < CAP; i++)
        if (heap[i].used)
            for (int k = 0; k < M_F; k++)
                if (heap[i].f[k] >= 0)
                    indeg[heap[i].f[k]]++;
    for (int i = 0; i < CAP; i++)
        if (heap[i].used)
            CHECK(heap[i].rc == indeg[i]);
}

static void reconcile(void) {
    memset(isroot, 0, sizeof isroot);
    for (int i = 0; i <= M_R; i++)
        if (roots[i] >= 0)
            isroot[roots[i]] = 1;
    static int keep[CAP * 2];
    int nkeep = 0;
    for (int i = 0; i < nzct; i++) {
        int o = zct[i];
        heap[o].inzct = 0;
        if (!heap[o].used || heap[o].rc > 0)
            continue;
        if (isroot[o]) {
            keep[nkeep++] = o;
            kept_by_stack++;
        } else {
            release(o, keep, &nkeep);
        }
    }
    nzct = 0;
    for (int i = 0; i < nkeep; i++)
        if (heap[keep[i]].used && heap[keep[i]].rc == 0)
            zct_add(keep[i]);
    reconciles++;
    check_counts();
    /* No acyclic garbage may remain: a zero-count object must be stack-referenced. */
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && heap[i].rc == 0)
            CHECK(isroot[i]);
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < CAP; i++)
        if (heap[i].used)
            present[heap[i].id] = 1;
    model_verify(present, 0);
}

static void collect(void) { reconcile(); }

static void alloc_root(int r, int id) {
    if (freelist < 0 || nzct >= ZCT_LIMIT)
        reconcile();
    if (freelist < 0) {
        fprintf(stderr, "heap exhausted by cyclic garbage\n");
        exit(1);
    }
    int s = freelist;
    freelist = heap[s].f[0];
    heap[s].used = 1;
    heap[s].id = id;
    heap[s].rc = 0;
    heap[s].inzct = 0;
    for (int k = 0; k < M_F; k++)
        heap[s].f[k] = -1;
    zct_add(s);
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return heap[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    int old = heap[ref].f[k];
    if (t >= 0)
        heap[t].rc++;
    if (old >= 0 && --heap[old].rc == 0)
        zct_add(old); /* deferred: the stack might still hold it */
    heap[ref].f[k] = t;
    rc_ops += (t >= 0) + (old >= 0);
}
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
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, collect, NULL};
    drive(&o, 1500, 211);
    reconcile();
    int present_n = 0;
    for (int i = 0; i < CAP; i++)
        present_n += heap[i].used;
    unsigned char reach[M_IDS];
    int live = model_reach(reach);
    saved_by_deferral = kept_by_stack;
    printf("objects allocated: %d, freed by counting: %d\n", M.nids, freed);
    printf("reconciliations: %d, zct peak: %d\n", reconciles, zct_peak);
    printf("zero-count objects kept alive by the stack: %d\n", saved_by_deferral);
    printf("heap-field count updates: %d\n", rc_ops);
    printf("reachable at end: %d, present: %d, leaked cyclic garbage: %d\n", live, present_n, present_n - live);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
