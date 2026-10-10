/*
 * title: Train algorithm (Hudson-Moss) collecting one car at a time
 * topic: memory
 * covers: cars grouped into trains, per-car incoming reference counts maintained by a write barrier, reclaiming a whole train with no external references, evacuating a car to the referencing train, cross-train garbage cycles migrating until collectable, remembered-set invariant check
 * deps: libc
 */
#define SEED 0x5EED0018ULL
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


#define CAP 220
#define CARSZ 6
#define MAXCARS 80
#define TRAIN_EVERY 40

typedef struct {
    int id;
    int f[M_F];
    int car;
    unsigned char used;
} Obj;
typedef struct {
    int train, n, seq;
    unsigned char live;
} Car;

static Obj heap[CAP];
static Car cars[MAXCARS];
static int xref[MAXCARS][MAXCARS]; /* pointer fields from objects in car s to objects in car d */
static int roots[M_R + 1];
static int freeslots[CAP], nfreeslots;
static int seqctr, maxtrain, alloc_car = -1, alloc_count;
static int steps, reclaimed_trains, evacuations, moved, freed_objs, new_trains, new_cars, max_cars_live;

static void xinc(int a, int b) {
    if (a != b)
        xref[a][b]++;
}
static void xdec(int a, int b) {
    if (a != b) {
        CHECK(xref[a][b] > 0);
        xref[a][b]--;
    }
}
static void detach(int o) {
    for (int k = 0; k < M_F; k++)
        if (heap[o].f[k] >= 0)
            xdec(heap[o].car, heap[heap[o].f[k]].car);
    for (int s = 0; s < CAP; s++)
        if (s != o && heap[s].used)
            for (int k = 0; k < M_F; k++)
                if (heap[s].f[k] == o)
                    xdec(heap[s].car, heap[o].car);
}
static void attach(int o) {
    for (int k = 0; k < M_F; k++)
        if (heap[o].f[k] >= 0)
            xinc(heap[o].car, heap[heap[o].f[k]].car);
    for (int s = 0; s < CAP; s++)
        if (s != o && heap[s].used)
            for (int k = 0; k < M_F; k++)
                if (heap[s].f[k] == o)
                    xinc(heap[s].car, heap[o].car);
}

static void check_remsets(void) {
    static int want[MAXCARS][MAXCARS];
    memset(want, 0, sizeof want);
    for (int s = 0; s < CAP; s++)
        if (heap[s].used)
            for (int k = 0; k < M_F; k++) {
                int t = heap[s].f[k];
                if (t >= 0) {
                    CHECK(heap[t].used);
                    if (heap[s].car != heap[t].car)
                        want[heap[s].car][heap[t].car]++;
                }
            }
    for (int a = 0; a < MAXCARS; a++)
        for (int b = 0; b < MAXCARS; b++)
            CHECK(want[a][b] == xref[a][b]);
}

static int new_car(int train) {
    for (int i = 0; i < MAXCARS; i++)
        if (!cars[i].live) {
            cars[i].live = 1;
            cars[i].train = train;
            cars[i].n = 0;
            cars[i].seq = ++seqctr;
            new_cars++;
            int live = 0;
            for (int j = 0; j < MAXCARS; j++)
                live += cars[j].live;
            if (live > max_cars_live)
                max_cars_live = live;
            return i;
        }
    return -1;
}

static int tail_car(int train, int exclude) {
    int best = -1;
    for (int i = 0; i < MAXCARS; i++)
        if (cars[i].live && cars[i].train == train && i != exclude && (best < 0 || cars[i].seq > cars[best].seq))
            best = i;
    if (best >= 0 && cars[best].n < CARSZ)
        return best;
    int c = new_car(train);
    CHECK(c >= 0);
    return c;
}

static void move_obj(int o, int dc) {
    detach(o);
    cars[heap[o].car].n--;
    heap[o].car = dc;
    cars[dc].n++;
    attach(o);
    moved++;
}

static void free_obj(int o) {
    detach(o); /* drops every count involving o */
    for (int s = 0; s < CAP; s++) /* only garbage can still point at o: clear those fields */
        if (heap[s].used)
            for (int k = 0; k < M_F; k++)
                if (heap[s].f[k] == o)
                    heap[s].f[k] = -1;
    cars[heap[o].car].n--;
    heap[o].used = 0;
    freeslots[nfreeslots++] = o;
    freed_objs++;
}

