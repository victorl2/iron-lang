/*
 * title: Bartlett mostly-copying collector with page pinning
 * topic: memory
 * covers: ambiguous stack words, page-granular pinning of retained pages, Cheney evacuation of everything else, interior pointers, collateral retention of dead objects on pinned pages, independent page-level oracle, reachability check
 * deps: libc
 */
#define SEED 0x5EED001CULL
#define M_IDS 2048
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


#define NP 40
#define PW 32
#define OBJW 4
#define PERPAGE (PW / OBJW)
#define BASE 0x1000
#define NOISE 6
#define FWD 0x40000000
enum { P_FREE, P_USED, P_PINNED, P_TO };

static int H[NP * PW];
static unsigned char state[NP];
static int fill[NP]; /* objects allocated on the page */
static int ap = -1;
static int stk[M_R + 1 + NOISE];
static int to_list[NP], nto;
static int ncoll, pinned_total, true_pinned_total, copied, retained_garbage_total, free_pages_min = NP;

/* Layout per object: [id][f0][f1][data] */
static int page_of(int a) { return a / PW; }

static int free_pages(void) {
    int n = 0;
    for (int p = 0; p < NP; p++)
        n += state[p] == P_FREE;
    return n;
}

static int in_object(int w, int *a_out) { /* collector's decoder: arithmetic, interior pointers allowed */
    if (w < BASE || w >= BASE + NP * PW)
        return 0;
    int a = w - BASE, p = page_of(a);
    if (state[p] != P_USED || (a % PW) >= fill[p] * OBJW)
        return 0;
    *a_out = a - a % OBJW;
    return 1;
}

static int take_free_page(void) {
    for (int p = 0; p < NP; p++)
        if (state[p] == P_FREE) {
            state[p] = P_TO;
            fill[p] = 0;
            to_list[nto++] = p;
            return p;
        }
    fprintf(stderr, "no page for to-space\n");
    exit(1);
}

static int evacuate(int a) {
    if (a < 0)
        return -1;
    int p = page_of(a);
    if (state[p] == P_PINNED || state[p] == P_TO)
        return a;
    if (H[a] & FWD)
        return H[a] & ~FWD;
    int tp = nto ? to_list[nto - 1] : -1;
    if (tp < 0 || fill[tp] == PERPAGE)
        tp = take_free_page();
    int dst = tp * PW + fill[tp]++ * OBJW;
    memcpy(&H[dst], &H[a], OBJW * sizeof(int));
    H[a] = FWD | dst;
    copied++;
    return dst;
}

/* Oracle over a snapshot of the pre-collection heap: which object ids must survive? */
static void oracle(const int *snap, const unsigned char *sstate, const int *sfill, unsigned char *expect_ids,
                   int true_only, int *pinned_pages) {
    unsigned char pinned[NP];
    memset(pinned, 0, sizeof pinned);
    int limit = true_only ? M_R + 1 : M_R + 1 + NOISE;
    for (int i = 0; i < limit; i++)
        for (int p = 0; p < NP; p++)
            for (int s = 0; s < sfill[p]; s++) {
                int base = BASE + p * PW + s * OBJW;
                if (sstate[p] == P_USED && stk[i] >= base && stk[i] < base + OBJW)
                    pinned[p] = 1;
            }
    int work[NP * PERPAGE], nw = 0;
    unsigned char seen[NP * PW];
    memset(seen, 0, sizeof seen);
    memset(expect_ids, 0, M_IDS);
    *pinned_pages = 0;
    for (int p = 0; p < NP; p++) {
        if (!pinned[p])
            continue;
        (*pinned_pages)++;
        for (int s = 0; s < sfill[p]; s++) {
            int a = p * PW + s * OBJW;
            seen[a] = 1;
            work[nw++] = a;
        }
    }
    while (nw > 0) {
        int a = work[--nw];
        expect_ids[snap[a]] = 1;
        for (int k = 1; k <= 2; k++) {
            int t = snap[a + k];
            if (t >= 0 && !seen[t]) {
                seen[t] = 1;
                work[nw++] = t;
            }
        }
    }
}

