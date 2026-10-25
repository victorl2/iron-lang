/*
 * title: Radix heap for monotone keys
 * topic: data_structures
 * covers: radix heap, monotone priority queue, bucket by highest differing bit, redistribution counting, bit operations
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 4004;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    uint32_t key;
    int val;
} Item;

typedef struct {
    Item *v;
    int n, cap;
} Vec;

typedef struct {
    Vec b[33]; /* bucket 0: key == last; bucket i: highest differing bit is i-1 */
    uint32_t last;
    long size, moves, redistributions;
} Radix;

static int bucket_of(uint32_t last, uint32_t key) {
    uint32_t x = last ^ key;
    int i = 0;
    while (x) {
        i++;
        x >>= 1;
    }
    return i;
}

static void vpush(Vec *v, Item it) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->v = realloc(v->v, sizeof(Item) * (size_t)v->cap);
    }
    v->v[v->n++] = it;
}

static void rh_push(Radix *r, uint32_t key, int val) {
    check(key >= r->last, "monotone: key >= last extracted");
    Item it = {key, val};
    vpush(&r->b[bucket_of(r->last, key)], it);
    r->size++;
}

static Item rh_pop(Radix *r) {
    if (r->b[0].n == 0) {
        int i = 1;
        while (r->b[i].n == 0)
            i++;
        uint32_t mn = r->b[i].v[0].key;
        for (int k = 1; k < r->b[i].n; k++)
            if (r->b[i].v[k].key < mn)
                mn = r->b[i].v[k].key;
        r->last = mn;
        r->redistributions++;
        Vec old = r->b[i];
        r->b[i].v = NULL;
        r->b[i].n = r->b[i].cap = 0;
        for (int k = 0; k < old.n; k++) {
            vpush(&r->b[bucket_of(r->last, old.v[k].key)], old.v[k]);
            r->moves++;
        }
        free(old.v);
    }
    r->size--;
    return r->b[0].v[--r->b[0].n];
}

static void rh_check(const Radix *r) {
    long total = 0;
    for (int i = 0; i <= 32; i++)
        for (int k = 0; k < r->b[i].n; k++) {
            check(bucket_of(r->last, r->b[i].v[k].key) == i, "item in the right bucket");
            total++;
        }
    check(total == r->size, "size");
}

int main(void) {
    Radix r = {{{0}}, 0, 0, 0, 0};
    /* model: multiset as array of (key,val) */
    enum { CAP = 600 };
    Item model[CAP];
    int mn = 0, next_val = 0;
    long popsum = 0;
    uint32_t top_seen = 0;
    int pushed = 0, popped = 0;
    for (int op = 0; op < 30000; op++) {
        if ((rng() % 100 < 52 || mn == 0) && mn < CAP) {
            /* new keys are last + a delta drawn from a heavy-tailed distribution */
            unsigned span = 1u << (rng() % 20);
            uint32_t key = r.last + rng() % span;
            model[mn].key = key;
            model[mn].val = next_val;
            mn++;
            rh_push(&r, key, next_val++);
            pushed++;
        } else {
            int bi = 0;
            for (int i = 1; i < mn; i++)
                if (model[i].key < model[bi].key)
                    bi = i;
            uint32_t want = model[bi].key;
            Item it = rh_pop(&r);
            check(it.key == want, "popped key equals model minimum");
            int found = -1;
            for (int i = 0; i < mn; i++)
                if (model[i].key == it.key && model[i].val == it.val)
                    found = i;
            check(found >= 0, "popped pair was present");
            model[found] = model[--mn];
            popsum += it.key;
            top_seen = it.key;
            popped++;
        }
        if (op % 97 == 0)
            rh_check(&r);
    }
    printf("pushed=%d popped=%d size=%ld last=%u\n", pushed, popped, r.size, r.last);
    printf("redistributions=%ld moves=%ld popsum=%ld top=%u\n", r.redistributions, r.moves, popsum, top_seen);
    printf("bucket sizes:");
    for (int i = 0; i <= 32; i++)
        if (r.b[i].n)
            printf(" %d:%d", i, r.b[i].n);
    printf("\n");
    long dr = 0;
    uint32_t prev = 0;
    while (r.size > 0) {
        Item it = rh_pop(&r);
        check(it.key >= prev, "drain ascending");
        prev = it.key;
        dr++;
    }
    printf("drained=%ld max=%u\n", dr, prev);
    for (int i = 0; i <= 32; i++)
        free(r.b[i].v);
    return 0;
}
