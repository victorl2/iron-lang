/*
 * title: Precise marking with per-type pointer maps
 * topic: memory
 * covers: object headers carrying a type index, type descriptors as pointer bitmaps, integer words that alias heap indices, precise mark-sweep versus a conservative marker on the same heap, false retention avoided, reachability oracle
 * deps: libc
 */
#define SEED 0x5EED0024ULL
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


#define CAP 150
#define NW 6 /* payload words per cell */
#define NTYPES 4

/* Word positions of the M_F (=2) pointer fields of each type; all other words are integers. */
static const int ptr_word[NTYPES][M_F] = {{0, 1}, {2, 5}, {1, 3}, {4, 0}};
static const char *type_name[NTYPES] = {"pair", "spread", "mixed", "wrapped"};
static unsigned type_mask[NTYPES]; /* pointer bitmap, derived from ptr_word at startup */

static int H[CAP][1 + NW]; /* [0] = type << 16 | id, [1..NW] payload */
static unsigned char used[CAP], markp[CAP], markc[CAP];
static int freelist;
static int roots[M_R + 1];
static int ncoll, swept, precise_total, conservative_total, aliasing_ints;

static int type_of(int c) { return H[c][0] >> 16; }
static int idw(int c) { return H[c][0] & 0xFFFF; }

static void mark_precise(int c) {
    if (c < 0 || markp[c])
        return;
    markp[c] = 1;
    for (int w = 0; w < NW; w++)
        if ((type_mask[type_of(c)] >> w) & 1)
            mark_precise(H[c][1 + w]);
}
/* Conservative comparison marker: every payload word that looks like a cell index counts. */
static void mark_conservative(int c) {
    if (c < 0 || c >= CAP || !used[c] || markc[c])
        return;
    markc[c] = 1;
    for (int w = 0; w < NW; w++)
        mark_conservative(H[c][1 + w]);
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int c = 0; c < CAP; c++)
        if (used[c])
            present[idw(c)] = 1;
    model_verify(present, 1);
}

static void gc(void) {
    memset(markc, 0, sizeof markc);
    for (int i = 0; i <= M_R; i++) {
        mark_precise(roots[i]);
        mark_conservative(roots[i]);
    }
    int p = 0, c = 0;
    freelist = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        p += markp[i];
        c += markc[i];
        CHECK(!markp[i] || markc[i]); /* everything precise finds, conservative finds too */
        if (used[i] && !markp[i]) {
            used[i] = 0;
            swept++;
        }
        markp[i] = 0;
        if (!used[i]) {
            H[i][1] = freelist;
            freelist = i;
        }
    }
    precise_total += p;
    conservative_total += c;
    ncoll++;
    verify();
}

static void alloc_root(int r, int idv) {
    if (freelist < 0)
        gc();
    CHECK(freelist >= 0);
    int s = freelist;
    freelist = H[s][1];
    used[s] = 1;
    int t = idv % NTYPES;
    H[s][0] = t << 16 | idv;
    for (int w = 0; w < NW; w++) {
        if ((type_mask[t] >> w) & 1)
            H[s][1 + w] = -1;
        else if (rnd_n(5) < 3) {
            H[s][1 + w] = (int)rnd_n(CAP); /* an integer that happens to look like a cell index */
            aliasing_ints++;
        } else
            H[s][1 + w] = (int)rnd_n(1000) + CAP;
    }
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return H[ref][1 + ptr_word[type_of(ref)][k]]; }
static void set_f(int ref, int k, int t) { H[ref][1 + ptr_word[type_of(ref)][k]] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return idw(ref); }

int main(void) {
    for (int t = 0; t < NTYPES; t++)
        for (int k = 0; k < M_F; k++)
            type_mask[t] |= 1u << ptr_word[t][k];
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    freelist = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        H[i][1] = freelist;
        freelist = i;
    }
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, NULL};
    drive(&o, 2200, 90);
    printf("type descriptors (pointer bitmap over %d payload words):\n", NW);
    for (int t = 0; t < NTYPES; t++) {
        char bits[NW + 1];
        for (int w = 0; w < NW; w++)
            bits[w] = ((type_mask[t] >> w) & 1) ? 'P' : 'i';
        bits[NW] = 0;
        printf("  %d %-8s %s\n", t, type_name[t], bits);
    }
    printf("objects allocated: %d, collections: %d, swept: %d\n", M.nids, ncoll, swept);
    printf("integer words that alias a cell index: %d\n", aliasing_ints);
    printf("survivors summed over collections: precise %d, conservative would keep %d\n", precise_total,
           conservative_total);
    printf("false retention avoided: %d\n", conservative_total - precise_total);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
