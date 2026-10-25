/*
 * title: Generational collector with nursery, promotion and remembered set
 * topic: memory
 * covers: young and old spaces in one index space, promotion of survivors, write barrier recording old-to-young stores, remembered set pruning, minor versus major collections, barrier ablation scenario, reachability oracle
 * deps: libc
 */
#define SEED 0x5EED0009ULL
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


#define OLD 130
#define NUR 20

typedef struct {
    int id;
    int f[M_F];
    int fwd;
    unsigned char used, inrem, mark;
} Obj;

static Obj h[OLD + NUR];
static int oldfree, noldfree;
static int nptr = OLD;
static int rem[OLD], nrem;
static int roots[M_R + 1];
static int minors, majors, promoted, barrier_hits, rem_scanned, swept_old;
static int barrier_enabled = 1, verify_on = 1;

static int is_young(int r) { return r >= OLD; }

static int promote(int r) {
    if (r < 0 || !is_young(r))
        return r;
    if (h[r].fwd >= 0)
        return h[r].fwd;
    int s = oldfree;
    CHECK(s >= 0);
    oldfree = h[s].f[0];
    noldfree--;
    h[s] = h[r];
    h[s].inrem = 0;
    h[s].used = 1;
    h[r].fwd = s;
    promoted++;
    for (int k = 0; k < M_F; k++)
        h[s].f[k] = promote(h[r].f[k]);
    return s;
}

static void verify(int exact) {
    if (!verify_on)
        return;
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < OLD; i++)
        if (h[i].used)
            present[h[i].id] = 1;
    for (int i = OLD; i < nptr; i++)
        present[h[i].id] = 1;
    model_verify(present, exact);
}

static void minor(void) {
    for (int i = 0; i <= M_R; i++)
        roots[i] = promote(roots[i]);
    for (int i = 0; i < nrem; i++) {
        int o = rem[i];
        h[o].inrem = 0;
        rem_scanned++;
        for (int k = 0; k < M_F; k++)
            h[o].f[k] = promote(h[o].f[k]);
    }
    nrem = 0;
    for (int i = OLD; i < nptr; i++) {
        h[i].fwd = -1;
        h[i].used = 0;
    }
    nptr = OLD;
    minors++;
}

static void mark(int r) {
    if (r < 0 || h[r].mark)
        return;
    h[r].mark = 1;
    for (int k = 0; k < M_F; k++)
        mark(h[r].f[k]);
}

static void major(void) {
    for (int i = 0; i <= M_R; i++)
        mark(roots[i]);
    for (int i = 0; i < OLD; i++) {
        if (h[i].used && !h[i].mark) {
            h[i].used = 0;
            h[i].inrem = 0;
            h[i].f[0] = oldfree;
            oldfree = i;
            noldfree++;
            swept_old++;
        }
        h[i].mark = 0;
    }
    for (int i = OLD; i < nptr; i++)
        h[i].mark = 0;
    int w = 0;
    for (int i = 0; i < nrem; i++)
        if (h[rem[i]].used)
            rem[w++] = rem[i];
        else
            CHECK(!h[rem[i]].inrem);
    nrem = w;
    majors++;
    minor();
}

static void collect_full(void) {
    major();
    verify(1);
}
static void collect_minor(void) {
    minor();
    verify(0);
}

static int collect_calls;
static void collect(void) {
    if (++collect_calls % 4 == 0 || noldfree < nptr - OLD)
        collect_full();
    else
        collect_minor();
}

static void alloc_root(int r, int id) {
    if (nptr == OLD + NUR) {
        if (noldfree < NUR)
            collect_full();
        else
            collect_minor();
    }
    int s = nptr++;
    h[s].id = id;
    h[s].fwd = -1;
    h[s].used = 1;
    h[s].inrem = 0;
    h[s].mark = 0;
    for (int k = 0; k < M_F; k++)
        h[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return h[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    h[ref].f[k] = t;
    if (barrier_enabled && !is_young(ref) && t >= 0 && is_young(t) && !h[ref].inrem) {
        h[ref].inrem = 1;
        rem[nrem++] = ref;
        barrier_hits++;
    }
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return h[ref].id; }

static void heap_reset(void) {
    oldfree = -1;
    noldfree = 0;
    for (int i = OLD - 1; i >= 0; i--) {
        memset(&h[i], 0, sizeof h[i]);
        h[i].f[0] = oldfree;
        oldfree = i;
        noldfree++;
    }
    nptr = OLD;
    nrem = 0;
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    model_reset();
}

/* One old object gets a pointer to a fresh young object that is otherwise unreferenced. */
static int lost_after_scenario(int with_barrier) {
    heap_reset();
    barrier_enabled = with_barrier;
    verify_on = 0;
    int a = model_new();
    alloc_root(0, a);
    M.root[0] = a;
    minor(); /* a is now old */
    int b = model_new();
    alloc_root(1, b);
    set_f(roots[0], 0, roots[1]);
    M.edge[a][0] = b;
    roots[1] = -1;
    minor();
    unsigned char reach[M_IDS];
    model_reach(reach);
    int lost = 0;
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < OLD; i++)
        if (h[i].used)
            present[h[i].id] = 1;
    for (int id = 0; id < M.nids; id++)
        if (reach[id] && !present[id])
            lost++;
    verify_on = 1;
    barrier_enabled = 1;
    return lost;
}

int main(void) {
    int lost_with = lost_after_scenario(1);
    int lost_without = lost_after_scenario(0);
    CHECK(lost_with == 0 && lost_without == 1);
    heap_reset();
    minors = majors = promoted = barrier_hits = rem_scanned = swept_old = 0;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, collect, NULL};
    drive(&o, 2000, 41);
    printf("barrier ablation: lost with barrier %d, without barrier %d\n", lost_with, lost_without);
    printf("objects allocated: %d\n", M.nids);
    printf("minor passes: %d, of which %d followed a full old-space sweep\n", minors, majors);
    printf("objects promoted: %d, old objects swept: %d\n", promoted, swept_old);
    printf("barrier recorded: %d, remembered entries scanned: %d\n", barrier_hits, rem_scanned);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
