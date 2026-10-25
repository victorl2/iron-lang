/*
 * title: Sticky mark bit generational collector
 * topic: memory
 * covers: non-moving generations from persistent mark bits, minor collections that trace only unmarked objects, dirty-object remembered set from a barrier on marked sources, full collection clearing all bits, marked-implies-children-marked invariant, work comparison against always-full collection
 * deps: libc
 */
#define SEED 0x5EED0025ULL
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


#define CAP 190

typedef struct {
    int id;
    int f[M_F];
    unsigned char used, mark, dirty;
} Obj;

static Obj heap[CAP];
static int freelist, nfree;
static int roots[M_R + 1];
static int rem[CAP], nrem;
static int minors, fulls, barrier_hits, minor_work, full_work, would_be_full_work, swept_minor, swept_full;
static int floating_after_minor, minor_calls;

static int trace_young(int o, int *stack, int *sp) {
    if (o < 0 || heap[o].mark)
        return 0;
    heap[o].mark = 1;
    stack[(*sp)++] = o;
    return 1;
}

static void rebuild_freelist(void) {
    freelist = -1;
    nfree = 0;
    for (int i = CAP - 1; i >= 0; i--)
        if (!heap[i].used) {
            heap[i].f[0] = freelist;
            freelist = i;
            nfree++;
        }
}

/* Marked objects never point at unmarked ones once a collection has finished. */
static void check_closure(void) {
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && heap[i].mark)
            for (int k = 0; k < M_F; k++)
                CHECK(heap[i].f[k] < 0 || heap[heap[i].f[k]].mark);
}

static void collect_generic(int full) {
    int stack[CAP], sp = 0, work = 0;
    unsigned char pre_reach[M_IDS];
    int reach_now = model_reach(pre_reach);
    if (full) {
        for (int i = 0; i < CAP; i++)
            heap[i].mark = 0;
        for (int i = 0; i < CAP; i++)
            heap[i].dirty = 0;
        nrem = 0;
    }
    for (int i = 0; i <= M_R; i++)
        work += trace_young(roots[i], stack, &sp);
    for (int i = 0; i < nrem; i++) {
        int o = rem[i];
        heap[o].dirty = 0;
        for (int k = 0; k < M_F; k++)
            work += trace_young(heap[o].f[k], stack, &sp);
    }
    nrem = 0;
    while (sp > 0) {
        int o = stack[--sp];
        for (int k = 0; k < M_F; k++)
            work += trace_young(heap[o].f[k], stack, &sp);
    }
    int swept = 0;
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && !heap[i].mark) {
            heap[i].used = 0;
            swept++;
        }
    rebuild_freelist();
    check_closure();
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    int cnt = 0;
    for (int i = 0; i < CAP; i++)
        if (heap[i].used) {
            present[heap[i].id] = 1;
            cnt++;
        }
    model_verify(present, full);
    would_be_full_work += reach_now;
    if (full) {
        fulls++;
        full_work += work;
        swept_full += swept;
    } else {
        minors++;
        minor_work += work;
        swept_minor += swept;
        floating_after_minor += cnt - reach_now;
    }
}

static void collect(void) {
    minor_calls++;
    collect_generic(minor_calls % 5 == 0);
}

static void alloc_root(int r, int idv) {
    if (freelist < 0) {
        collect_generic(0);
        if (freelist < 0 || nfree < CAP / 8)
            collect_generic(1);
    }
    CHECK(freelist >= 0);
    int s = freelist;
    freelist = heap[s].f[0];
    nfree--;
    heap[s].used = 1;
    heap[s].mark = 0; /* new objects are young */
    heap[s].dirty = 0;
    heap[s].id = idv;
    for (int k = 0; k < M_F; k++)
        heap[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return heap[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    heap[ref].f[k] = t;
    if (heap[ref].mark && t >= 0 && !heap[t].mark && !heap[ref].dirty) {
        heap[ref].dirty = 1;
        rem[nrem++] = ref;
        barrier_hits++;
    }
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return heap[ref].id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    rebuild_freelist();
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, collect, NULL};
    drive(&o, 2600, 37);
    printf("objects allocated: %d\n", M.nids);
    printf("minor collections: %d (work %d, swept %d), full: %d (work %d, swept %d)\n", minors, minor_work, swept_minor,
           fulls, full_work, swept_full);
    printf("marking work if every collection had been full: %d\n", would_be_full_work);
    printf("barrier recorded %d dirty sources\n", barrier_hits);
    printf("old garbage kept by minors, summed: %d\n", floating_after_minor);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