static void gc(void) {
    int snap[NP * PW], sfill[NP];
    unsigned char sstate[NP], expect[M_IDS];
    memcpy(snap, H, sizeof H);
    memcpy(sstate, state, sizeof state);
    memcpy(sfill, fill, sizeof fill);
    int pinned_pages, true_pinned;
    unsigned char dummy[M_IDS];
    oracle(snap, sstate, sfill, expect, 0, &pinned_pages);
    oracle(snap, sstate, sfill, dummy, 1, &true_pinned);
    nto = 0;
    /* 1. pin every page an ambiguous word points into */
    int npin = 0;
    for (int i = 0; i < M_R + 1 + NOISE; i++) {
        int a;
        if (in_object(stk[i], &a) && state[page_of(a)] == P_USED) {
            state[page_of(a)] = P_PINNED;
            npin++;
        }
    }
    CHECK(npin == pinned_pages);
    /* 2. objects on pinned pages are retained wholesale; their referents are evacuated */
    for (int p = 0; p < NP; p++)
        if (state[p] == P_PINNED)
            for (int s = 0; s < fill[p]; s++) {
                int a = p * PW + s * OBJW;
                for (int k = 1; k <= 2; k++)
                    H[a + k] = evacuate(H[a + k]);
            }
    for (int si = 0, so = 0; si < nto;) {
        int tp = to_list[si];
        if (so < fill[tp]) {
            int a = tp * PW + so * OBJW;
            for (int k = 1; k <= 2; k++)
                H[a + k] = evacuate(H[a + k]);
            so++;
        } else if (si + 1 < nto) {
            si++;
            so = 0;
        } else {
            break;
        }
    }
    /* 3. release the old pages */
    for (int p = 0; p < NP; p++) {
        if (state[p] == P_USED) {
            state[p] = P_FREE;
            fill[p] = 0;
        } else if (state[p] == P_PINNED || state[p] == P_TO) {
            state[p] = P_USED;
        }
    }
    ap = -1;
    if (nto && fill[to_list[nto - 1]] < PERPAGE)
        ap = to_list[nto - 1];
    ncoll++;
    pinned_total += pinned_pages;
    true_pinned_total += true_pinned;
    /* 4. compare with the oracle and with true reachability */
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int p = 0; p < NP; p++)
        if (state[p] == P_USED)
            for (int s = 0; s < fill[p]; s++)
                present[H[p * PW + s * OBJW]] = 1;
    for (int id = 0; id < M.nids; id++)
        CHECK(present[id] == expect[id]);
    int n = model_verify(present, 0);
    int cnt = 0;
    for (int id = 0; id < M.nids; id++)
        cnt += present[id];
    retained_garbage_total += cnt - n;
    int fp = free_pages();
    if (fp < free_pages_min)
        free_pages_min = fp;
}

static void alloc_root(int r, int idv) {
    if (free_pages() < 10 && (ap < 0 || fill[ap] == PERPAGE))
        gc();
    if (ap < 0 || fill[ap] == PERPAGE) {
        for (ap = 0; ap < NP && state[ap] != P_FREE; ap++)
            ;
        CHECK(ap < NP);
        state[ap] = P_USED;
        fill[ap] = 0;
    }
    int a = ap * PW + fill[ap]++ * OBJW;
    H[a] = idv;
    H[a + 1] = H[a + 2] = -1;
    H[a + 3] = idv * 3;
    stk[r] = BASE + a;
}
static int get_root(int r) {
    int w = stk[r];
    if (w < BASE)
        return -1;
    int a = w - BASE;
    return a - a % OBJW;
}
static int get_f(int ref, int k) { return H[ref + 1 + k]; }
static void set_f(int ref, int k, int t) { H[ref + 1 + k] = t; }
static void set_root(int r, int ref) {
    if (ref < 0)
        stk[r] = 0;
    else
        stk[r] = BASE + ref + (r == M_R ? 0 : (int)rnd_n(OBJW)); /* interior pointers */
}
static int id_of(int ref) { return H[ref]; }

static void tick(void) {
    for (int i = 0; i < NOISE; i++)
        if (rnd_n(6) == 0)
            stk[M_R + 1 + i] = rnd_n(3) == 0 ? (int)rnd_n(4000) : BASE + (int)rnd_n(NP * PW + 60);
}

int main(void) {
    model_reset();
    for (int i = 0; i < M_R + 1 + NOISE; i++)
        stk[i] = 0;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, tick};
    drive(&o, 1600, 60);
    printf("objects allocated: %d\n", M.nids);
    printf("collections: %d, objects copied: %d\n", ncoll, copied);
    printf("pages pinned by ambiguous words: %d, by true roots alone: %d\n", pinned_total, true_pinned_total);
    printf("dead objects retained (pinned pages and floating): %d\n", retained_garbage_total);
    printf("fewest free pages after a collection: %d of %d\n", free_pages_min, NP);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
