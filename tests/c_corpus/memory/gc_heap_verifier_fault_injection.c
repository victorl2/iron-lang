/*
 * title: Heap verifier with deterministic fault injection
 * topic: memory
 * covers: heap invariants (reference validity, free list shape, mark and forwarding state, canaries, unique ids, root validity), verifier error masks mapped to names, targeted corruption of each invariant, repair by snapshot restore, byte-level bit-flip campaign with detection statistics
 * deps: libc
 */
#define SEED 0x5EED0026ULL
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



#define CAP 48
#define NR 4
#define CANARY 0xC0FFEE11u

/* Only ints: no padding, so a snapshot copy captures the whole state. */
typedef struct {
    unsigned canary_lo;
    int id;
    int f0, f1;
    int used, mark, fwd;
    unsigned canary_hi;
} Slot;

typedef struct {
    Slot s[CAP];
    int freehead;
    int roots[NR];
} Heap;

static Heap hp;
static int next_id = 1;

enum { E_REF_RANGE, E_DANGLING, E_FREELIST_CYCLE, E_FREELIST_USED, E_FREELIST_COUNT, E_STALE_MARK, E_STALE_FWD,
       E_CANARY, E_DUP_ID, E_BAD_ROOT, NERR };
static const char *err_name[NERR] = {"ref-out-of-range", "dangling-ref", "freelist-cycle", "used-slot-on-freelist",
                                     "freelist-length", "stale-mark", "stale-forward", "canary", "duplicate-id",
                                     "bad-root"};

static unsigned verify_heap(void) {
    unsigned mask = 0;
    int nused = 0;
    for (int i = 0; i < CAP; i++) {
        Slot *o = &hp.s[i];
        if (o->canary_lo != CANARY || o->canary_hi != CANARY) {
            mask |= 1u << E_CANARY;
            continue; /* the slot contents cannot be trusted */
        }
        if (o->mark)
            mask |= 1u << E_STALE_MARK;
        if (o->fwd != -1)
            mask |= 1u << E_STALE_FWD;
        if (!o->used)
            continue;
        nused++;
        int refs[2] = {o->f0, o->f1};
        for (int k = 0; k < 2; k++) {
            if (refs[k] == -1)
                continue;
            if (refs[k] < 0 || refs[k] >= CAP)
                mask |= 1u << E_REF_RANGE;
            else if (!hp.s[refs[k]].used)
                mask |= 1u << E_DANGLING;
        }
        for (int j = 0; j < i; j++)
            if (hp.s[j].used && hp.s[j].canary_lo == CANARY && hp.s[j].id == o->id)
                mask |= 1u << E_DUP_ID;
    }
    for (int r = 0; r < NR; r++) {
        int v = hp.roots[r];
        if (v != -1 && (v < 0 || v >= CAP || !hp.s[v].used))
            mask |= 1u << E_BAD_ROOT;
    }
    /* free list: bounded walk, so a cycle is reported instead of hanging */
    int len = 0, cur = hp.freehead, ok = 1;
    while (cur != -1) {
        if (cur < 0 || cur >= CAP) {
            mask |= 1u << E_REF_RANGE;
            ok = 0;
            break;
        }
        if (++len > CAP) {
            mask |= 1u << E_FREELIST_CYCLE;
            ok = 0;
            break;
        }
        if (hp.s[cur].used)
            mask |= 1u << E_FREELIST_USED;
        cur = hp.s[cur].f0;
    }
    if (ok && len != CAP - nused && !(mask & (1u << E_CANARY)))
        mask |= 1u << E_FREELIST_COUNT;
    return mask;
}

static void print_mask(unsigned m) {
    int first = 1;
    for (int e = 0; e < NERR; e++)
        if (m & (1u << e)) {
            printf("%s%s", first ? "" : "+", err_name[e]);
            first = 0;
        }
    if (first)
        printf("clean");
}

/* ---- a small mutator so the heap has a realistic shape ---- */
static void mark(int o) {
    if (o < 0 || hp.s[o].mark)
        return;
    hp.s[o].mark = 1;
    mark(hp.s[o].f0);
    mark(hp.s[o].f1);
}
static void gc(void) {
    for (int r = 0; r < NR; r++)
        mark(hp.roots[r]);
    hp.freehead = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        if (hp.s[i].used && !hp.s[i].mark)
            hp.s[i].used = 0;
        hp.s[i].mark = 0;
        if (!hp.s[i].used) {
            hp.s[i].f0 = hp.freehead;
            hp.s[i].f1 = -1;
            hp.freehead = i;
        }
    }
}
static int alloc_obj(void) {
    if (hp.freehead < 0)
        gc();
    CHECK(hp.freehead >= 0);
    int s = hp.freehead;
    hp.freehead = hp.s[s].f0;
    hp.s[s].used = 1;
    hp.s[s].id = next_id++;
    hp.s[s].f0 = hp.s[s].f1 = -1;
    return s;
}

