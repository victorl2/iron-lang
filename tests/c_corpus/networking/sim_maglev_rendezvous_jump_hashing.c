/*
 * title: Rendezvous, jump and Maglev consistent hashing compared
 * topic: networking
 * covers: highest random weight hashing, jump consistent hash, Maglev lookup table population, key movement on node change, load balance
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

enum { NKEYS = 20000, MAXN = 12, MTAB = 1009 }; /* 1009 is prime */

static uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
static uint64_t hash2(uint64_t a, uint64_t b) { return mix(a * 0x100000001b3ull ^ mix(b)); }
static uint64_t node_id(int n) { return 0x5eed0000ull + (uint64_t)n * 7919ull; }

/* rendezvous: every (key, node) pair gets a score, highest wins */
static int rendezvous(uint64_t key, const int *nodes, int n) {
    int best = -1;
    uint64_t bs = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = hash2(key, node_id(nodes[i]));
        if (best < 0 || s > bs || (s == bs && nodes[i] < best)) { best = nodes[i]; bs = s; }
    }
    return best;
}

/* jump consistent hash: buckets are 0..n-1, only growth at the end is cheap */
static int jump_hash(uint64_t key, int nbuckets) {
    int64_t b = -1, j = 0;
    while (j < nbuckets) {
        b = j;
        key = key * 2862933555777941757ull + 1;
        j = (int64_t)((double)(b + 1) * ((double)(1ll << 31) / (double)((key >> 33) + 1)));
    }
    return (int)b;
}

/* Maglev: each node walks its own permutation of table slots and claims free ones in turn */
static int maglev_build(int *table, const int *nodes, int n) {
    int next[MAXN], offset[MAXN], skip[MAXN];
    for (int i = 0; i < MAXN; i++) next[i] = 0;
    for (int i = 0; i < n; i++) {
        offset[i] = (int)(hash2(node_id(nodes[i]), 1) % MTAB);
        skip[i] = 1 + (int)(hash2(node_id(nodes[i]), 2) % (MTAB - 1));
    }
    for (int i = 0; i < MTAB; i++) table[i] = -1;
    int filled = 0;
    while (filled < MTAB) {
        for (int i = 0; i < n && filled < MTAB; i++) {
            int c;
            do {
                c = (int)(((uint64_t)offset[i] + (uint64_t)next[i] * (uint64_t)skip[i]) % MTAB);
                next[i]++;
            } while (table[c] >= 0);
            table[c] = nodes[i];
            filled++;
        }
    }
    return filled;
}
static int maglev_lookup(const int *table, uint64_t key) { return table[hash2(key, 99) % MTAB]; }

static void balance(const char *name, const int *owner, int nn, const int *nodes) {
    int cnt[MAXN + 4] = {0};
    for (int k = 0; k < NKEYS; k++) cnt[owner[k]]++;
    int mx = 0, mn = NKEYS;
    for (int i = 0; i < nn; i++) { if (cnt[nodes[i]] > mx) mx = cnt[nodes[i]]; if (cnt[nodes[i]] < mn) mn = cnt[nodes[i]]; }
    printf("  %-10s load min=%d max=%d (ideal %d) max/ideal=%d%%\n", name, mn, mx, NKEYS / nn, mx * 100 / (NKEYS / nn));
}

int main(void) {
    static int keys_own[3][NKEYS], keys_after[3][NKEYS];
    static uint64_t keys[NKEYS];
    for (int k = 0; k < NKEYS; k++) keys[k] = mix((uint64_t)k * 31 + 5);
    int nodes8[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    int nodes7[7] = {0, 1, 2, 3, 4, 5, 6};   /* node 7 removed */
    int nodes_wo3[7] = {0, 1, 2, 4, 5, 6, 7}; /* node 3 removed (middle) */
    static int tab8[MTAB], tab7[MTAB], tabw3[MTAB];
    check(maglev_build(tab8, nodes8, 8) == MTAB, "maglev fills table");
    maglev_build(tab7, nodes7, 7);
    maglev_build(tabw3, nodes_wo3, 7);
    int slots[MAXN] = {0};
    for (int i = 0; i < MTAB; i++) slots[tab8[i]]++;
    int smin = MTAB, smax = 0;
    for (int i = 0; i < 8; i++) { if (slots[i] < smin) smin = slots[i]; if (slots[i] > smax) smax = slots[i]; }
    printf("maglev table slots per node: min=%d max=%d (spread %d)\n", smin, smax, smax - smin);
    check(smax - smin <= 1, "maglev slots differ by at most one");

    printf("8 nodes, %d keys\n", NKEYS);
    for (int k = 0; k < NKEYS; k++) {
        keys_own[0][k] = rendezvous(keys[k], nodes8, 8);
        keys_own[1][k] = jump_hash(keys[k], 8);
        keys_own[2][k] = maglev_lookup(tab8, keys[k]);
    }
    balance("rendezvous", keys_own[0], 8, nodes8);
    balance("jump", keys_own[1], 8, nodes8);
    balance("maglev", keys_own[2], 8, nodes8);

    /* remove the last node (7), then a middle node (3) */
    for (int scenario = 0; scenario < 2; scenario++) {
        const int *nn = scenario == 0 ? nodes7 : nodes_wo3;
        const int *tabn = scenario == 0 ? tab7 : tabw3;
        int removed = scenario == 0 ? 7 : 3;
        printf("remove node %d:\n", removed);
        for (int k = 0; k < NKEYS; k++) {
            keys_after[0][k] = rendezvous(keys[k], nn, 7);
            keys_after[2][k] = maglev_lookup(tabn, keys[k]);
        }
        const char *names[3] = {"rendezvous", "jump", "maglev"};
        for (int m = 0; m < 3; m++) {
            if (m == 1) {
                if (scenario == 1) { printf("  %-10s cannot remove a middle bucket (renumbering moves keys)\n", names[m]); continue; }
                for (int k = 0; k < NKEYS; k++) keys_after[1][k] = jump_hash(keys[k], 7);
            }
            int moved = 0, forced = 0, collateral = 0;
            for (int k = 0; k < NKEYS; k++) {
                if (keys_own[m][k] == removed) forced++;
                if (keys_own[m][k] != keys_after[m][k]) {
                    moved++;
                    if (keys_own[m][k] != removed) collateral++;
                }
            }
            printf("  %-10s moved=%5d (%2d%%) of which forced=%5d collateral=%5d\n", names[m], moved, moved * 100 / NKEYS, forced,
                   collateral);
            if (m == 0) check(collateral == 0, "rendezvous moves only the removed node's keys");
            if (m == 1) check(collateral == 0, "jump hash moves nothing but the last bucket's keys");
            if (m == 2) check(collateral * 100 / NKEYS < 15, "maglev disruption stays small");
            check(moved >= forced, "at least the removed node's keys move");
        }
    }
    /* growth 8 -> 9 with jump hash: keys only move to the new bucket */
    int to_new = 0, elsewhere = 0;
    for (int k = 0; k < NKEYS; k++) {
        int a = jump_hash(keys[k], 8), b = jump_hash(keys[k], 9);
        if (a != b) { if (b == 8) to_new++; else elsewhere++; }
    }
    printf("jump 8->9 buckets: %d keys moved, all to the new bucket=%s (ideal %d)\n", to_new, elsewhere == 0 ? "yes" : "no", NKEYS / 9);
    check(elsewhere == 0, "jump hash growth only feeds the new bucket");
    return 0;
}
