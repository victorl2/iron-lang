/*
 * title: Harris-Michael linked list over pool indices with mark bits
 * topic: concurrency
 * covers: logical deletion by mark bit in next word, physical unlink during find, sentinels, per-key net accounting
 * deps: libc, pthread
 */
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * next words hold (index << 1) | mark. remove() first CASes the mark bit onto the victim's own
 * next word (logical delete: from then on nobody can link a node after it), then tries to
 * unlink it. find() snips every marked node it passes. Nodes are never recycled, so indices do
 * not suffer ABA; the mark bit is what makes insert-after-a-deleted-node fail its CAS.
 *
 * Correctness argument (oracle): for every key, successful inserts minus successful removes must
 * end at 1 if the key is in the list and 0 if it is not. Only one insert can win while the key
 * is present and only one remove while it is absent, so the net count is always 0 or 1.
 */
enum { KEYS = 12, T = 5, OPS = 5000, POOL = 40000 };

typedef struct {
    int key;
    atomic_uint next;
} Node;

static Node pool[POOL];
static atomic_uint pool_top = 2; /* 0 = head sentinel, 1 = tail sentinel */
static atomic_int net[KEYS];
static atomic_long snips;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned idx_of(unsigned w) { return w >> 1; }
static unsigned mark_of(unsigned w) { return w & 1u; }
static unsigned mk(unsigned idx, unsigned mark) { return (idx << 1) | mark; }

static void find(int key, unsigned *ppred, unsigned *pcurr) {
retry:;
    unsigned pred = 0;
    unsigned curr = idx_of(atomic_load(&pool[pred].next));
    for (;;) {
        unsigned sw = atomic_load(&pool[curr].next);
        while (mark_of(sw)) {
            unsigned expect = mk(curr, 0);
            if (!atomic_compare_exchange_strong(&pool[pred].next, &expect, mk(idx_of(sw), 0)))
                goto retry;
            atomic_fetch_add_explicit(&snips, 1, memory_order_relaxed);
            curr = idx_of(sw);
            sw = atomic_load(&pool[curr].next);
        }
        if (pool[curr].key >= key) {
            *ppred = pred;
            *pcurr = curr;
            return;
        }
        pred = curr;
        curr = idx_of(sw);
    }
}

static int list_insert(int key) {
    unsigned n = atomic_fetch_add(&pool_top, 1u);
    check(n < POOL, "pool");
    pool[n].key = key;
    for (;;) {
        unsigned pred, curr;
        find(key, &pred, &curr);
        if (pool[curr].key == key)
            return 0;
        atomic_store(&pool[n].next, mk(curr, 0));
        unsigned expect = mk(curr, 0);
        if (atomic_compare_exchange_strong(&pool[pred].next, &expect, mk(n, 0)))
            return 1;
    }
}

static int list_remove(int key) {
    for (;;) {
        unsigned pred, curr;
        find(key, &pred, &curr);
        if (pool[curr].key != key)
            return 0;
        unsigned sw = atomic_load(&pool[curr].next);
        if (mark_of(sw))
            continue; /* someone else is deleting it: find will snip it */
        if (atomic_compare_exchange_strong(&pool[curr].next, &sw, sw | 1u)) {
            unsigned expect = mk(curr, 0);
            atomic_compare_exchange_strong(&pool[pred].next, &expect, mk(idx_of(sw), 0));
            return 1;
        }
    }
}

static int list_contains(int key) {
    unsigned curr = idx_of(atomic_load(&pool[0].next));
    while (pool[curr].key < key)
        curr = idx_of(atomic_load(&pool[curr].next));
    return pool[curr].key == key && !mark_of(atomic_load(&pool[curr].next));
}

static void *worker(void *p) {
    unsigned s = 0xC0FFEEu + 977u * (unsigned)(size_t)p;
    for (int i = 0; i < OPS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int key = (int)((s >> 8) % KEYS);
        switch (s & 3u) {
        case 0:
        case 1:
            if (list_insert(key))
                atomic_fetch_add(&net[key], 1);
            break;
        case 2:
            if (list_remove(key))
                atomic_fetch_sub(&net[key], 1);
            break;
        default:
            (void)list_contains(key);
            break;
        }
    }
    return NULL;
}

int main(void) {
    pool[0].key = INT_MIN;
    pool[1].key = INT_MAX;
    atomic_store(&pool[0].next, mk(1, 0));
    atomic_store(&pool[1].next, mk(1, 0));
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    /* sweep marked leftovers, then verify */
    {
        unsigned p, c;
        find(INT_MAX - 1, &p, &c); /* walks the whole list and snips every marked node */
    }
    int present[KEYS] = {0};
    int prev = INT_MIN, count = 0;
    for (unsigned n = idx_of(atomic_load(&pool[0].next)); n != 1; n = idx_of(atomic_load(&pool[n].next))) {
        check(!mark_of(atomic_load(&pool[n].next)), "no marked node reachable after sweep");
        check(pool[n].key > prev, "strictly sorted");
        prev = pool[n].key;
        if (pool[n].key >= 0 && pool[n].key < KEYS) {
            present[pool[n].key] = 1;
            count++;
        }
    }
    for (int k = 0; k < KEYS; k++) {
        int nk = atomic_load(&net[k]);
        check(nk == 0 || nk == 1, "net is 0 or 1");
        check(nk == present[k], "net matches presence");
        check(list_contains(k) == present[k], "contains matches presence");
    }
    check(count >= 0 && count <= KEYS, "live count range");
    /* the surviving set depends on scheduling, so finish with a deterministic phase */
    int ins_new = 0, rem_ok = 0;
    for (int k = 0; k < KEYS; k++)
        ins_new += list_insert(k);
    check(ins_new == KEYS - count, "inserting every key adds exactly the missing ones");
    for (int k = 1; k < KEYS; k += 2)
        rem_ok += list_remove(k);
    check(rem_ok == KEYS / 2, "odd keys removed");
    int evens = 0;
    for (int k = 0; k < KEYS; k++)
        evens += list_contains(k);
    check(evens == KEYS / 2 && !list_remove(1) && !list_insert(2), "final even set");
    printf("keys=%d, after the deterministic phase %d live keys (the even ones)\n", KEYS, evens);
    printf("net insert-minus-remove equals presence for every key: yes\n");
    printf("list strictly sorted: yes\n");
    return 0;
}
