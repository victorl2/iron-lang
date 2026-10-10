/*
 * title: Card marking write barrier with card-size sweep
 * topic: memory
 * covers: unconditional card-marking barrier, dirty card scanning at minor collection, card table soundness invariant, scan cost as a function of card size, promotion of nursery survivors, floating old garbage
 * deps: libc
 */
#define SEED 0x5EED000AULL
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


#define OLDCAP 1000
#define NUR 24
#define NREF (OLDCAP + NUR)
#define MAXCARDS OLDCAP

typedef struct {
    int id;
    int f[M_F];
    int fwd;
} Obj;

static Obj h[NREF];
static unsigned char card[MAXCARDS];
static int cs; /* slots per card */
static int oldtop, nptr;
static int roots[M_R + 1];
static int minors, promoted, barrier_execs, dirty_scanned, objs_scanned, floating;

static int is_young(int r) { return r >= OLDCAP; }

static int promote(int r) {
    if (r < 0 || !is_young(r))
        return r;
    if (h[r].fwd >= 0)
        return h[r].fwd;
    CHECK(oldtop < OLDCAP);
    int s = oldtop++;
    h[s] = h[r];
    h[r].fwd = s;
    promoted++;
    for (int k = 0; k < M_F; k++)
        h[s].f[k] = promote(h[r].f[k]);
    return s;
}

/* Soundness: any old object holding a young pointer must live in a dirty card. */
static void check_card_invariant(void) {
    for (int i = 0; i < oldtop; i++)
        for (int k = 0; k < M_F; k++)
            if (is_young(h[i].f[k]))
                CHECK(card[i / cs]);
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < oldtop; i++)
        present[h[i].id] = 1;
    for (int i = OLDCAP; i < nptr; i++)
        present[h[i].id] = 1;
    int n = model_verify(present, 0);
    int cnt = 0;
    for (int id = 0; id < M.nids; id++)
        cnt += present[id];
    floating = cnt - n;
}

static void minor(void) {
    check_card_invariant();
    for (int i = 0; i <= M_R; i++)
        roots[i] = promote(roots[i]);
    int ncards = (oldtop + cs - 1) / cs;
    for (int c = 0; c < ncards; c++) {
        if (!card[c])
            continue;
        dirty_scanned++;
        int end = (c + 1) * cs < oldtop ? (c + 1) * cs : oldtop;
        /* promotion appends fully resolved copies past the scanned range */
        for (int i = c * cs; i < end; i++) {
            objs_scanned++;
            for (int k = 0; k < M_F; k++)
                h[i].f[k] = promote(h[i].f[k]);
        }
        card[c] = 0;
    }
    for (int i = OLDCAP; i < nptr; i++)
        h[i].fwd = -1;
    nptr = OLDCAP;
    memset(card, 0, sizeof card);
    minors++;
    verify();
}

static void alloc_root(int r, int id) {
    if (nptr == NREF)
        minor();
    int s = nptr++;
    h[s].id = id;
    h[s].fwd = -1;
    for (int k = 0; k < M_F; k++)
        h[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return h[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    h[ref].f[k] = t;
    if (!is_young(ref)) { /* card mark: no test of the stored value */
        card[ref / cs] = 1;
        barrier_execs++;
    }
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return h[ref].id; }

static void run(int card_slots) {
    cs = card_slots;
    memset(card, 0, sizeof card);
    oldtop = 0;
    nptr = OLDCAP;
    minors = promoted = barrier_execs = dirty_scanned = objs_scanned = 0;
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    rng_s = SEED;
    model_reset();
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, minor, NULL};
    drive(&o, 1500, 500);
}

int main(void) {
    static const int sizes[] = {1, 4, 16, 64, 256};
    int base_promoted = -1;
    printf("card  minors dirty-cards objs-scanned barrier-execs\n");
    for (int s = 0; s < 5; s++) {
        run(sizes[s]);
        if (base_promoted < 0)
            base_promoted = promoted;
        CHECK(promoted == base_promoted); /* card size never changes what survives */
        printf("%4d  %6d %11d %12d %13d\n", sizes[s], minors, dirty_scanned, objs_scanned, barrier_execs);
    }
    printf("objects allocated: %d, promoted: %d\n", M.nids, promoted);
    printf("old garbage never collected: %d\n", floating);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
