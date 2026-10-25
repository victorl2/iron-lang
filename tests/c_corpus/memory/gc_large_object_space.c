/*
 * title: Large object space with page runs and allocation-volume triggers
 * topic: memory
 * covers: size-based routing to a separate space, first-fit page run allocation over a bitmap, non-moving mark-sweep of large objects, byte-pattern integrity check against overlapping runs, fragmentation accounting, GC triggers by reason
 * deps: libc
 */
#define SEED 0x5EED0017ULL
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


#define CAPS 52
#define CAPL 40
#define LBASE 1000
#define NPAGES 96
#define PAGE 64
#define LOS_TRIGGER 66 /* pages allocated since the last collection */

typedef struct {
    int id;
    int f[M_F];
    unsigned char used, mark;
} Small;
typedef struct {
    int id;
    int f[M_F];
    int first, np;
    unsigned char used, mark;
} Large;

static Small sm[CAPS];
static Large lg[CAPL];
static unsigned char pages[NPAGES][PAGE];
static unsigned char page_used[NPAGES];
static int sfree, lfree;
static int roots[M_R + 1];
static int ncoll, by_small, by_volume, by_los_fail, los_allocs, frag_fail, pages_freed, peak_pages, since_gc;
static int worst_largest_run = NPAGES;

static int is_large(int r) { return r >= LBASE; }
static int large_pages(int idv) { return 2 + (idv * 7) % 9; }
static int wants_large(int idv) { return idv % 5 == 0; }
static unsigned char pat(int idv, int i) { return (unsigned char)(idv * 13 + i * 7); }

static int *field_ptr(int r, int k) { return is_large(r) ? &lg[r - LBASE].f[k] : &sm[r].f[k]; }

static int find_run(int np) {
    int run = 0;
    for (int p = 0; p < NPAGES; p++) {
        run = page_used[p] ? 0 : run + 1;
        if (run == np)
            return p - np + 1;
    }
    return -1;
}
static int largest_run(int *total_free) {
    int best = 0, run = 0, tot = 0;
    for (int p = 0; p < NPAGES; p++) {
        if (page_used[p])
            run = 0;
        else {
            run++;
            tot++;
        }
        if (run > best)
            best = run;
    }
    *total_free = tot;
    return best;
}

static void mark(int r) {
    if (r < 0)
        return;
    unsigned char *m = is_large(r) ? &lg[r - LBASE].mark : &sm[r].mark;
    if (*m)
        return;
    *m = 1;
    for (int k = 0; k < M_F; k++)
        mark(*field_ptr(r, k));
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    unsigned char owner_seen[NPAGES];
    memset(owner_seen, 0, sizeof owner_seen);
    for (int i = 0; i < CAPS; i++)
        if (sm[i].used)
            present[sm[i].id] = 1;
    for (int i = 0; i < CAPL; i++)
        if (lg[i].used) {
            present[lg[i].id] = 1;
            for (int p = lg[i].first; p < lg[i].first + lg[i].np; p++) {
                CHECK(page_used[p] && !owner_seen[p]);
                owner_seen[p] = 1;
                for (int b = 0; b < PAGE; b++)
                    CHECK(pages[p][b] == pat(lg[i].id, (p - lg[i].first) * PAGE + b));
            }
        }
    for (int p = 0; p < NPAGES; p++)
        CHECK(page_used[p] == owner_seen[p]);
    model_verify(present, 1);
}

static void gc(void) {
    for (int i = 0; i <= M_R; i++)
        mark(roots[i]);
    sfree = -1;
    for (int i = CAPS - 1; i >= 0; i--) {
        if (sm[i].used && !sm[i].mark)
            sm[i].used = 0;
        sm[i].mark = 0;
        if (!sm[i].used) {
            sm[i].f[0] = sfree;
            sfree = i;
        }
    }
    lfree = -1;
    for (int i = CAPL - 1; i >= 0; i--) {
        if (lg[i].used && !lg[i].mark) {
            lg[i].used = 0;
            for (int p = lg[i].first; p < lg[i].first + lg[i].np; p++)
                page_used[p] = 0;
            pages_freed += lg[i].np;
        }
        lg[i].mark = 0;
        if (!lg[i].used) {
            lg[i].f[0] = lfree;
            lfree = i;
        }
    }
    int total_free;
    int lr = largest_run(&total_free);
    if (lr < worst_largest_run)
        worst_largest_run = lr;
    since_gc = 0;
    ncoll++;
    verify();
}

static void alloc_root(int r, int idv) {
    if (!wants_large(idv)) {
        if (sfree < 0) {
            by_small++;
            gc();
        }
        CHECK(sfree >= 0);
        int s = sfree;
        sfree = sm[s].f[0];
        sm[s].used = 1;
        sm[s].id = idv;
        for (int k = 0; k < M_F; k++)
            sm[s].f[k] = -1;
        roots[r] = s;
        return;
    }
    int np = large_pages(idv);
    if (since_gc + np > LOS_TRIGGER) {
        by_volume++;
        gc();
    }
    int first = lfree >= 0 ? find_run(np) : -1;
    if (first < 0) {
        int tf;
        by_los_fail++;
        gc();
        first = lfree >= 0 ? find_run(np) : -1;
        if (first < 0 && largest_run(&tf) < np && tf >= np)
            frag_fail++;
    }
    CHECK(first >= 0);
    int d = lfree;
    lfree = lg[d].f[0];
    lg[d].used = 1;
    lg[d].id = idv;
    lg[d].first = first;
    lg[d].np = np;
    for (int k = 0; k < M_F; k++)
        lg[d].f[k] = -1;
    for (int p = first; p < first + np; p++) {
        page_used[p] = 1;
        for (int b = 0; b < PAGE; b++)
            pages[p][b] = pat(idv, (p - first) * PAGE + b);
    }
    since_gc += np;
    los_allocs++;
    int used_pages = 0;
    for (int p = 0; p < NPAGES; p++)
        used_pages += page_used[p];
    if (used_pages > peak_pages)
        peak_pages = used_pages;
    roots[r] = LBASE + d;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return *field_ptr(ref, k); }
static void set_f(int ref, int k, int t) { *field_ptr(ref, k) = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return is_large(ref) ? lg[ref - LBASE].id : sm[ref].id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    sfree = lfree = -1;
    for (int i = CAPS - 1; i >= 0; i--) {
        sm[i].f[0] = sfree;
        sfree = i;
    }
    for (int i = CAPL - 1; i >= 0; i--) {
        lg[i].f[0] = lfree;
        lfree = i;
    }
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, NULL};
    drive(&o, 2000, 400);
    int lpages = 0;
    for (int p = 0; p < NPAGES; p++)
        lpages += page_used[p];
    printf("objects allocated: %d, large: %d\n", M.nids, los_allocs);
    printf("collections: %d (small space full: %d, page volume: %d, no page run: %d)\n", ncoll, by_small, by_volume,
           by_los_fail);
    printf("pages freed: %d, peak pages in use: %d of %d, in use at end: %d\n", pages_freed, peak_pages, NPAGES, lpages);
    printf("smallest largest-free-run after a collection: %d pages\n", worst_largest_run);
    printf("failures caused by fragmentation alone: %d\n", frag_fail);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
