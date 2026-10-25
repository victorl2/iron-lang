/*
 * title: Garbage-first region evacuation with per-region remembered sets
 * topic: memory
 * covers: fixed-size regions, liveness marking per region, collection set chosen by garbage with an evacuation budget, per-region remembered sets fed by a cross-region write barrier, coarsening on overflow, copying live objects to free regions, remembered set completeness check
 * deps: libc
 */
#define SEED 0x5EED0027ULL
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


#define NREG 16
#define RS 10
#define BUDGET 9
#define REMCAP 6
#define TOTAL (NREG * RS)

typedef struct {
    int id;
    int f[M_F];
    int fwd;
    unsigned char used, mark;
} Obj;

typedef struct {
    int fill;
    unsigned char in_use, in_cset;
    int live;
    int rem[REMCAP], nrem;
    unsigned char coarse;
} Region;

static Obj heap[TOTAL];
static Region reg[NREG];
static int alloc_reg = -1;
static int roots[M_R + 1];
static int pauses, evacuated, regions_freed, coarsenings, rem_adds, cset_total, budget_limited, skipped_no_free;
static int regions_in_use_max;

static int region_of(int r) { return r / RS; }

static void add_rem(int src, int target) {
    if (target < 0 || region_of(src) == region_of(target))
        return;
    Region *t = &reg[region_of(target)];
    if (t->coarse)
        return;
    for (int i = 0; i < t->nrem; i++)
        if (t->rem[i] == src)
            return;
    rem_adds++;
    if (t->nrem == REMCAP) {
        t->coarse = 1; /* too many sources: remember only that a full scan is needed */
        t->nrem = 0;
        coarsenings++;
        return;
    }
    t->rem[t->nrem++] = src;
}

static int take_free_region(void) {
    for (int r = 0; r < NREG; r++)
        if (!reg[r].in_use) {
            reg[r].in_use = 1;
            reg[r].fill = 0;
            reg[r].nrem = 0;
            reg[r].coarse = 0;
            reg[r].in_cset = 0;
            int n = 0;
            for (int i = 0; i < NREG; i++)
                n += reg[i].in_use;
            if (n > regions_in_use_max)
                regions_in_use_max = n;
            return r;
        }
    return -1;
}

static void mark(int o) {
    if (o < 0 || heap[o].mark)
        return;
    heap[o].mark = 1;
    for (int k = 0; k < M_F; k++)
        mark(heap[o].f[k]);
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < TOTAL; i++)
        if (heap[i].used)
            present[heap[i].id] = 1;
    model_verify(present, 0);
}

static int upd(int t) { /* new location of a reference into the collection set; dead targets become null */
    if (t < 0 || !reg[region_of(t)].in_cset)
        return t;
    return heap[t].mark ? heap[t].fwd : -1;
}

static void fix_field(int s, int k) {
    int t = upd(heap[s].f[k]);
    heap[s].f[k] = t;
    add_rem(s, t);
}

