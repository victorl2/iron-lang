/*
 * title: Lisp 2 sliding mark-compact collector
 * topic: memory
 * covers: variable-size objects in a word heap, forwarding word per object, four phases (mark, compute addresses, update pointers, slide), order-preserving compaction, payload integrity, reachability oracle
 * deps: libc
 */
#define SEED 0x5EED0005ULL
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


#define HW 300
#define MARKBIT 0x40000000
#define IDMASK 0xFFFFF

static int H[HW];
static int top;
static int roots[M_R + 1];
static int ncoll, moved_objs, words_reclaimed, words_slid, scans;

/* Layout: [header][forward][field 0..M_F-1][payload words]. */
static int osize(int id) { return 2 + M_F + id % 3; }
static int sz_at(int a) { return osize(H[a] & IDMASK); }
static int payload(int id, int j) { return (id * 31 + j * 7 + 5) & 0xFFFF; }

static void mark(int ref) {
    if (ref < 0 || (H[ref] & MARKBIT))
        return;
    H[ref] |= MARKBIT;
    for (int k = 0; k < M_F; k++)
        mark(H[ref + 2 + k]);
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int a = 0; a < top; a += sz_at(a)) {
        int id = H[a] & IDMASK;
        present[id] = 1;
        for (int j = 0; j < id % 3; j++)
            CHECK(H[a + 2 + M_F + j] == payload(id, j));
    }
    model_verify(present, 1);
}

static void gc(void) {
    int before = top;
    for (int i = 0; i <= M_R; i++)
        mark(roots[i]);
    int dest = 0;
    for (int a = 0; a < top; a += sz_at(a)) { /* phase 2: assign new addresses */
        scans++;
        if (H[a] & MARKBIT) {
            H[a + 1] = dest;
            dest += sz_at(a);
        }
    }
    for (int i = 0; i <= M_R; i++) /* phase 3: update references */
        if (roots[i] >= 0)
            roots[i] = H[roots[i] + 1];
    for (int a = 0; a < top; a += sz_at(a))
        if (H[a] & MARKBIT)
            for (int k = 0; k < M_F; k++) {
                int t = H[a + 2 + k];
                if (t >= 0)
                    H[a + 2 + k] = H[t + 1];
            }
    for (int a = 0; a < top;) { /* phase 4: slide down */
        int n = sz_at(a);
        if (H[a] & MARKBIT) {
            int d = H[a + 1];
            H[a] &= ~MARKBIT;
            if (d != a) {
                memmove(&H[d], &H[a], (size_t)n * sizeof H[0]);
                moved_objs++;
                words_slid += n;
            }
            H[d + 1] = -1;
        }
        a += n;
    }
    top = dest;
    words_reclaimed += before - top;
    ncoll++;
    verify();
}

static void alloc_root(int r, int id) {
    int need = osize(id);
    if (top + need > HW)
        gc();
    CHECK(top + need <= HW);
    int a = top;
    top += need;
    H[a] = id;
    H[a + 1] = -1;
    for (int k = 0; k < M_F; k++)
        H[a + 2 + k] = -1;
    for (int j = 0; j < id % 3; j++)
        H[a + 2 + M_F + j] = payload(id, j);
    roots[r] = a;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return H[ref + 2 + k]; }
static void set_f(int ref, int k, int t) { H[ref + 2 + k] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return H[ref] & IDMASK; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, NULL};
    drive(&o, 1800, 131);
    printf("objects allocated: %d\n", M.nids);
    printf("compactions: %d\n", ncoll);
    printf("objects moved: %d (%d words slid)\n", moved_objs, words_slid);
    printf("words reclaimed: %d, heap top at end: %d of %d\n", words_reclaimed, top, HW);
    printf("linear heap scans: %d\n", scans);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
