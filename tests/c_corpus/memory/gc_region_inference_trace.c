/*
 * title: Region inference over a call trace with dangling-pointer checking
 * topic: memory
 * covers: allocation sites assigned to frame regions, outlives constraints from stores and returns, fixed-point lifting to outer regions, region stack simulation, dangling pointer detection for the naive assignment, region waste versus reachability
 * deps: libc
 */
#define SEED 0x5EED0013ULL
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



#define MAXOBJ 400
#define MAXEV 4000
#define MAXDEPTH 6
#define NF 2
enum { E_ENTER, E_EXIT, E_ALLOC, E_STORE, E_VAR };
typedef struct {
    int kind, a, b, c;
} Ev;

static Ev ev[MAXEV];
static int nev, nobj;
static int alloc_depth[MAXOBJ];

static void emit(int kind, int a, int b, int c) {
    CHECK(nev < MAXEV);
    ev[nev].kind = kind;
    ev[nev].a = a;
    ev[nev].b = b;
    ev[nev].c = c;
    nev++;
}

/* Generate one activation: allocate, store, call children, return one local. */
static int gen_frame(int depth, const int *params, int np) {
    int loc[64], nl = 0;
    for (int i = 0; i < np; i++)
        loc[nl++] = params[i]; /* the caller already emitted VAR for these */
    int nalloc = 2 + (int)rnd_n(3);
    for (int i = 0; i < nalloc && nobj < MAXOBJ - 40; i++) {
        int id = nobj++;
        alloc_depth[id] = depth;
        emit(E_ALLOC, id, 0, 0);
        emit(E_VAR, id, 0, 0);
        loc[nl++] = id;
    }
    int rounds = 2;
    for (int r = 0; r < rounds; r++) {
        int nst = 1 + (int)rnd_n(3);
        for (int i = 0; i < nst && nl > 1; i++) {
            int x = loc[rnd_n((unsigned)nl)], y = loc[rnd_n((unsigned)nl)], k = (int)rnd_n(NF);
            if (x != y)
                emit(E_STORE, x, k, y);
        }
        int ncalls = depth < MAXDEPTH ? (int)rnd_n(3) : 0;
        for (int i = 0; i < ncalls && nl > 0; i++) {
            int p[2], n = 1 + (int)rnd_n(2);
            for (int j = 0; j < n; j++)
                p[j] = loc[rnd_n((unsigned)nl)];
            emit(E_ENTER, depth + 1, 0, 0);
            for (int j = 0; j < n; j++)
                emit(E_VAR, p[j], 0, 0);
            int ret = gen_frame(depth + 1, p, n);
            emit(E_EXIT, depth + 1, ret, 0);
            if (ret >= 0) {
                emit(E_VAR, ret, 0, 0);
                loc[nl++] = ret;
            }
        }
    }
    if (depth == 0 || nl == 0 || rnd_n(10) < 3)
        return -1;
    return loc[rnd_n((unsigned)nl)];
}

static int region[MAXOBJ];

/* Solve: r(y) <= r(x) for every store x.f = y, r(ret) <= depth-1 at every return. */
static int infer(int *rounds_out) {
    for (int i = 0; i < nobj; i++)
        region[i] = alloc_depth[i];
    int rounds = 0, lifted = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        rounds++;
        for (int e = 0; e < nev; e++) {
            if (ev[e].kind == E_STORE && region[ev[e].c] > region[ev[e].a]) {
                region[ev[e].c] = region[ev[e].a];
                changed = 1;
            } else if (ev[e].kind == E_EXIT && ev[e].b >= 0 && region[ev[e].b] > ev[e].a - 1) {
                region[ev[e].b] = ev[e].a - 1;
                changed = 1;
            }
        }
    }
    for (int i = 0; i < nobj; i++)
        lifted += region[i] != alloc_depth[i];
    *rounds_out = rounds;
    return lifted;
}

typedef struct {
    int violations, peak_held, peak_live, freed_at_exit, dead_but_held_sum;
} Result;

