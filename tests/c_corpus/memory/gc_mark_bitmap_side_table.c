/*
 * title: Mark-sweep with side bitmaps and word-parallel sweeping
 * topic: memory
 * covers: object-start, occupancy and mark bitmaps over 64-bit words, sweeping whole words with and-not, popcount and lowest-set-bit without builtins, free extent computation by word scan versus a naive per-bit loop, first-fit run search that skips full words
 * deps: libc
 */
#define SEED 0x5EED001EULL
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


#define NG 1536 /* granules */
#define NW (NG / 64)

typedef uint64_t W;
static W starts[NW], occ[NW], markb[NW];
static int oid[NG];
static int of[NG][M_F];
static int roots[M_R + 1];
static int ncoll, swept_objs, skipped_words, visited_words, alloc_scans, alloc_words_skipped;
static int largest_run_seen, extents_seen;

static int size_of(int idv) { return 1 + idv % 5; }

static int popcount(W x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
static int lowest_bit(W x) { /* index of the lowest set bit, x != 0 */
    return popcount((x & (~x + 1)) - 1);
}
static int test(const W *b, int i) { return (int)((b[i / 64] >> (i % 64)) & 1); }
static void setb(W *b, int i) { b[i / 64] |= (W)1 << (i % 64); }
static void set_range(W *b, int i, int n) {
    for (int k = 0; k < n; k++)
        setb(b, i + k);
}
static void clear_range(W *b, int i, int n) {
    while (n > 0) {
        int off = i % 64, take = 64 - off < n ? 64 - off : n;
        W mask = (take == 64 ? ~(W)0 : (((W)1 << take) - 1)) << off;
        b[i / 64] &= ~mask;
        i += take;
        n -= take;
    }
}

static void mark(int g) {
    static int stack[NG];
    int sp = 0;
    if (g < 0 || test(markb, g))
        return;
    setb(markb, g);
    stack[sp++] = g;
    while (sp > 0) {
        int o = stack[--sp];
        for (int k = 0; k < M_F; k++) {
            int c = of[o][k];
            if (c >= 0 && !test(markb, c)) {
                setb(markb, c);
                stack[sp++] = c;
            }
        }
    }
}

/* Free space statistics two ways: by word, and by looking at every bit. */
static void free_extents(int *count, int *largest, int *total) {
    int c = 0, best = 0, tot = 0, run = 0;
    for (int w = 0; w < NW; w++) {
        W f = ~occ[w];
        if (f == 0) { /* full word: closes any open run */
            run = 0;
            continue;
        }
        if (f == ~(W)0) { /* empty word: extends the run by 64 */
            if (run == 0)
                c++;
            run += 64;
            tot += 64;
            if (run > best)
                best = run;
            continue;
        }
        for (int b = 0; b < 64; b++) {
            if ((f >> b) & 1) {
                if (run == 0)
                    c++;
                run++;
                tot++;
                if (run > best)
                    best = run;
            } else {
                run = 0;
            }
        }
    }
    *count = c;
    *largest = best;
    *total = tot;
}
static void free_extents_naive(int *count, int *largest, int *total) {
    int c = 0, best = 0, tot = 0, run = 0;
    for (int g = 0; g < NG; g++) {
        if (!test(occ, g)) {
            if (run == 0)
                c++;
            run++;
            tot++;
            if (run > best)
                best = run;
        } else {
            run = 0;
        }
    }
    *count = c;
    *largest = best;
    *total = tot;
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    int occupied = 0;
    for (int w = 0; w < NW; w++) {
        W s = starts[w];
        while (s) {
            int g = w * 64 + lowest_bit(s);
            s &= s - 1;
            present[oid[g]] = 1;
            for (int j = 0; j < size_of(oid[g]); j++)
                CHECK(test(occ, g + j));
        }
        occupied += popcount(occ[w]);
    }
    int total = 0;
    for (int g = 0; g < NG; g++)
        total += test(occ, g);
    CHECK(total == occupied);
    int c1, l1, t1, c2, l2, t2;
    free_extents(&c1, &l1, &t1);
    free_extents_naive(&c2, &l2, &t2);
    CHECK(c1 == c2 && l1 == l2 && t1 == t2 && t1 == NG - occupied);
    extents_seen += c1;
    if (l1 > largest_run_seen)
        largest_run_seen = l1;
    model_verify(present, 1);
}

static void gc(void) {
    memset(markb, 0, sizeof markb);
    for (int i = 0; i <= M_R; i++)
        mark(roots[i]);
    for (int w = 0; w < NW; w++) {
        visited_words++;
        W dead = starts[w] & ~markb[w];
        if (dead == 0) {
            skipped_words++;
            continue;
        }
        while (dead) {
            int g = w * 64 + lowest_bit(dead);
            dead &= dead - 1;
            clear_range(occ, g, size_of(oid[g]));
            swept_objs++;
        }
        starts[w] &= markb[w];
    }
    ncoll++;
    verify();
}

static int find_run(int n) {
    int run = 0;
    for (int w = 0; w < NW; w++) {
        if (occ[w] == ~(W)0) {
            run = 0;
            alloc_words_skipped++;
            continue;
        }
        for (int b = 0; b < 64; b++) {
            run = ((occ[w] >> b) & 1) ? 0 : run + 1;
            if (run == n)
                return w * 64 + b - n + 1;
        }
    }
    return -1;
}

static void alloc_root(int r, int idv) {
    int n = size_of(idv);
    alloc_scans++;
    int g = find_run(n);
    if (g < 0) {
        gc();
        g = find_run(n);
    }
    CHECK(g >= 0);
    set_range(occ, g, n);
    setb(starts, g);
    oid[g] = idv;
    for (int k = 0; k < M_F; k++)
        of[g][k] = -1;
    roots[r] = g;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return of[ref][k]; }
static void set_f(int ref, int k, int t) { of[ref][k] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return oid[ref]; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    CHECK(popcount(0) == 0 && popcount(~(W)0) == 64 && lowest_bit((W)1 << 37) == 37 && lowest_bit(6) == 1);
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, NULL};
    drive(&o, 2500, 500);
    int c, l, t;
    free_extents(&c, &l, &t);
    printf("objects allocated: %d, collections: %d, swept: %d\n", M.nids, ncoll, swept_objs);
    printf("sweep visited %d bitmap words, skipped %d with no garbage\n", visited_words, skipped_words);
    printf("allocator skipped %d full words over %d searches\n", alloc_words_skipped, alloc_scans);
    printf("free extents at end: %d, largest %d, total free %d of %d granules\n", c, l, t, NG);
    printf("extents summed over collections: %d, largest run seen: %d\n", extents_seen, largest_run_seen);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
