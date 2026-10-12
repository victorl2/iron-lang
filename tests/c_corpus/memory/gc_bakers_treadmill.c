/*
 * title: Baker's treadmill non-moving incremental collector
 * topic: memory
 * covers: colors as membership of doubly linked lists, constant-time list splicing, read barrier moving ecru cells to grey, allocate into the new list, whole-list reclamation at cycle end, tri-color invariant check
 * deps: libc
 */
#define SEED 0x5EED0014ULL
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


#define CAP 140
#define TRIGGER 40
#define SLICE 3
enum { FREE, NEWL, ECRU, GREY, BLACK, NLISTS };
#define SENT(l) (CAP + (l))

static int nxt[CAP + NLISTS], prv[CAP + NLISTS];
static int where[CAP];
static int count[NLISTS];
static int id[CAP], fld[CAP][M_F];
static int roots[M_R + 1];
static int active; /* a collection cycle is in progress */
static int cycles, scanned, barrier_moves, splices, max_grey, flips, sync_finish;

static void list_init(void) {
    for (int l = 0; l < NLISTS; l++) {
        nxt[SENT(l)] = prv[SENT(l)] = SENT(l);
        count[l] = 0;
    }
}

static void unlink_cell(int c) {
    nxt[prv[c]] = nxt[c];
    prv[nxt[c]] = prv[c];
    count[where[c]]--;
}
static void push_tail(int l, int c) {
    int s = SENT(l);
    prv[c] = prv[s];
    nxt[c] = s;
    nxt[prv[s]] = c;
    prv[s] = c;
    where[c] = l;
    count[l]++;
}
static void move_to(int l, int c) {
    unlink_cell(c);
    push_tail(l, c);
}
/* O(1): splice all of list src onto the tail of dst. */
static void splice_all(int dst, int src) {
    int ss = SENT(src), ds = SENT(dst);
    if (nxt[ss] == ss)
        return;
    int first = nxt[ss], last = prv[ss];
    nxt[prv[ds]] = first;
    prv[first] = prv[ds];
    nxt[last] = ds;
    prv[ds] = last;
    nxt[ss] = prv[ss] = ss;
    count[dst] += count[src];
    count[src] = 0;
    splices++;
    /* the per-cell membership tags are refreshed by retag() afterwards */
}
static void retag(int l) {
    for (int c = nxt[SENT(l)]; c != SENT(l); c = nxt[c])
        where[c] = l;
}

static void shade(int c) {
    if (c >= 0 && where[c] == ECRU) {
        move_to(GREY, c);
        if (count[GREY] > max_grey)
            max_grey = count[GREY];
    }
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int c = 0; c < CAP; c++)
        if (where[c] != FREE)
            present[id[c]] = 1;
    model_verify(present, 0);
}

static void finish(void) {
    /* ecru cells are unreachable: reclaim them all at once */
    splice_all(FREE, ECRU);
    retag(FREE);
    active = 0;
    cycles++;
    verify();
}

static void flip(void) {
    /* everything allocated or surviving becomes the from-set, then roots are shaded */
    splice_all(ECRU, BLACK);
    splice_all(ECRU, NEWL);
    retag(ECRU);
    for (int i = 0; i <= M_R; i++)
        shade(roots[i]);
    active = 1;
    flips++;
}

static void scan_one(void) {
    int c = nxt[SENT(GREY)];
    for (int k = 0; k < M_F; k++)
        shade(fld[c][k]);
    move_to(BLACK, c);
    scanned++;
}

static void step_check(void) {
    for (int c = 0; c < CAP; c++)
        if (where[c] == BLACK)
            for (int k = 0; k < M_F; k++)
                CHECK(fld[c][k] < 0 || where[fld[c][k]] != ECRU);
    for (int i = 0; i <= M_R; i++)
        CHECK(roots[i] < 0 || where[roots[i]] != ECRU);
}

static void tick(void) {
    if (!active) {
        if (count[FREE] < TRIGGER)
            flip();
        return;
    }
    for (int i = 0; i < SLICE && count[GREY] > 0; i++)
        scan_one();
    step_check();
    if (count[GREY] == 0)
        finish();
}

static void collect(void) {
    if (!active)
        flip();
    while (count[GREY] > 0)
        scan_one();
    finish();
}

static void alloc_root(int r, int newid) {
    if (count[FREE] == 0) {
        collect();
        sync_finish++;
    }
    CHECK(count[FREE] > 0);
    int c = nxt[SENT(FREE)];
    move_to(NEWL, c); /* allocated black, appended to the new list */
    id[c] = newid;
    for (int k = 0; k < M_F; k++)
        fld[c][k] = -1;
    roots[r] = c;
}
/* Baker read barrier: the mutator never holds an ecru cell. */
static int get_root(int r) {
    int c = roots[r];
    if (active && c >= 0 && where[c] == ECRU) {
        shade(c);
        barrier_moves++;
    }
    return c;
}
static int get_f(int ref, int k) {
    int c = fld[ref][k];
    if (active && c >= 0 && where[c] == ECRU) {
        shade(c);
        barrier_moves++;
    }
    return c;
}
static void set_f(int ref, int k, int t) { fld[ref][k] = t; }
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return id[ref]; }

int main(void) {
    model_reset();
    list_init();
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    for (int c = 0; c < CAP; c++)
        push_tail(FREE, c);
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, collect, tick};
    drive(&o, 2200, 450);
    int live = CAP - count[FREE];
    CHECK(count[FREE] + count[NEWL] + count[ECRU] + count[GREY] + count[BLACK] == CAP);
    printf("objects allocated: %d\n", M.nids);
    printf("cycles: %d (forced by allocation: %d), cells scanned: %d\n", cycles, sync_finish, scanned);
    printf("read barrier moved %d cells from ecru to grey\n", barrier_moves);
    printf("O(1) list splices: %d, grey peak: %d\n", splices, max_grey);
    printf("cells in use at end: %d of %d\n", live, CAP);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