static void evac_pause(void) {
    for (int i = 0; i <= M_R; i++)
        mark(roots[i]);
    for (int r = 0; r < NREG; r++) {
        reg[r].live = 0;
        reg[r].in_cset = 0;
    }
    for (int i = 0; i < TOTAL; i++)
        if (heap[i].used && heap[i].mark)
            reg[region_of(i)].live++;
    /* choose the collection set: most garbage first, within the evacuation budget */
    int chosen[NREG], nc = 0, live_sum = 0, free_regions = 0;
    for (int r = 0; r < NREG; r++)
        free_regions += !reg[r].in_use;
    int cand[NREG], ncand = 0;
    for (int r = 0; r < NREG; r++)
        if (reg[r].in_use && reg[r].fill > reg[r].live)
            cand[ncand++] = r;
    for (int i = 1; i < ncand; i++) { /* insertion sort by garbage, ties by region number */
        int x = cand[i], j = i - 1;
        int gx = reg[x].fill - reg[x].live;
        while (j >= 0 && (reg[cand[j]].fill - reg[cand[j]].live < gx)) {
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = x;
    }
    for (int i = 0; i < ncand; i++) {
        int r = cand[i];
        if (live_sum + reg[r].live > BUDGET) {
            budget_limited++;
            continue;
        }
        int need = (live_sum + reg[r].live + RS - 1) / RS;
        if (need > free_regions) {
            skipped_no_free++;
            continue;
        }
        chosen[nc++] = r;
        live_sum += reg[r].live;
        reg[r].in_cset = 1;
    }
    cset_total += nc;
    /* evacuate marked objects of the collection set into fresh regions */
    int to_regs[NREG], nto = 0, to_reg = -1;
    for (int c = 0; c < nc; c++) {
        int r = chosen[c];
        for (int i = r * RS; i < r * RS + reg[r].fill; i++) {
            if (!heap[i].used || !heap[i].mark)
                continue;
            if (to_reg < 0 || reg[to_reg].fill == RS) {
                to_reg = take_free_region();
                CHECK(to_reg >= 0);
                to_regs[nto++] = to_reg;
            }
            int dst = to_reg * RS + reg[to_reg].fill++;
            heap[dst] = heap[i];
            heap[i].fwd = dst;
            evacuated++;
        }
    }
    /* update references: roots, remembered-set sources of each cset region, and the copies */
    for (int i = 0; i <= M_R; i++)
        roots[i] = upd(roots[i]);
    for (int c = 0; c < nc; c++) {
        Region *t = &reg[chosen[c]];
        if (t->coarse) {
            for (int s = 0; s < TOTAL; s++)
                if (heap[s].used && !reg[region_of(s)].in_cset)
                    for (int k = 0; k < M_F; k++)
                        fix_field(s, k);
        } else {
            for (int e = 0; e < t->nrem; e++) {
                int s = t->rem[e];
                if (!heap[s].used || reg[region_of(s)].in_cset)
                    continue;
                for (int k = 0; k < M_F; k++)
                    fix_field(s, k);
            }
        }
    }
    for (int q = 0; q < nto; q++)
        for (int i = to_regs[q] * RS; i < to_regs[q] * RS + reg[to_regs[q]].fill; i++)
            for (int k = 0; k < M_F; k++)
                fix_field(i, k);
    /* purge remembered set entries whose source lived in a region that is about to be freed */
    for (int r = 0; r < NREG; r++) {
        if (!reg[r].in_use || reg[r].in_cset)
            continue;
        int w = 0;
        for (int e = 0; e < reg[r].nrem; e++)
            if (!reg[region_of(reg[r].rem[e])].in_cset)
                reg[r].rem[w++] = reg[r].rem[e];
        reg[r].nrem = w;
    }
    for (int c = 0; c < nc; c++) {
        int r = chosen[c];
        for (int i = r * RS; i < (r + 1) * RS; i++) {
            heap[i].used = 0;
            heap[i].mark = 0;
        }
        reg[r].in_use = 0;
        reg[r].in_cset = 0;
        reg[r].fill = 0;
        reg[r].nrem = 0;
        reg[r].coarse = 0;
        regions_freed++;
        if (alloc_reg == r)
            alloc_reg = -1;
    }
    for (int i = 0; i < TOTAL; i++)
        heap[i].mark = 0;
    /* completeness: every cross-region pointer is covered by its target region's remembered set */
    for (int s = 0; s < TOTAL; s++) {
        if (!heap[s].used)
            continue;
        for (int k = 0; k < M_F; k++) {
            int t = heap[s].f[k];
            if (t < 0)
                continue;
            CHECK(heap[t].used);
            if (region_of(s) == region_of(t) || reg[region_of(t)].coarse)
                continue;
            int found = 0;
            for (int e = 0; e < reg[region_of(t)].nrem; e++)
                found |= reg[region_of(t)].rem[e] == s;
            CHECK(found);
        }
    }
    pauses++;
    verify();
}

static void alloc_root(int r, int idv) {
    if (alloc_reg < 0 || reg[alloc_reg].fill == RS) {
        alloc_reg = take_free_region();
        if (alloc_reg < 0) {
            evac_pause();
            alloc_reg = take_free_region();
        }
        CHECK(alloc_reg >= 0);
    }
    int s = alloc_reg * RS + reg[alloc_reg].fill++;
    heap[s].used = 1;
    heap[s].mark = 0;
    heap[s].id = idv;
    heap[s].fwd = -1;
    for (int k = 0; k < M_F; k++)
        heap[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return heap[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    heap[ref].f[k] = t;
    add_rem(ref, t);
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return heap[ref].id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, evac_pause, NULL};
    drive(&o, 1500, 300);
    int live = 0;
    for (int i = 0; i < TOTAL; i++)
        live += heap[i].used;
    printf("objects allocated: %d, pauses: %d\n", M.nids, pauses);
    printf("regions freed: %d, objects evacuated: %d, average cset size x100: %d\n", regions_freed, evacuated,
           pauses ? cset_total * 100 / pauses : 0);
    printf("remembered set adds: %d, coarsened to full scan: %d\n", rem_adds, coarsenings);
    printf("regions skipped by budget: %d, by lack of free regions: %d\n", budget_limited, skipped_no_free);
    printf("peak regions in use: %d of %d, objects at end: %d\n", regions_in_use_max, NREG, live);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