static void init_heap(void) {
    for (int i = 0; i < CAP; i++) {
        hp.s[i].canary_lo = hp.s[i].canary_hi = CANARY;
        hp.s[i].used = hp.s[i].mark = 0;
        hp.s[i].fwd = -1;
        hp.s[i].id = 0;
        hp.s[i].f0 = i + 1 < CAP ? i + 1 : -1;
        hp.s[i].f1 = -1;
    }
    hp.freehead = 0;
    for (int r = 0; r < NR; r++)
        hp.roots[r] = -1;
}

static void workload(void) {
    for (int step = 0; step < 400; step++) {
        int r = (int)rnd_n(NR);
        unsigned c = rnd_n(10);
        if (c < 5) {
            int s = alloc_obj();
            int cur = hp.roots[r];
            if (cur < 0 || rnd_n(4) == 0) {
                hp.roots[r] = s;
            } else {
                if (rnd_n(2))
                    hp.s[cur].f0 = s;
                else
                    hp.s[cur].f1 = s;
            }
        } else if (c < 8) {
            int a = hp.roots[r], b = hp.roots[rnd_n(NR)];
            if (a >= 0 && b >= 0)
                (rnd_n(2) ? &hp.s[a].f0 : &hp.s[a].f1)[0] = b;
        } else {
            hp.roots[r] = -1;
        }
        if (step % 60 == 59) {
            gc();
            CHECK(verify_heap() == 0);
        }
    }
    gc();
}

static int first_used(int skip) {
    for (int i = 0; i < CAP; i++)
        if (hp.s[i].used && skip-- == 0)
            return i;
    return -1;
}
static int first_free(void) { return hp.freehead; }

int main(void) {
    init_heap();
    workload();
    CHECK(verify_heap() == 0);
    int nused = 0;
    for (int i = 0; i < CAP; i++)
        nused += hp.s[i].used;
    printf("workload done: %d of %d slots in use, verifier says ", nused, CAP);
    print_mask(verify_heap());
    printf("\n");

    Heap snapshot = hp;
    int u0 = first_used(0), u1 = first_used(1), f0 = first_free();
    CHECK(u0 >= 0 && u1 >= 0 && f0 >= 0);
    int last_free = f0;
    while (hp.s[last_free].f0 != -1)
        last_free = hp.s[last_free].f0;
    /* one targeted corruption per invariant */
    struct {
        const char *what;
        unsigned expect;
    } faults[] = {
        {"live field set to a free slot", 1u << E_DANGLING},
        {"live field set past the heap", 1u << E_REF_RANGE},
        {"free list tail linked to its head", 1u << E_FREELIST_CYCLE},
        {"live slot pushed onto the free list", (1u << E_DANGLING) | (1u << E_FREELIST_USED) | (1u << E_FREELIST_COUNT)},
        {"free list truncated", 1u << E_FREELIST_COUNT},
        {"mark bit left set", 1u << E_STALE_MARK},
        {"forwarding pointer left behind", 1u << E_STALE_FWD},
        {"canary overwritten", 1u << E_CANARY},
        {"two live objects share an id", 1u << E_DUP_ID},
        {"root points at a free slot", 1u << E_BAD_ROOT},
    };
    for (int t = 0; t < 10; t++) {
        switch (t) {
        case 0: hp.s[u0].f0 = f0; break;
        case 1: hp.s[u0].f1 = CAP + 5; break;
        case 2: hp.s[last_free].f0 = f0; break;
        case 3: hp.s[u0].f0 = hp.freehead; hp.freehead = u0; break;
        case 4: hp.s[f0].f0 = -1; break;
        case 5: hp.s[u1].mark = 1; break;
        case 6: hp.s[u1].fwd = u0; break;
        case 7: hp.s[u0].canary_hi ^= 0x00010000u; break;
        case 8: hp.s[u1].id = hp.s[u0].id; break;
        default: hp.roots[0] = f0; break;
        }
        unsigned m = verify_heap();
        printf("fault %2d %-36s -> ", t, faults[t].what);
        print_mask(m);
        printf("\n");
        CHECK(m == faults[t].expect);
        hp = snapshot;
        CHECK(verify_heap() == 0);
    }
    /* bit-flip campaign over the raw bytes of a slot */
    int detected = 0, silent = 0, per_kind[NERR] = {0};
    const int trials = 600;
    for (int t = 0; t < trials; t++) {
        int slot = (int)rnd_n(CAP), byte = (int)rnd_n(sizeof(Slot)), bit = (int)rnd_n(8);
        unsigned char *p = (unsigned char *)&hp.s[slot];
        p[byte] ^= (unsigned char)(1u << bit);
        unsigned m = verify_heap();
        if (m) {
            detected++;
            for (int e = 0; e < NERR; e++)
                per_kind[e] += (m >> e) & 1;
        } else {
            silent++;
        }
        hp = snapshot;
    }
    CHECK(verify_heap() == 0);
    printf("bit flips: %d trials, %d detected, %d silent\n", trials, detected, silent);
    for (int e = 0; e < NERR; e++)
        printf("  %-22s reported in %d trials\n", err_name[e], per_kind[e]);
    return 0;
}
