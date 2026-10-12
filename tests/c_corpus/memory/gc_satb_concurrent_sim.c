/*
 * title: Simulated concurrent marker with SATB buffers and remark pause
 * topic: memory
 * covers: snapshot-at-the-beginning, thread-local SATB buffer flushed to a global queue, marker slices with random budgets, initial-mark and remark pauses, pause work counters, mark bits, allocate-marked, snapshot retention check
 * deps: libc
 */
#define SEED 0x5EED000DULL
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
#define TRIGGER 45
#define BUFCAP 4
enum { IDLE, CONCURRENT };

typedef struct {
    int id;
    int f[M_F];
    unsigned char used, marked;
} Obj;

static Obj heap[CAP];
static int freelist, nfree;
static int roots[M_R + 1];
static int work[CAP * 2], nwork;     /* marker work list: refs still to scan */
static int gqueue[CAP * 4], ngq;     /* global SATB queue fed by buffer flushes */
static int lbuf[BUFCAP], nlbuf;      /* mutator-local SATB buffer */
static int phase;
static unsigned char snap[M_IDS];
static int ncycles, logged, redundant, flushes, initial_work, remark_work, max_remark, marker_work;
static int concurrent_ticks, floating_total, swept, alloc_marked, sync_cycles;

static void enqueue_ref(int r) {
    if (r >= 0 && !heap[r].marked) {
        heap[r].marked = 1;
        work[nwork++] = r;
    }
}

static int drain_queue(void) {
    int n = 0;
    for (int i = 0; i < ngq; i++) {
        enqueue_ref(gqueue[i]);
        n++;
    }
    ngq = 0;
    return n;
}

static int scan_one(void) {
    int o = work[--nwork];
    for (int k = 0; k < M_F; k++)
        enqueue_ref(heap[o].f[k]);
    return 1;
}

static void flush_local(void) {
    for (int i = 0; i < nlbuf; i++)
        gqueue[ngq++] = lbuf[i];
    if (nlbuf)
        flushes++;
    nlbuf = 0;
}

static void start_cycle(void) {
    phase = CONCURRENT;
    model_reach(snap);
    nwork = ngq = nlbuf = 0;
    for (int i = 0; i <= M_R; i++) { /* initial-mark pause: only the roots */
        enqueue_ref(roots[i]);
        initial_work++;
    }
}

static int verify_end(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    int cnt = 0;
    for (int i = 0; i < CAP; i++)
        if (heap[i].used) {
            present[heap[i].id] = 1;
            cnt++;
        }
    for (int id = 0; id < M.nids; id++)
        if (snap[id])
            CHECK(present[id]); /* SATB retains everything reachable at the snapshot */
    int n = model_verify(present, 0);
    return cnt - n;
}

static void remark_and_sweep(void) {
    int w = 0;
    flush_local(); /* handshake with the mutator */
    for (;;) {
        w += drain_queue();
        if (nwork == 0)
            break;
        while (nwork > 0)
            w += scan_one();
    }
    remark_work += w;
    if (w > max_remark)
        max_remark = w;
    freelist = -1;
    nfree = 0;
    for (int i = CAP - 1; i >= 0; i--) {
        if (heap[i].used && !heap[i].marked) {
            heap[i].used = 0;
            swept++;
        }
        heap[i].marked = 0;
        if (!heap[i].used) {
            heap[i].f[0] = freelist;
            freelist = i;
            nfree++;
        }
    }
    phase = IDLE;
    ncycles++;
    floating_total += verify_end();
}

static void tick(void) {
    if (phase == IDLE) {
        if (nfree < TRIGGER)
            start_cycle();
        return;
    }
    concurrent_ticks++;
    int budget = (int)rnd_n(5); /* the marker thread is scheduled unevenly */
    while (budget-- > 0 && (nwork > 0 || ngq > 0)) {
        if (nwork == 0)
            drain_queue();
        else {
            marker_work += scan_one();
        }
    }
    if (nwork == 0 && ngq == 0 && rnd_n(3) == 0)
        remark_and_sweep();
}

static void collect(void) {
    if (phase == IDLE) {
        start_cycle();
        sync_cycles++;
    }
    remark_and_sweep();
}

static void alloc_root(int r, int id) {
    if (freelist < 0) {
        if (phase == IDLE)
            start_cycle();
        remark_and_sweep();
    }
    CHECK(freelist >= 0);
    int s = freelist;
    freelist = heap[s].f[0];
    nfree--;
    heap[s].used = 1;
    heap[s].marked = phase == CONCURRENT; /* allocate marked during the cycle */
    if (phase == CONCURRENT)
        alloc_marked++;
    heap[s].id = id;
    for (int k = 0; k < M_F; k++)
        heap[s].f[k] = -1;
    roots[r] = s;
}
static int get_root(int r) { return roots[r]; }
static int get_f(int ref, int k) { return heap[ref].f[k]; }
static void set_f(int ref, int k, int t) {
    int old = heap[ref].f[k];
    if (phase == CONCURRENT && old >= 0) { /* pre-write barrier: log the old value */
        logged++;
        if (heap[old].marked)
            redundant++;
        lbuf[nlbuf++] = old;
        if (nlbuf == BUFCAP)
            flush_local();
    }
    heap[ref].f[k] = t;
}
static void set_root(int r, int ref) { roots[r] = ref; }
static int id_of(int ref) { return heap[ref].id; }

int main(void) {
    model_reset();
    freelist = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        heap[i].f[0] = freelist;
        freelist = i;
        nfree++;
    }
    for (int i = 0; i <= M_R; i++)
        roots[i] = -1;
    Ops o = {alloc_root, get_root, get_f, set_f, set_root, id_of, collect, tick};
    drive(&o, 2500, 700);
    printf("objects allocated: %d\n", M.nids);
    printf("cycles: %d (%d forced), concurrent ticks: %d\n", ncycles, sync_cycles, concurrent_ticks);
    printf("initial-mark pause work: %d, remark pause work total %d max %d\n", initial_work, remark_work, max_remark);
    printf("marker scanned concurrently: %d\n", marker_work);
    printf("SATB entries logged: %d (already marked: %d), buffer flushes: %d\n", logged, redundant, flushes);
    printf("allocated marked: %d, swept: %d, floating garbage: %d\n", alloc_marked, swept, floating_total);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
