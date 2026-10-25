/*
 * title: Insert-only lock-free skip list over a node pool
 * topic: concurrency
 * covers: multi-level next arrays, per-level CAS linking, deterministic tower heights, level-0 linearization, sorted traversal
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * A key becomes visible to lookups when it is CASed into level 0; its upper levels are express
 * lanes linked afterwards, bottom up. Because nothing is ever deleted, a failed CAS at any level
 * only means a neighbor was inserted; re-running find() refreshes the predecessor/successor
 * arrays and the link is retried. The tower height is a pure function of the key (trailing ones
 * of a hash), so the number of nodes per level is deterministic even though insertion order is not.
 *
 * Correctness argument: level 0 is a sorted linked list built by CAS-at-predecessor. Level i > 0
 * only contains nodes already in level 0 and in sorted position, so every level stays sorted
 * and a search that descends levels always lands at the correct level-0 position.
 */
enum { MAXH = 8, POOL = 6000, T = 4, PER = 1400, NIL = 0 };

typedef struct {
    uint32_t key;
    int height;
    atomic_uint next[MAXH];
} Node;

static Node pool[POOL];
static atomic_uint pool_top = 2; /* 0 = tail sentinel, 1 = head sentinel */

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int height_of(uint32_t key) {
    uint32_t h = hash32(key);
    int n = 1;
    while (n < MAXH && (h & 1u)) {
        n++;
        h >>= 1;
    }
    return n;
}

static void find(uint32_t key, unsigned preds[MAXH], unsigned succs[MAXH]) {
    unsigned pred = 1;
    for (int lvl = MAXH - 1; lvl >= 0; lvl--) {
        unsigned curr = atomic_load(&pool[pred].next[lvl]);
        while (pool[curr].key < key) {
            pred = curr;
            curr = atomic_load(&pool[pred].next[lvl]);
        }
        preds[lvl] = pred;
        succs[lvl] = curr;
    }
}

static int insert(uint32_t key) {
    unsigned preds[MAXH], succs[MAXH];
    int h = height_of(key);
    unsigned n = 0;
    for (;;) {
        find(key, preds, succs);
        if (pool[succs[0]].key == key)
            return 0; /* duplicate (the orphan node, if any, is simply wasted) */
        if (n == 0) {
            n = atomic_fetch_add(&pool_top, 1u);
            check(n < POOL, "pool");
            pool[n].key = key;
            pool[n].height = h;
        }
        atomic_store(&pool[n].next[0], succs[0]);
        unsigned expect = succs[0];
        if (atomic_compare_exchange_strong(&pool[preds[0]].next[0], &expect, n))
            break;
    }
    for (int lvl = 1; lvl < h; lvl++) {
        for (;;) {
            atomic_store(&pool[n].next[lvl], succs[lvl]);
            unsigned expect = succs[lvl];
            if (atomic_compare_exchange_strong(&pool[preds[lvl]].next[lvl], &expect, n))
                break;
            find(key, preds, succs);
        }
    }
    return 1;
}

static int contains(uint32_t key) {
    unsigned preds[MAXH], succs[MAXH];
    find(key, preds, succs);
    return pool[succs[0]].key == key;
}

static uint32_t key_of(int t, int i) {
    return 1u + hash32((uint32_t)(t % 2) * 100003u + (uint32_t)i) % 5000u;
}

static atomic_int inserted;

static void *worker(void *p) {
    int t = (int)(size_t)p;
    int ins = 0;
    for (int i = 0; i < PER; i++) {
        if (insert(key_of(t, i)))
            ins++;
        (void)contains(key_of(t, i / 2));
    }
    atomic_fetch_add(&inserted, ins);
    return NULL;
}

int main(void) {
    pool[0].key = UINT32_MAX;
    pool[1].key = 0;
    for (int l = 0; l < MAXH; l++) {
        atomic_store(&pool[0].next[l], NIL);
        atomic_store(&pool[1].next[l], NIL);
    }
    pool[0].height = pool[1].height = MAXH;
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    static unsigned char ref[5002];
    int distinct = 0;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < PER; i++) {
            uint32_t k = key_of(t, i);
            if (!ref[k]) {
                ref[k] = 1;
                distinct++;
            }
        }
    /* head sentinel's next[l] must be present at every level even for empty levels */
    int per_level[MAXH] = {0};
    for (int lvl = 0; lvl < MAXH; lvl++) {
        uint32_t prev = 0;
        for (unsigned n = atomic_load(&pool[1].next[lvl]); n != NIL; n = atomic_load(&pool[n].next[lvl])) {
            check(pool[n].key > prev, "level strictly sorted");
            check(pool[n].height > lvl, "node height covers level");
            check(ref[pool[n].key], "key was inserted");
            prev = pool[n].key;
            per_level[lvl]++;
        }
    }
    check(per_level[0] == distinct, "level 0 has every distinct key");
    check(atomic_load(&inserted) == distinct, "insert successes equal distinct");
    int expect_level[MAXH] = {0};
    for (uint32_t k = 1; k <= 5000; k++)
        if (ref[k])
            for (int l = 0; l < height_of(k); l++)
                expect_level[l]++;
    for (int l = 0; l < MAXH; l++)
        check(per_level[l] == expect_level[l], "level population matches the height function");
    for (uint32_t k = 1; k <= 5000; k++)
        check(contains(k) == (ref[k] != 0), "contains matches reference");
    printf("distinct keys=%d\n", distinct);
    for (int l = 0; l < MAXH; l++)
        printf("level %d: %d nodes\n", l, per_level[l]);
    return 0;
}
