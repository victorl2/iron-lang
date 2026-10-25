/*
 * title: Bump allocation, GC triggers and heap growth policies
 * topic: memory
 * covers: bump-pointer allocation failure as GC trigger, Cheney copy into resizable buffers, growth policies (fixed, proportional to live data, additive, target occupancy), per-policy collection and copy counts, phase-changing workload, reachability oracle
 * deps: libc
 */
#define SEED 0x5EED0015ULL
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



#define MINCAP 256
#define MAXCAP 12000
#define NROOTS 6
#define TMP NROOTS
#define FWD 0x40000000

enum { P_FIXED, P_DOUBLE_LIVE, P_HALF_MORE, P_ADDITIVE, P_OCCUPANCY };

static int *H;
static int cap, top, policy;
static int roots[NROOTS + 1]; /* the last one is the allocation temp */
static int gcs, copied, peak_cap, resizes, max_live;

/* Layout: [id][f0][f1][payload word when id is odd] */
static int osize(int idv) { return 3 + idv % 2; }

static int copy_obj(int *to, int *tfree, int a) {
    if (a < 0)
        return -1;
    if (H[a] & FWD)
        return H[a] & ~FWD;
    int n = osize(H[a]);
    int dst = *tfree;
    memcpy(&to[dst], &H[a], (size_t)n * sizeof(int));
    *tfree += n;
    H[a] = FWD | dst;
    return dst;
}

static int next_cap(int live, int need) {
    int c = cap;
    switch (policy) {
    case P_FIXED:
        break;
    case P_DOUBLE_LIVE:
        c = 2 * live;
        break;
    case P_HALF_MORE:
        c = live + live / 2;
        break;
    case P_ADDITIVE:
        if (cap - live < cap / 4)
            c = cap + 400;
        break;
    case P_OCCUPANCY:
        c = live * 100 / 35;
        break;
    }
    if (c < MINCAP)
        c = MINCAP;
    if (c < live + need + 32)
        c = live + need + 32;
    CHECK(c <= MAXCAP);
    return c;
}

static void verify(void) {
    unsigned char present[M_IDS];
    memset(present, 0, sizeof present);
    for (int a = 0; a < top; a += osize(H[a]))
        present[H[a]] = 1;
    model_verify(present, 1);
}

static void gc(int need) {
    int *to = malloc((size_t)cap * sizeof(int));
    CHECK(to != NULL);
    int tfree = 0, scan = 0;
    for (int i = 0; i <= NROOTS; i++)
        roots[i] = copy_obj(to, &tfree, roots[i]);
    while (scan < tfree) {
        for (int k = 1; k <= 2; k++)
            to[scan + k] = copy_obj(to, &tfree, to[scan + k]);
        scan += osize(to[scan]);
    }
    free(H);
    H = to;
    top = tfree;
    copied += tfree;
    if (tfree > max_live)
        max_live = tfree;
    gcs++;
    int nc = next_cap(tfree, need);
    if (nc != cap) {
        H = realloc(H, (size_t)nc * sizeof(int));
        CHECK(H != NULL);
        cap = nc;
        resizes++;
    }
    if (cap > peak_cap)
        peak_cap = cap;
    verify();
}

static int alloc(int idv) {
    int n = osize(idv);
    if (top + n > cap)
        gc(n);
    CHECK(top + n <= cap);
    int a = top;
    top += n;
    H[a] = idv;
    H[a + 1] = H[a + 2] = -1;
    if (n == 4)
        H[a + 3] = idv * 5;
    return a;
}

/* Prepends a fresh node to the chain hanging off root r. */
static void push_front(int r) {
    int idv = model_new();
    roots[TMP] = alloc(idv);
    M.root[TMP] = idv;
    H[roots[TMP] + 1] = roots[r];
    M.edge[idv][0] = M.root[r];
    roots[r] = roots[TMP];
    M.root[r] = idv;
    roots[TMP] = -1;
    M.root[TMP] = -1;
}

static void drop_after(int r, int depth) {
    int a = roots[r], m = M.root[r];
    for (int d = 0; d < depth && a >= 0; d++) {
        a = H[a + 1];
        m = M.edge[m][0];
    }
    if (a >= 0) {
        H[a + 1] = -1;
        M.edge[m][0] = -1;
    }
}

static void run_workload(void) {
    for (int i = 0; i < 300; i++) /* build a long-lived chain */
        push_front(0);
    for (int i = 0; i < 700; i++) { /* churn: short chains that die quickly */
        int r = 1 + (int)rnd_n(4);
        push_front(r);
        if (rnd_n(3) == 0)
            drop_after(r, (int)rnd_n(4));
        if (rnd_n(40) == 0) {
            roots[r] = -1;
            M.root[r] = -1;
        }
    }
    roots[0] = -1; /* the big structure dies */
    M.root[0] = -1;
    for (int i = 0; i < 500; i++) {
        int r = 1 + (int)rnd_n(4);
        push_front(r);
        if (rnd_n(2) == 0)
            drop_after(r, (int)rnd_n(3));
    }
    for (int i = 0; i < 150; i++) /* second, smaller build phase */
        push_front(5);
    for (int i = 0; i < 400; i++) {
        int r = 1 + (int)rnd_n(4);
        push_front(r);
        drop_after(r, 1 + (int)rnd_n(2));
    }
}

int main(void) {
    static const char *names[] = {"fixed 2600 words", "2 x live", "1.5 x live", "additive +400", "35% occupancy"};
    printf("%-18s %5s %9s %8s %8s %8s\n", "policy", "gcs", "copied", "peakcap", "finalcap", "resizes");
    int first_ids = -1;
    for (policy = 0; policy < 5; policy++) {
        rng_s = SEED;
        model_reset();
        cap = policy == P_FIXED ? 2600 : MINCAP;
        top = 0;
        H = malloc((size_t)cap * sizeof(int));
        CHECK(H != NULL);
        for (int i = 0; i <= NROOTS; i++)
            roots[i] = -1;
        gcs = copied = resizes = max_live = 0;
        peak_cap = cap;
        run_workload();
        gc(0); /* final collection so the checker sees the end state */
        if (first_ids < 0)
            first_ids = M.nids;
        CHECK(M.nids == first_ids); /* same workload under every policy */
        printf("%-18s %5d %9d %8d %8d %8d\n", names[policy], gcs, copied, peak_cap, cap, resizes);
        free(H);
        H = NULL;
    }
    printf("objects allocated per run: %d\n", first_ids);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
