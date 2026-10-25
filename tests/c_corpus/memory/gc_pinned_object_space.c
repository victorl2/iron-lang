/*
 * title: Copying collector with a non-moving pinned object space
 * topic: memory
 * covers: pin-at-allocation into a mark-sweep space, semispace copying for movable objects, mixed reference encoding, scan queue shared by both spaces, pinned slot stability check, cross-space edge fixups, reachability oracle
 * deps: libc
 */
#define SEED 0x5EED0016ULL
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


#define CAPM 100
#define CAPP 50
#define PIN_BASE 1000

typedef struct {
    int id;
    int f[M_F];
    int fwd;
    unsigned char used, mark;
} Obj;

static Obj sp[2][CAPM];
static Obj pin[CAPP];
static int cur, top;
static int pfree;
static int slot_of_id[M_IDS]; /* pinned slot recorded at allocation */
static int roots[M_R + 1];
static Obj *from, *to;
static int tfree;
static int pstack[CAPP], npst;
static int ncoll, moved, pinned_marked, pinned_swept, pin_to_mov, mov_to_pin;

static int is_pinned(int r) { return r >= PIN_BASE; }
static Obj *obj(int r) { return is_pinned(r) ? &pin[r - PIN_BASE] : &sp[cur][r]; }

static int evacuate(int r) {
    if (r < 0)
        return -1;
    if (is_pinned(r)) {
        Obj *p = &pin[r - PIN_BASE];
        if (!p->mark) {
            p->mark = 1;
            pstack[npst++] = r; /* fields fixed up later; the object itself never moves */
            pinned_marked++;
        }
        return r;
    }
    if (from[r].fwd >= 0)
        return from[r].fwd;
    int n = tfree++;
    to[n] = from[r];
    to[n].fwd = -1;
    from[r].fwd = n;
    moved++;
    return n;
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int i = 0; i < top; i++)
        present[sp[cur][i].id] = 1;
    for (int i = 0; i < CAPP; i++)
        if (pin[i].used) {
            present[pin[i].id] = 1;
            CHECK(slot_of_id[pin[i].id] == i); /* a pinned object never changes address */
        }
    model_verify(present, 1);
}

static void gc(void) {
    from = sp[cur];
    to = sp[1 - cur];
    tfree = 0;
    npst = 0;
    for (int i = 0; i <= M_R; i++)
        roots[i] = evacuate(roots[i]);
    int scan = 0;
    while (scan < tfree || npst > 0) {
        while (scan < tfree) {
            for (int k = 0; k < M_F; k++) {
                int t = to[scan].f[k];
                if (is_pinned(t))
                    mov_to_pin++;
                to[scan].f[k] = evacuate(t);
            }
            scan++;
        }
        while (npst > 0) {
            Obj *p = &pin[pstack[--npst] - PIN_BASE];
            for (int k = 0; k < M_F; k++) {
                if (p->f[k] >= 0 && !is_pinned(p->f[k]))
                    pin_to_mov++;
                p->f[k] = evacuate(p->f[k]);
            }
        }
    }
    cur = 1 - cur;
    top = tfree;
    pfree = -1;
    for (int i = CAPP - 1; i >= 0; i--) {
        if (pin[i].used && !pin[i].mark) {
            pin[i].used = 0;
            pinned_swept++;
        }
        pin[i].mark = 0;
        if (!pin[i].used) {
            pin[i].f[0] = pfree;
            pfree = i;
        }
    }
    ncoll++;
    verify();
}

static void alloc_root(int r, int idv) {
    int want_pin = idv % 4 == 1;
    if (want_pin ? pfree < 0 : top == CAPM)
        gc();
    if (want_pin) {
        CHECK(pfree >= 0);
        int s = pfree;
        pfree = pin[s].f[0];
        pin[s].used = 1;
        pin[s].mark = 0;
        pin[s].id = idv;
        for (int k = 0; k < M_F; k++)
            pin[s].f[k] = -1;
        slot_of_id[idv] = s;
        roots[r] = PIN_BASE + s;
    } else {
        CHECK(top < CAPM);
        Obj *o = &sp[cur][top];
        o->id = idv;
        o->fwd = -1;
        for (int k = 0; k < M_F; k++)
            o->f[k] = -1;
        roots[r] = top++;
    }
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return obj(ref)->f[k]; }
static void set_f(int ref, int k, int t) { obj(ref)->f[k] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return obj(ref)->id; }

int main(void) {
    model_reset();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    pfree = -1;
    for (int i = CAPP - 1; i >= 0; i--) {
        pin[i].f[0] = pfree;
        pfree = i;
    }
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, gc, NULL};
    drive(&o, 2000, 79);
    int pinned_live = 0;
    for (int i = 0; i < CAPP; i++)
        pinned_live += pin[i].used;
    printf("objects allocated: %d\n", M.nids);
    printf("collections: %d, movable objects copied: %d\n", ncoll, moved);
    printf("pinned objects marked: %d, swept: %d, live at end: %d\n", pinned_marked, pinned_swept, pinned_live);
    printf("edges movable->pinned: %d, pinned->movable fixed up: %d\n", mov_to_pin, pin_to_mov);
    printf("movable space in use at end: %d of %d\n", top, CAPM);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
