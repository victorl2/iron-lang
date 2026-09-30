/*
 * title: Weak references, finalizers and resurrection in a mark-sweep heap
 * topic: memory
 * covers: weak reference clearing, reference queue, finalization queue, resurrection by marking the finalizable closure, finalize-once rule, graveyard roots, independent set-based oracle for survivors and cleared references
 * deps: libc
 */
#define SEED 0x5EED0010ULL
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



#define CAP 160
#define NROOT 6
#define NGRAVE 4
enum { PLAIN, WEAKREF, FINAL };

typedef struct {
    int id;
    int kind;
    int f[2]; /* WEAKREF: f[0] is the weak referent, f[1] is strong */
    unsigned char used, mark, finalized, pending;
} Obj;

/* Shadow description of the graph, maintained by the mutator only. */
typedef struct {
    int kind;
    int e[2];
    unsigned char alive, finalized;
} Sh;

static Obj heap[CAP];
static Sh sh[CAP];
static int freelist, nfree;
static int roots[NROOT + NGRAVE];
static int nid;
static int weak_examined;
static int cycles, weak_cleared, finalizers_run, resurrected, freed, queued;
static unsigned fin_hash = 2166136261u;
static int fin_runs[4096];

static void mark_strong(int o) {
    if (o < 0 || heap[o].mark)
        return;
    heap[o].mark = 1;
    if (heap[o].kind == WEAKREF)
        mark_strong(heap[o].f[1]); /* the weak field is not traced */
    else {
        mark_strong(heap[o].f[0]);
        mark_strong(heap[o].f[1]);
    }
}
static void mark_all(int o) {
    if (o < 0 || heap[o].mark)
        return;
    heap[o].mark = 1;
    if (heap[o].kind != WEAKREF)
        mark_all(heap[o].f[0]);
    mark_all(heap[o].f[1]);
}

/* Oracle: recomputes what must survive from the shadow graph, with plain BFS. */
static void oracle(unsigned char *strong, unsigned char *keep, unsigned char *fin_now) {
    int queue[CAP], qh = 0, qt = 0;
    memset(strong, 0, CAP);
    for (int i = 0; i < NROOT + NGRAVE; i++)
        if (roots[i] >= 0 && !strong[roots[i]]) {
            strong[roots[i]] = 1;
            queue[qt++] = roots[i];
        }
    while (qh < qt) {
        int o = queue[qh++];
        for (int k = 0; k < 2; k++) {
            if (sh[o].kind == WEAKREF && k == 0)
                continue;
            int c = sh[o].e[k];
            if (c >= 0 && !strong[c]) {
                strong[c] = 1;
                queue[qt++] = c;
            }
        }
    }
    memcpy(keep, strong, CAP);
    qh = qt = 0;
    for (int i = 0; i < CAP; i++) {
        fin_now[i] = sh[i].alive && sh[i].kind == FINAL && !sh[i].finalized && !strong[i];
        if (fin_now[i]) {
            keep[i] = 1;
            queue[qt++] = i;
        }
    }
    while (qh < qt) {
        int o = queue[qh++];
        for (int k = 0; k < 2; k++) {
            if (sh[o].kind == WEAKREF && k == 0)
                continue;
            int c = sh[o].e[k];
            if (c >= 0 && !keep[c]) {
                keep[c] = 1;
                queue[qt++] = c;
            }
        }
    }
}

static void gc(void) {
    unsigned char strong[CAP], keep[CAP], fin_now[CAP], exp_null[CAP];
    oracle(strong, keep, fin_now);
    for (int i = 0; i < CAP; i++)
        exp_null[i] = sh[i].alive && sh[i].kind == WEAKREF && (sh[i].e[0] < 0 || !strong[sh[i].e[0]]);
    for (int i = 0; i < NROOT + NGRAVE; i++)
        mark_strong(roots[i]);
    unsigned char smark[CAP];
    for (int i = 0; i < CAP; i++)
        smark[i] = heap[i].mark;
    /* 2. unreachable finalizable objects (and everything they reach) are resurrected */
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && heap[i].kind == FINAL && !heap[i].finalized && !heap[i].mark) {
            heap[i].pending = 1;
            CHECK(fin_now[i]);
        }
    for (int i = 0; i < CAP; i++)
        if (heap[i].pending)
            mark_all(i);
    /* 1b. clear weak references whose referent was not strongly reachable, before any finalizer runs */
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && heap[i].kind == WEAKREF && heap[i].mark && heap[i].f[0] >= 0)
            weak_examined++;
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && heap[i].kind == WEAKREF && heap[i].mark && heap[i].f[0] >= 0 && !smark[heap[i].f[0]]) {
            heap[i].f[0] = -1;
            sh[i].e[0] = -1;
            weak_cleared++;
            queued++;
        }
    /* 3. sweep and compare with the oracle */
    for (int i = 0; i < CAP; i++) {
        if (!heap[i].used)
            continue;
        CHECK(heap[i].mark == keep[i]);
        if (!heap[i].mark) {
            heap[i].used = 0;
            sh[i].alive = 0;
            freed++;
            heap[i].f[0] = freelist;
            freelist = i;
            nfree++;
        }
        heap[i].mark = 0;
    }
    /* weak references are cleared exactly when the referent was not strongly reachable */
    for (int i = 0; i < CAP; i++)
        if (heap[i].used && heap[i].kind == WEAKREF) {
            CHECK((heap[i].f[0] < 0) == (exp_null[i] != 0));
            if (heap[i].f[0] >= 0)
                CHECK(heap[heap[i].f[0]].used);
        }
    /* 4. run finalizers in slot order; every third id resurrects itself */
    for (int i = 0; i < CAP; i++) {
        if (!heap[i].pending)
            continue;
        heap[i].pending = 0;
        heap[i].finalized = 1;
        sh[i].finalized = 1;
        fin_runs[heap[i].id]++;
        CHECK(fin_runs[heap[i].id] == 1);
        finalizers_run++;
        fin_hash = (fin_hash ^ (unsigned)heap[i].id) * 16777619u;
        if (heap[i].id % 3 == 0) {
            roots[NROOT + resurrected % NGRAVE] = i;
            resurrected++;
        }
    }
    cycles++;
}

