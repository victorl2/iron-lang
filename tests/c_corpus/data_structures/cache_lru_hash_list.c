/*
 * title: LRU cache from a hash table and an intrusive doubly linked list
 * topic: data_structures
 * covers: LRU cache, chained hash table, index-linked doubly linked list, get/put/delete, capacity eviction, timestamp oracle, hit ratio
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP 32
#define HB 64        /* hash buckets */
#define KEYS 200

static unsigned long long rs = 0x1F7E0CAC4EULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int key, val, prev, next, hnext; } Slot;
typedef struct {
    Slot s[CAP + 1];          /* slot CAP is the list sentinel */
    int bucket[HB];
    int free_head, size;
    long hits, misses, evictions;
} Lru;

static unsigned hash(int k) { unsigned x = (unsigned)k * 2654435761u; return (x >> 7) % HB; }
static void lru_init(Lru *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < HB; i++) c->bucket[i] = -1;
    for (int i = 0; i < CAP; i++) c->s[i].next = i + 1 < CAP ? i + 1 : -1;
    c->free_head = 0;
    c->s[CAP].prev = c->s[CAP].next = CAP;
}
static void unlink_node(Lru *c, int i) { c->s[c->s[i].prev].next = c->s[i].next; c->s[c->s[i].next].prev = c->s[i].prev; }
static void push_front(Lru *c, int i) {   /* MRU is sentinel.next */
    int f = c->s[CAP].next;
    c->s[i].prev = CAP; c->s[i].next = f; c->s[f].prev = i; c->s[CAP].next = i;
}
static int lookup(const Lru *c, int key) {
    for (int i = c->bucket[hash(key)]; i >= 0; i = c->s[i].hnext) if (c->s[i].key == key) return i;
    return -1;
}
static void hash_remove(Lru *c, int i) {
    int *p = &c->bucket[hash(c->s[i].key)];
    while (*p != i) p = &c->s[*p].hnext;
    *p = c->s[i].hnext;
}
static int lru_get(Lru *c, int key, int *val) {
    int i = lookup(c, key);
    if (i < 0) { c->misses++; return 0; }
    c->hits++; unlink_node(c, i); push_front(c, i); *val = c->s[i].val;
    return 1;
}
static void lru_put(Lru *c, int key, int val) {
    int i = lookup(c, key);
    if (i >= 0) { c->s[i].val = val; unlink_node(c, i); push_front(c, i); return; }
    if (c->free_head >= 0) { i = c->free_head; c->free_head = c->s[i].next; c->size++; }
    else {
        i = c->s[CAP].prev;           /* LRU victim */
        unlink_node(c, i); hash_remove(c, i); c->evictions++;
    }
    c->s[i].key = key; c->s[i].val = val;
    int *b = &c->bucket[hash(key)];
    c->s[i].hnext = *b; *b = i;
    push_front(c, i);
}
static int lru_del(Lru *c, int key) {
    int i = lookup(c, key);
    if (i < 0) return 0;
    unlink_node(c, i); hash_remove(c, i);
    c->s[i].next = c->free_head; c->free_head = i; c->size--;
    return 1;
}

/* oracle: entries carry a last-use tick; victim = smallest tick */
typedef struct { int used, val; long tick; } Ref;
static Ref ref[KEYS]; static int ref_size; static long tick;
static void ref_touch(int k) { ref[k].tick = ++tick; }
static void ref_put(int k, int v) {
    if (!ref[k].used) {
        if (ref_size == CAP) {
            int victim = -1;
            for (int i = 0; i < KEYS; i++) if (ref[i].used && (victim < 0 || ref[i].tick < ref[victim].tick)) victim = i;
            ref[victim].used = 0; ref_size--;
        }
        ref[k].used = 1; ref_size++;
    }
    ref[k].val = v; ref_touch(k);
}

static int pick_key(void) {
    unsigned r = rnd() % 100;
    if (r < 70) return (int)(rnd() % 24);            /* hot set */
    if (r < 90) return 24 + (int)(rnd() % 76);       /* warm */
    return 100 + (int)(rnd() % 100);                 /* cold */
}

int main(void) {
    Lru c; lru_init(&c);
    long rhits = 0, rmiss = 0, dels = 0, puts = 0;
    for (int step = 0; step < 30000; step++) {
        int k = pick_key(); unsigned op = rnd() % 20;
        if (op < 12) {
            int v = 0, got = lru_get(&c, k, &v);
            check(got == ref[k].used, "get presence");
            if (got) { check(v == ref[k].val, "get value"); ref_touch(k); rhits++; }
            else { rmiss++; int nv = step; lru_put(&c, k, nv); ref_put(k, nv); puts++; }
        } else if (op < 18) {
            int v = step * 7; lru_put(&c, k, v); ref_put(k, v); puts++;
        } else {
            int r = lru_del(&c, k);
            check(r == ref[k].used, "delete presence");
            if (r) { ref[k].used = 0; ref_size--; dels++; }
        }
        check(c.size == ref_size, "size");
    }
    check(c.hits == rhits && c.misses == rmiss, "counters");
    /* recency order from the list equals oracle order by tick */
    int n = 0, prev_key = -1; long last_tick = 1L << 60;
    for (int i = c.s[CAP].next; i != CAP; i = c.s[i].next) {
        check(ref[c.s[i].key].used, "list entry in oracle");
        check(ref[c.s[i].key].tick < last_tick, "recency order descending");
        last_tick = ref[c.s[i].key].tick; n++; prev_key = c.s[i].key;
    }
    check(n == c.size, "list length");
    printf("hits=%ld misses=%ld hit_ratio=%ld.%02ld%% evictions=%ld puts=%ld deletes=%ld\n",
           c.hits, c.misses, c.hits * 100 / (c.hits + c.misses), c.hits * 10000 / (c.hits + c.misses) % 100, c.evictions, puts, dels);
    printf("size=%d LRU key=%d; MRU 8 keys:", c.size, prev_key);
    int shown = 0;
    for (int i = c.s[CAP].next; i != CAP && shown < 8; i = c.s[i].next, shown++) printf(" %d", c.s[i].key);
    printf("\n");
    return 0;
}
