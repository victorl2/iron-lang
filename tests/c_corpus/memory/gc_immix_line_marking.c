/*
 * title: Immix-style line marking with hole-filling bump allocation
 * topic: memory
 * covers: blocks of lines, objects spanning several lines, line mark table computed from live objects, holes as runs of free lines, bump allocation within holes, abandoned hole tails, fully free block accounting, ASCII line map, reachability oracle
 * deps: libc
 */
#define SEED 0x5EED001DULL
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


#define NB 6
#define LPB 16 /* lines per block */
#define LG 4   /* granules per line */
#define NL (NB * LPB)
#define NG (NL * LG)

/* An object occupies 1..6 granules starting at its granule index. */
static int oid[NG]; /* -1 when no object starts here */
static int of[NG][M_F];
static unsigned char omark[NG];
static unsigned char line_live[NL];
static int roots[M_R + 1];
static int cur, limit, next_line;
static int ncoll, holes, wasted, allocs_total, free_blocks_total, lines_live_total, oversize_retries;

static int size_of(int idv) { return 1 + idv % 6; }
static int size_at(int g) { return size_of(oid[g]); }

static void mark(int g) {
    if (g < 0 || omark[g])
        return;
    omark[g] = 1;
    for (int k = 0; k < M_F; k++)
        mark(of[g][k]);
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int g = 0; g < NG; g++)
        if (oid[g] >= 0) {
            present[oid[g]] = 1;
            /* no two objects overlap */
            for (int j = 1; j < size_at(g); j++)
                CHECK(oid[g + j] < 0);
        }
    model_verify(present, 1);
}

static void gc(void) {
    for (int i = 0; i <= M_R; i++)
        mark(roots[i]);
    memset(line_live, 0, sizeof line_live);
    for (int g = 0; g < NG; g++) {
        if (oid[g] < 0)
            continue;
        if (!omark[g]) {
            oid[g] = -1;
        } else {
            for (int l = g / LG; l <= (g + size_at(g) - 1) / LG; l++)
                line_live[l] = 1;
            omark[g] = 0;
        }
    }
    int fb = 0;
    for (int b = 0; b < NB; b++) {
        int any = 0;
        for (int l = 0; l < LPB; l++)
            any |= line_live[b * LPB + l];
        fb += !any;
    }
    for (int l = 0; l < NL; l++)
        lines_live_total += line_live[l];
    free_blocks_total += fb;
    cur = limit = 0;
    next_line = 0;
    ncoll++;
    verify();
}

/* Find the next hole: a run of free lines inside one block. Returns 0 when the heap is exhausted. */
static int next_hole(void) {
    while (next_line < NL && line_live[next_line])
        next_line++;
    if (next_line >= NL)
        return 0;
    int start = next_line;
    int block_end = (start / LPB + 1) * LPB;
    int end = start;
    while (end < block_end && !line_live[end])
        end++;
    next_line = end;
    cur = start * LG;
    limit = end * LG;
    holes++;
    return 1;
}

static int bump(int n) {
    for (;;) {
        if (cur + n <= limit) {
            int g = cur;
            cur += n;
            /* the lines under a new object now count as used */
            for (int l = g / LG; l <= (g + n - 1) / LG; l++)
                line_live[l] = 1;
            return g;
        }
        wasted += limit - cur;
        if (!next_hole())
            return -1;
    }
}

static void alloc_root(int r, int idv) {
    int n = size_of(idv);
    int g = bump(n);
    if (g < 0) {
        gc();
        oversize_retries++;
        g = bump(n);
        if (g < 0) {
            fprintf(stderr, "heap exhausted\n");
            exit(1);
        }
    }
    oid[g] = idv;
    for (int k = 0; k < M_F; k++)
        of[g][k] = -1;
    omark[g] = 0;
    allocs_total++;
    roots[r] = g;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return of[ref][k]; }
static void set_f(int ref, int k, int t) { of[ref][k] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return oid[ref]; }

static void print_map(void) {
    for (int b = 0; b < NB; b++) {
        char row[LPB + 1];
        for (int l = 0; l < LPB; l++) {
            int used = 0, starts = 0;
            for (int g = (b * LPB + l) * LG; g < (b * LPB + l + 1) * LG; g++)
                starts += oid[g] >= 0;
            used = line_live[b * LPB + l];
            row[l] = used ? (starts ? '#' : '+') : '.';
        }
        row[LPB] = 0;
        printf("  block %d: %s\n", b, row);
    }
}

int main(void) {
    model_reset();
    for (int g = 0; g < NG; g++)
        oid[g] = -1;
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    next_hole();
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, NULL};
    drive(&o, 1800, 250);
    int live = 0, gran = 0;
    for (int g = 0; g < NG; g++)
        if (oid[g] >= 0) {
            live++;
            gran += size_at(g);
        }
    printf("objects allocated: %d, collections: %d\n", M.nids, ncoll);
    printf("holes visited: %d, granules abandoned at hole tails: %d\n", holes, wasted);
    printf("live lines summed over collections: %d, fully free blocks summed: %d\n", lines_live_total, free_blocks_total);
    printf("live at end: %d objects, %d granules of %d\n", live, gran, NG);
    printf("line map after the final collection (# object start, + continuation, . free):\n");
    print_map();
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