static void step(void) {
    int t = -1;
    for (int i = 0; i < MAXCARS; i++)
        if (cars[i].live && (t < 0 || cars[i].train < t))
            t = cars[i].train;
    if (t < 0)
        return;
    int c = -1;
    for (int i = 0; i < MAXCARS; i++)
        if (cars[i].live && cars[i].train == t && (c < 0 || cars[i].seq < cars[c].seq))
            c = i;
    steps++;
    int ext = 0;
    for (int i = 0; i <= M_R; i++)
        if (roots[i] >= 0 && cars[heap[roots[i]].car].train == t)
            ext = 1;
    for (int s = 0; s < MAXCARS && !ext; s++)
        if (cars[s].live && cars[s].train != t)
            for (int d = 0; d < MAXCARS; d++)
                if (cars[d].live && cars[d].train == t && xref[s][d] > 0)
                    ext = 1;
    if (!ext) { /* nothing outside the train points in: the whole train is garbage, cycles included */
        for (int o = 0; o < CAP; o++)
            if (heap[o].used && cars[heap[o].car].train == t)
                free_obj(o);
        for (int i = 0; i < MAXCARS; i++)
            if (cars[i].live && cars[i].train == t)
                cars[i].live = 0;
        reclaimed_trains++;
        return;
    }
    evacuations++;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int o = 0; o < CAP; o++) {
            if (!heap[o].used || heap[o].car != c)
                continue;
            int dt = -1;
            for (int i = 0; i <= M_R && dt < 0; i++)
                if (roots[i] == o) {
                    if (maxtrain == t)
                        maxtrain++;
                    dt = maxtrain;
                }
            for (int s = 0; s < CAP && dt < 0; s++)
                if (heap[s].used && heap[s].car != c)
                    for (int k = 0; k < M_F; k++)
                        if (heap[s].f[k] == o)
                            dt = cars[heap[s].car].train;
            if (dt >= 0) {
                move_obj(o, tail_car(dt, c));
                changed = 1;
            }
        }
    }
    for (int o = 0; o < CAP; o++)
        if (heap[o].used && heap[o].car == c)
            free_obj(o);
    cars[c].live = 0;
}

static void verify(int exact) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int o = 0; o < CAP; o++)
        if (heap[o].used)
            present[heap[o].id] = 1;
    check_remsets();
    model_verify(present, exact);
}

static void collect(void) {
    for (int i = 0; i < 6; i++)
        step();
    verify(0);
}

static void tick(void) {
    if (nfreeslots < CAP / 2 || rnd_n(4) == 0)
        step();
}

static void alloc_root(int r, int idv) {
    for (int guard = 0; nfreeslots == 0; guard++) {
        CHECK(guard < 500);
        step();
    }
    if (alloc_count++ % TRAIN_EVERY == 0) {
        maxtrain++;
        new_trains++;
        alloc_car = -1;
    }
    if (alloc_car < 0 || cars[alloc_car].n >= CARSZ || !cars[alloc_car].live) {
        for (int guard = 0; (alloc_car = new_car(maxtrain)) < 0; guard++) {
            CHECK(guard < 500);
            step();
        }
    }
    int o = freeslots[--nfreeslots];
    heap[o].used = 1;
    heap[o].id = idv;
    heap[o].car = alloc_car;
    for (int k = 0; k < M_F; k++)
        heap[o].f[k] = -1;
    cars[alloc_car].n++;
    roots[r] = o;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return heap[ref].f[k]; }
static void set_f(int ref, int k, int t) { /* the write barrier keeps car-to-car counts exact */
    if (heap[ref].f[k] >= 0)
        xdec(heap[ref].car, heap[heap[ref].f[k]].car);
    heap[ref].f[k] = t;
    if (t >= 0)
        xinc(heap[ref].car, heap[t].car);
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return heap[ref].id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    for (int i = CAP - 1; i >= 0; i--)
        freeslots[nfreeslots++] = i;
    maxtrain = 0;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, collect, tick};
    drive(&o, 1600, 50);
    /* quiesce: with the mutator stopped, repeated steps must eventually drain all garbage */
    int extra_steps = 0, exact = 0;
    while (!exact && extra_steps < 3000) {
        step();
        extra_steps++;
        unsigned char reach[M_IDS];
        model_reach(reach);
        exact = 1;
        for (int i = 0; i < CAP; i++)
            if (heap[i].used && !reach[heap[i].id])
                exact = 0;
    }
    CHECK(exact);
    verify(1);
    int live = 0;
    for (int i = 0; i < CAP; i++)
        live += heap[i].used;
    printf("objects allocated: %d, trains created: %d, cars created: %d\n", M.nids, new_trains, new_cars);
    printf("collector steps: %d (whole trains reclaimed: %d, cars evacuated: %d)\n", steps, reclaimed_trains, evacuations);
    printf("objects moved between cars: %d, objects freed: %d\n", moved, freed_objs);
    printf("most cars live at once: %d\n", max_cars_live);
    printf("steps to drain all garbage after the mutator stopped: %d\n", extra_steps);
    printf("objects live at end: %d\n", live);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
