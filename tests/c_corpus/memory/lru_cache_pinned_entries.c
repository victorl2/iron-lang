/*
 * title: LRU cache with pinned refcounted entries
 * topic: memory
 * covers: cache eviction, pins prevent eviction, deferred free of evicted-but-pinned entries, doubly linked LRU list, hash chains
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPACITY 4
#define NBUCKET 8

typedef struct Entry Entry;
struct Entry {
    int key;
    int value;
    int pins;     /* handles held by users */
    int in_cache; /* still indexed by the cache */
    Entry *prev, *next;  /* LRU list: head = most recent */
    Entry *chain;        /* hash bucket chain */
};

typedef struct {
    Entry *buckets[NBUCKET];
    Entry *head, *tail;
    int size;
    int hits, misses, evictions, orphaned_frees;
} Cache;

static int live_entries;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void lru_unlink(Cache *c, Entry *e) {
    if (e->prev)
        e->prev->next = e->next;
    else
        c->head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        c->tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_front(Cache *c, Entry *e) {
    e->prev = NULL;
    e->next = c->head;
    if (c->head)
        c->head->prev = e;
    c->head = e;
    if (!c->tail)
        c->tail = e;
}

static void bucket_remove(Cache *c, Entry *e) {
    Entry **pp = &c->buckets[(unsigned)e->key % NBUCKET];
    while (*pp != e)
        pp = &(*pp)->chain;
    *pp = e->chain;
}

static void unpin(Cache *c, Entry *e) {
    check(e->pins > 0, "unpin underflow");
    e->pins--;
    if (e->pins == 0 && !e->in_cache) {
        free(e); /* evicted while pinned: last unpin frees it */
        live_entries--;
        c->orphaned_frees++;
    }
}

/* Evict the least recently used entry that is not pinned. Returns 0 if all are pinned. */
static int evict_one(Cache *c) {
    for (Entry *e = c->tail; e; e = e->prev) {
        if (e->pins == 0) {
            lru_unlink(c, e);
            bucket_remove(c, e);
            e->in_cache = 0;
            free(e);
            live_entries--;
            c->size--;
            c->evictions++;
            return 1;
        }
    }
    return 0;
}

static Entry *cache_get(Cache *c, int key) {
    for (Entry *e = c->buckets[(unsigned)key % NBUCKET]; e; e = e->chain)
        if (e->key == key) {
            c->hits++;
            lru_unlink(c, e);
            lru_push_front(c, e);
            e->pins++;
            return e;
        }
    c->misses++;
    if (c->size >= CAPACITY && !evict_one(c)) {
        /* everything pinned: over-commit by making an orphan entry that lives only as long as its pin */
        Entry *o = calloc(1, sizeof *o);
        check(o != NULL, "alloc");
        o->key = key;
        o->value = key * key;
        o->pins = 1;
        o->in_cache = 0;
        live_entries++;
        return o;
    }
    Entry *e = calloc(1, sizeof *e);
    check(e != NULL, "alloc");
    e->key = key;
    e->value = key * key;
    e->pins = 1;
    e->in_cache = 1;
    unsigned b = (unsigned)key % NBUCKET;
    e->chain = c->buckets[b];
    c->buckets[b] = e;
    lru_push_front(c, e);
    c->size++;
    live_entries++;
    return e;
}

static void print_lru(const Cache *c) {
    printf("  lru:");
    for (const Entry *e = c->head; e; e = e->next)
        printf(" %d%s", e->key, e->pins ? "*" : "");
    printf(" (size %d)\n", c->size);
}

int main(void) {
    Cache c;
    memset(&c, 0, sizeof c);
    Entry *pinned[4];
    int np = 0;

    /* touch keys and release immediately */
    int seq[] = {1, 2, 3, 4, 1, 5, 2};
    for (int i = 0; i < 7; i++) {
        Entry *e = cache_get(&c, seq[i]);
        unpin(&c, e);
    }
    printf("after simple accesses: hits=%d misses=%d evictions=%d\n", c.hits, c.misses, c.evictions);
    print_lru(&c);

    /* pin all current entries, then request new keys: nothing evictable */
    int keys[CAPACITY];
    int nk = 0;
    for (Entry *e = c.head; e; e = e->next)
        keys[nk++] = e->key;
    for (int i = nk - 1; i >= 0; i--) /* oldest first, so the LRU order is kept */
        pinned[np++] = cache_get(&c, keys[i]);
    printf("pinned %d entries\n", np);
    print_lru(&c);
    Entry *orphan1 = cache_get(&c, 9);
    Entry *orphan2 = cache_get(&c, 9);
    printf("orphans: value=%d, in_cache=%d, misses=%d, size=%d\n", orphan1->value, orphan1->in_cache,
           c.misses, c.size);
    check(orphan1 != orphan2, "orphans are separate uncached entries");
    unpin(&c, orphan1);
    unpin(&c, orphan2);
    printf("orphaned frees so far: %d, live entries=%d\n", c.orphaned_frees, live_entries);

    /* unpin two of the pinned entries and request new keys: only unpinned ones get evicted */
    unpin(&c, pinned[0]);
    unpin(&c, pinned[2]);
    Entry *n1 = cache_get(&c, 20);
    unpin(&c, n1);
    Entry *n2 = cache_get(&c, 21);
    unpin(&c, n2);
    print_lru(&c);
    printf("evictions=%d hits=%d misses=%d\n", c.evictions, c.hits, c.misses);
    check(pinned[1]->in_cache && pinned[3]->in_cache, "still-pinned entries survive");

    unpin(&c, pinned[1]);
    unpin(&c, pinned[3]);
    while (c.head) {
        Entry *e = c.head;
        check(e->pins == 0, "no pins at end");
        lru_unlink(&c, e);
        bucket_remove(&c, e);
        free(e);
        live_entries--;
        c.size--;
    }
    check(live_entries == 0 && c.size == 0, "leak");
    printf("final live=%d\n", live_entries);
    return 0;
}