/* Replays the trace with objects allocated in the region of frame region[o]. */
static Result simulate(const int *reg) {
    Result res = {0, 0, 0, 0, 0};
    static int field[MAXOBJ][NF];
    static unsigned char freed[MAXOBJ], allocd[MAXOBJ];
    static int inst[MAXOBJ];
    static int vars[MAXDEPTH + 2][80], nvars[MAXDEPTH + 2];
    int frame_inst[MAXDEPTH + 2], next_inst = 1, depth = 0;
    memset(freed, 0, sizeof freed);
    memset(allocd, 0, sizeof allocd);
    for (int i = 0; i < MAXOBJ; i++)
        field[i][0] = field[i][1] = -1;
    memset(nvars, 0, sizeof nvars);
    frame_inst[0] = next_inst++;
    for (int e = 0; e < nev; e++) {
        Ev *v = &ev[e];
        int held = 0;
        switch (v->kind) {
        case E_ALLOC:
            allocd[v->a] = 1;
            inst[v->a] = frame_inst[reg[v->a]];
            break;
        case E_STORE:
            field[v->a][v->b] = v->c;
            break;
        case E_VAR:
            vars[depth][nvars[depth]++] = v->a;
            break;
        case E_ENTER:
            depth++;
            frame_inst[depth] = next_inst++;
            break;
        case E_EXIT:
            nvars[depth] = 0;
            for (int o = 0; o < nobj; o++)
                if (allocd[o] && !freed[o] && inst[o] == frame_inst[depth]) {
                    freed[o] = 1;
                    res.freed_at_exit++;
                }
            depth--;
            break;
        }
        if (v->kind == E_EXIT || v->kind == E_VAR) {
            /* everything reachable from live variables must still be allocated */
            static unsigned char seen[MAXOBJ];
            int stack[MAXOBJ * 2], sp = 0, live = 0;
            memset(seen, 0, sizeof seen);
            for (int d = 0; d <= depth; d++)
                for (int i = 0; i < nvars[d]; i++)
                    if (!seen[vars[d][i]]) {
                        seen[vars[d][i]] = 1;
                        stack[sp++] = vars[d][i];
                    }
            while (sp > 0) {
                int o = stack[--sp];
                live++;
                if (freed[o])
                    res.violations++;
                for (int k = 0; k < NF; k++) {
                    int t = field[o][k];
                    if (t >= 0 && !seen[t]) {
                        seen[t] = 1;
                        stack[sp++] = t;
                    }
                }
            }
            for (int o = 0; o < nobj; o++)
                held += allocd[o] && !freed[o];
            if (held > res.peak_held)
                res.peak_held = held;
            if (live > res.peak_live)
                res.peak_live = live;
            res.dead_but_held_sum += held - live;
        }
    }
    return res;
}

int main(void) {
    nobj = 0;
    gen_frame(0, NULL, 0);
    int rounds;
    int lifted = infer(&rounds);
    Result naive_res;
    int naive[MAXOBJ];
    for (int i = 0; i < nobj; i++)
        naive[i] = alloc_depth[i];
    naive_res = simulate(naive);
    Result inferred = simulate(region);
    CHECK(inferred.violations == 0);
    CHECK(naive_res.violations > 0);
    int hist_alloc[MAXDEPTH + 2] = {0}, hist_reg[MAXDEPTH + 2] = {0};
    for (int i = 0; i < nobj; i++) {
        hist_alloc[alloc_depth[i]]++;
        hist_reg[region[i]]++;
        CHECK(region[i] <= alloc_depth[i]);
    }
    printf("trace events: %d, allocation sites: %d\n", nev, nobj);
    printf("constraint passes to fixed point: %d, objects lifted: %d\n", rounds, lifted);
    printf("depth  allocated-in-frame  assigned-region\n");
    for (int d = 0; d <= MAXDEPTH; d++)
        printf("%5d  %18d  %15d\n", d, hist_alloc[d], hist_reg[d]);
    printf("naive frame-local regions: %d dangling reachable-object observations\n", naive_res.violations);
    printf("inferred regions: %d dangling reachable-object observations\n", inferred.violations);
    printf("peak allocated: %d, peak reachable: %d\n", inferred.peak_held, inferred.peak_live);
    printf("region waste summed over checkpoints: %d\n", inferred.dead_but_held_sum);
    return 0;
}
