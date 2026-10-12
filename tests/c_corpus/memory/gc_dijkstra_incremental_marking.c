/*
 * title: Incremental tri-color marking with a Dijkstra insertion barrier
 * topic: memory
 * covers: white grey black colors, grey work stack, incremental mark slices between mutator steps, insertion write barrier, strong tri-color invariant check, root rescan at termination, barrier ablation scenario, floating garbage
 * deps: libc
 */
#define SEED 0x5EED000BULL
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


#define CAP 130
#define TRIGGER 45
#define SLICE 3
enum { WHITE, GREY, BLACK };
enum { IDLE, MARKING };

typedef struct {
    int id;
    int f[M_F];
    unsigned char used, color;
} Obj;

static Obj heap[CAP];
static int freelist, nfree;
static int gstack[CAP + 8], gsp;
static int roots[M_R + 1];
static int state, interleaved, barrier_on = 1;
static int ncycles, barrier_shades, sync_finishes, swept, floating_total, slices, max_grey;

static void shade(int r) {
    if (r >= 0 && heap[r].color == WHITE) {
        heap[r].color = GREY;
        gstack[gsp++] = r;
        if (gsp > max_grey)
            max_grey = gsp;
    }
}

static void mark_step(void) {
    int g = gstack[--gsp];
    for (int k = 0; k < M_F; k++)
        shade(heap[g].f[k]);
    heap[g].color = BLACK;
}

static int black_to_white_edges(void) {
    int n = 0;
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && heap[i].color == BLACK)
            for (int k = 0; k < M_F; k++)
                if (heap[i].f[k] >= 0 && heap[heap[i].f[k]].color == WHITE)
                    n++;
    return n;
}

static int verify(int exact) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    int cnt = 0;
    for (int i = 0; i < CAP; i++)
        if (heap[i].used) {
            present[heap[i].id] = 1;
            cnt++;
        }
    if (!barrier_on) { /* ablation: report instead of aborting */
        unsigned char reach[M_IDS];
        model_reach(reach);
        int lost = 0;
        for (int id = 0; id < M.nids; id++)
            if (reach[id] && !present[id])
                lost++;
        return lost;
    }
    int n = model_verify(present, exact);
    if (!exact)
        floating_total += cnt - n;
    return 0;
}

static void start_cycle(int concurrent) {
    state = MARKING;
    interleaved = concurrent;
    gsp = 0;
    for (int i = 0; i <= M_R; i++)
        shade(roots[i]);
}

static int finish_cycle(void) {
    for (;;) { /* roots are not covered by the barrier: rescan until stable */
        while (gsp > 0)
            mark_step();
        for (int i = 0; i <= M_R; i++)
            shade(roots[i]);
        if (gsp == 0)
            break;
    }
    freelist = -1;
    nfree = 0;
    for (int i = CAP - 1; i >= 0; i--) {
        if (heap[i].used && heap[i].color == WHITE) {
            heap[i].used = 0;
            swept++;
        }
        heap[i].color = WHITE;
        if (!heap[i].used) {
            heap[i].f[0] = freelist;
            freelist = i;
            nfree++;
        }
    }
    state = IDLE;
    ncycles++;
    return verify(!interleaved);
}

static void tick(void) {
    if (state == IDLE) {
        if (nfree < TRIGGER)
            start_cycle(1);
        return;
    }
    interleaved = 1;
    slices++;
    for (int i = 0; i < SLICE && gsp > 0; i++)
        mark_step();
    if (barrier_on)
        CHECK(black_to_white_edges() == 0);
    if (gsp == 0)
        finish_cycle();
}

static void collect(void) {
    if (state == IDLE)
        start_cycle(0);
    finish_cycle();
}

static void alloc_root(int r, int id) {
    if (freelist < 0) {
        if (state == IDLE)
            start_cycle(0);
        finish_cycle();
        sync_finishes++;
    }
    CHECK(freelist >= 0);
    int s = freelist;
    freelist = heap[s].f[0];
    nfree--;
    heap[s].used = 1;
    heap[s].color = WHITE;
    heap[s].id = id;
    for (int k = 0; k < M_F; k++)
        heap[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return heap[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    if (barrier_on && state == MARKING && heap[ref].color == BLACK && t >= 0 && heap[t].color == WHITE) {
        shade(t); /* Dijkstra: shade the target of every new pointer stored into a black object */
        barrier_shades++;
    }
    heap[ref].f[k] = t;
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return heap[ref].id; }

static void heap_reset(void) {
    freelist = -1;
    nfree = 0;
    for (int i = CAP - 1; i >= 0; i--) {
        memset(&heap[i], 0, sizeof heap[i]);
        heap[i].f[0] = freelist;
        freelist = i;
        nfree++;
    }
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    state = IDLE;
    gsp = 0;
    model_reset();
}

/* A black object receives a pointer to a fresh white object that only it references. */
static int scenario(int with_barrier) {
    heap_reset();
    barrier_on = with_barrier;
    int a = model_new();
    alloc_root(0, a);
    M.root[0] = a;
    start_cycle(1);
    mark_step(); /* a turns black */
    int b = model_new();
    alloc_root(1, b);
    set_f(roots[0], 0, roots[1]);
    M.edge[a][0] = b;
    roots[1] = -1;
    int lost = finish_cycle();
    barrier_on = 1;
    return lost;
}

int main(void) {
    int lost_on = scenario(1);
    int lost_off = scenario(0);
    CHECK(lost_on == 0 && lost_off == 1);
    heap_reset();
    swept = ncycles = barrier_shades = 0;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, collect, tick};
    drive(&o, 2200, 500);
    int live = 0;
    for (int i = 0; i < CAP; i++)
        live += heap[i].used;
    printf("barrier ablation: objects lost with barrier %d, without %d\n", lost_on, lost_off);
    printf("objects allocated: %d\n", M.nids);
    printf("cycles: %d, mark slices: %d, forced synchronous finishes: %d\n", ncycles, slices, sync_finishes);
    printf("barrier shaded %d objects, grey stack peak %d\n", barrier_shades, max_grey);
    printf("swept: %d, floating garbage over all cycles: %d\n", swept, floating_total);
    printf("live at end: %d\n", live);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
