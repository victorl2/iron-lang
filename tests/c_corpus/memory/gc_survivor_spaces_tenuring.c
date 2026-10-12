/*
 * title: Eden and survivor spaces with adaptive tenuring threshold
 * topic: memory
 * covers: eden plus two survivor semispaces, per-object age, promotion by age or survivor overflow, adaptive tenuring threshold, old-to-young remembered set maintained through promotion, age histogram of survivors, minor-collection oracle (nothing reachable lost)
 * deps: libc
 */
#define SEED 0x5EED0021ULL
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


#define ED 24
#define SV 8
#define OLDN 1400
#define S0 ED
#define S1 (ED + SV)
#define OLD0 (ED + 2 * SV)
#define TOTAL (OLD0 + OLDN)
#define MAXAGE 7

typedef struct {
    int id;
    int f[M_F];
    int age;
    int fwd;
    unsigned char inrem;
} Obj;

static Obj h[TOTAL];
static int eden_top, from_sp, sv_top, old_top;
static int rem[OLDN], nrem;
static int roots[M_R + 1];
static int thr = 3, thr_up, thr_down, minors, promoted_age, promoted_overflow, copied_sv, floating_total;
static int age_hist[MAXAGE + 1];
static int max_rem;

static int is_young(int r) { return r >= 0 && r < OLD0; }
static int in_eden(int r) { return r >= 0 && r < ED; }
static int sv_base(int s) { return s ? S1 : S0; }

static int evacuate(int r, int *to_sv_top, int to_sp) {
    if (r < 0 || !is_young(r))
        return r;
    int from_sv = (r >= sv_base(from_sp) && r < sv_base(from_sp) + SV);
    if (!in_eden(r) && !from_sv)
        return r; /* already in to-space */
    if (h[r].fwd >= 0)
        return h[r].fwd;
    int dst;
    int age = h[r].age + 1;
    if (age >= thr || *to_sv_top == SV) {
        CHECK(old_top < OLD0 + OLDN);
        dst = old_top++;
        if (age < thr)
            promoted_overflow++;
        else
            promoted_age++;
    } else {
        dst = sv_base(to_sp) + (*to_sv_top)++;
        copied_sv++;
        if (age <= MAXAGE)
            age_hist[age]++;
    }
    h[dst] = h[r];
    h[dst].age = age;
    h[dst].inrem = 0;
    h[dst].fwd = -1;
    h[r].fwd = dst;
    return dst;
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < eden_top; i++)
        present[h[i].id] = 1;
    for (int i = 0; i < sv_top; i++)
        present[h[sv_base(from_sp) + i].id] = 1;
    for (int i = OLD0; i < old_top; i++)
        present[h[i].id] = 1;
    int cnt = 0;
    for (int id = 0; id < M.nids; id++)
        cnt += present[id];
    int n = model_verify(present, 0);
    floating_total += cnt - n;
}

static int has_young_field(int o) {
    for (int k = 0; k < M_F; k++)
        if (is_young(h[o].f[k]))
            return 1;
    return 0;
}

static void minor(void) {
    int to_sp = 1 - from_sp, to_top = 0;
    int old_scan = old_top;
    for (int i = 0; i <= M_R; i++)
        roots[i] = evacuate(roots[i], &to_top, to_sp);
    for (int i = 0; i < nrem; i++) {
        int o = rem[i];
        h[o].inrem = 0;
        for (int k = 0; k < M_F; k++)
            h[o].f[k] = evacuate(h[o].f[k], &to_top, to_sp);
    }
    int sv_scan = 0;
    while (sv_scan < to_top || old_scan < old_top) {
        while (sv_scan < to_top) {
            int o = sv_base(to_sp) + sv_scan++;
            for (int k = 0; k < M_F; k++)
                h[o].f[k] = evacuate(h[o].f[k], &to_top, to_sp);
        }
        while (old_scan < old_top) {
            int o = old_scan++;
            for (int k = 0; k < M_F; k++)
                h[o].f[k] = evacuate(h[o].f[k], &to_top, to_sp);
        }
    }
    /* rebuild the remembered set: old objects that still point at young ones */
    int w = 0;
    for (int i = 0; i < nrem; i++) {
        int o = rem[i];
        if (has_young_field(o)) {
            h[o].inrem = 1;
            rem[w++] = o;
        }
    }
    for (int o = OLD0; o < old_top; o++)
        if (!h[o].inrem && has_young_field(o) && w < OLDN) {
            h[o].inrem = 1;
            rem[w++] = o;
        }
    nrem = w;
    if (nrem > max_rem)
        max_rem = nrem;
    for (int i = 0; i < ED; i++)
        h[i].fwd = -1;
    for (int i = 0; i < SV; i++)
        h[sv_base(from_sp) + i].fwd = -1;
    from_sp = to_sp;
    sv_top = to_top;
    eden_top = 0;
    minors++;
    /* adapt: a crowded survivor space means objects should be tenured sooner, an empty one later */
    if (sv_top * 4 > SV * 3 && thr > 1) {
        thr--;
        thr_down++;
    } else if (sv_top * 2 < SV && thr < MAXAGE) {
        thr++;
        thr_up++;
    }
    verify();
}

static void alloc_root(int r, int idv) {
    if (eden_top == ED)
        minor();
    int s = eden_top++;
    h[s].id = idv;
    h[s].age = 0;
    h[s].fwd = -1;
    h[s].inrem = 0;
    for (int k = 0; k < M_F; k++)
        h[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return h[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    h[ref].f[k] = t;
    if (ref >= OLD0 && t >= 0 && is_young(t) && !h[ref].inrem) {
        CHECK(nrem < OLDN);
        h[ref].inrem = 1;
        rem[nrem++] = ref;
    }
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return h[ref].id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    old_top = OLD0;
    for (int i = 0; i < TOTAL; i++)
        h[i].fwd = -1;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, minor, NULL};
    drive(&o, 2600, 500);
    printf("objects allocated: %d, minor collections: %d\n", M.nids, minors);
    printf("copied within survivor spaces: %d\n", copied_sv);
    printf("promoted by age: %d, promoted by survivor overflow: %d\n", promoted_age, promoted_overflow);
    printf("tenuring threshold now %d (raised %d times, lowered %d times)\n", thr, thr_up, thr_down);
    printf("survivors by age reached:");
    for (int a = 1; a <= MAXAGE; a++)
        printf(" %d:%d", a, age_hist[a]);
    printf("\n");
    printf("old space used: %d, peak remembered set: %d, unreachable objects kept, summed over collections: %d\n", old_top - OLD0, max_rem,
           floating_total);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