static int alloc(int kind) {
    if (freelist < 0)
        gc();
    CHECK(freelist >= 0);
    int s = freelist;
    freelist = heap[s].f[0];
    nfree--;
    memset(&heap[s], 0, sizeof heap[s]);
    heap[s].used = 1;
    heap[s].id = nid++;
    heap[s].kind = kind;
    heap[s].f[0] = heap[s].f[1] = -1;
    sh[s].kind = kind;
    sh[s].e[0] = sh[s].e[1] = -1;
    sh[s].alive = 1;
    sh[s].finalized = 0;
    return s;
}

static int pick(void) { /* a strongly reachable object, found by walking the shadow graph */
    int r = (int)rnd_n(NROOT);
    int o = roots[r];
    if (o < 0)
        return -1;
    int depth = (int)rnd_n(5);
    for (int d = 0; d < depth; d++) {
        int k = (int)rnd_n(2);
        if (sh[o].kind == WEAKREF && k == 0)
            k = 1;
        if (sh[o].e[k] < 0)
            break;
        o = sh[o].e[k];
    }
    return o;
}

static void link(int a, int k, int b) {
    heap[a].f[k] = b;
    sh[a].e[k] = b;
}

int main(void) {
    for (int i = 0; i < NROOT + NGRAVE; i++)
        roots[i] = -1;
    freelist = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        heap[i].f[0] = freelist;
        freelist = i;
        nfree++;
    }
    for (int step = 0; step < 1600; step++) {
        unsigned c = rnd_n(100);
        if (c < 45) {
            unsigned kr = rnd_n(10);
            int kind = kr < 4 ? PLAIN : (kr < 8 ? WEAKREF : FINAL);
            int s = alloc(kind); /* may collect; the pick happens afterwards */
            int tmp_root = (int)rnd_n(NROOT);
            int dst = pick();
            if (dst < 0 || rnd_n(4) == 0) {
                roots[tmp_root] = s;
            } else {
                int k = (int)rnd_n(2);
                if (sh[dst].kind == WEAKREF)
                    k = 1;
                link(dst, k, s);
            }
        } else if (c < 78) {
            int a = pick(), b = pick();
            if (a >= 0 && b >= 0) {
                int k = (int)rnd_n(2);
                if (sh[a].kind == WEAKREF && rnd_n(5) != 0)
                    k = 0; /* set the weak referent */
                link(a, k, b);
            }
        } else if (c < 88) {
            int a = pick();
            if (a >= 0)
                link(a, (int)rnd_n(2), -1);
        } else {
            roots[rnd_n(NROOT)] = -1;
        }
        if (step % 160 == 159)
            gc();
    }
    gc();
    int survivors = 0, weak_left = 0;
    for (int i = 0; i < CAP; i++) {
        survivors += heap[i].used;
        weak_left += heap[i].used && heap[i].kind == WEAKREF && heap[i].f[0] >= 0;
    }
    printf("objects allocated: %d\n", nid);
    printf("collections: %d, freed: %d, survivors: %d\n", cycles, freed, survivors);
    printf("weak references examined: %d, cleared: %d, still set at end: %d\n", weak_examined, weak_cleared, weak_left);
    printf("finalizers run: %d, resurrected into graveyard: %d\n", finalizers_run, resurrected);
    printf("reference queue entries: %d\n", queued);
    printf("finalization order hash: %08x\n", fin_hash);
    return 0;
}
