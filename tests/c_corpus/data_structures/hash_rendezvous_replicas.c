/*
 * title: Rendezvous (highest random weight) hashing with replica sets
 * topic: data_structures
 * covers: rendezvous hashing, HRW score, top-k replica selection, minimal disruption, per-node load, failure handling
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
enum { NODES = 10, KEYS = 20000, R = 3 };

static uint64_t score(uint32_t key, int node) { return mix64(((uint64_t)key << 32) ^ (uint64_t)(node * 0x9E3779B9u + 77)); }

/* fills out[0..R) with the R highest-scoring alive nodes, best first */
static void replicas(uint32_t key, const uint8_t *alive, int *out) {
    uint64_t best[R];
    for (int i = 0; i < R; i++) {
        out[i] = -1;
        best[i] = 0;
    }
    for (int n = 0; n < NODES; n++) {
        if (!alive[n])
            continue;
        uint64_t s = score(key, n);
        for (int i = 0; i < R; i++)
            if (out[i] < 0 || s > best[i] || (s == best[i] && n < out[i])) {
                for (int j = R - 1; j > i; j--) {
                    out[j] = out[j - 1];
                    best[j] = best[j - 1];
                }
                out[i] = n;
                best[i] = s;
                break;
            }
    }
}

/* reference: full sort of all alive nodes by (score desc, id asc) */
static void replicas_sorted(uint32_t key, const uint8_t *alive, int *out) {
    int ids[NODES], n = 0;
    for (int i = 0; i < NODES; i++)
        if (alive[i])
            ids[n++] = i;
    for (int i = 1; i < n; i++) {
        int x = ids[i], j = i - 1;
        while (j >= 0 && (score(key, ids[j]) < score(key, x) || (score(key, ids[j]) == score(key, x) && ids[j] > x))) {
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = x;
    }
    for (int i = 0; i < R; i++)
        out[i] = i < n ? ids[i] : -1;
}

int main(void) {
    uint8_t alive[NODES];
    memset(alive, 1, sizeof alive);
    static int prim[KEYS], rep[KEYS][R];
    long load[NODES] = {0}, rload[NODES] = {0};
    for (uint32_t k = 0; k < KEYS; k++) {
        int r2[R];
        replicas(k, alive, rep[k]);
        replicas_sorted(k, alive, r2);
        for (int i = 0; i < R; i++)
            check(rep[k][i] == r2[i], "top-k equals full sort");
        prim[k] = rep[k][0];
        load[prim[k]]++;
        for (int i = 0; i < R; i++)
            rload[rep[k][i]]++;
    }
    printf("primary load per node:");
    long mn = KEYS, mx = 0;
    for (int n = 0; n < NODES; n++) {
        printf(" %ld", load[n]);
        if (load[n] < mn)
            mn = load[n];
        if (load[n] > mx)
            mx = load[n];
    }
    printf("\nmin=%ld max=%ld ratio=%.3f\n", mn, mx, (double)mx / (double)mn);
    printf("replica load per node:");
    for (int n = 0; n < NODES; n++)
        printf(" %ld", rload[n]);
    printf("\n");
    /* fail node 4: primaries move only if node 4 was primary; replica sets change only if it was in the set */
    alive[4] = 0;
    long prim_moved = 0, set_changed = 0, in_set = 0, promoted = 0;
    for (uint32_t k = 0; k < KEYS; k++) {
        int nr[R];
        replicas(k, alive, nr);
        int had4 = 0;
        for (int i = 0; i < R; i++)
            had4 |= rep[k][i] == 4;
        in_set += had4;
        int changed = 0;
        for (int i = 0; i < R; i++)
            changed |= nr[i] != rep[k][i];
        set_changed += changed;
        check(changed == had4, "replica set changes iff the failed node was in it");
        if (nr[0] != prim[k]) {
            check(prim[k] == 4, "primary moves only when the failed node was primary");
            prim_moved++;
            check(nr[0] == rep[k][1], "old first replica is promoted");
            promoted++;
        }
    }
    printf("fail node 4: keys with node 4 in replica set=%ld changed sets=%ld primaries moved=%ld promoted replicas=%ld\n", in_set,
           set_changed, prim_moved, promoted);
    /* recover: node 4 returns, everything goes back exactly */
    alive[4] = 1;
    long diff = 0;
    for (uint32_t k = 0; k < KEYS; k++) {
        int nr[R];
        replicas(k, alive, nr);
        for (int i = 0; i < R; i++)
            diff += nr[i] != rep[k][i];
    }
    check(diff == 0, "recovery restores placement");
    printf("recovery differences=%ld\n", diff);
    return 0;
}
