/*
 * title: Iterator invalidation via version stamps
 * topic: memory
 * covers: container version counters, iterators borrowed from a container, stale iterator detection, safe re-seek by key
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int *items;
    int n, cap;
    unsigned version;
} IntVec;

typedef struct {
    const IntVec *v;
    int pos;
    unsigned stamp;
} Iter;

static int stale_uses;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void push(IntVec *v, int x) {
    if (v->n == v->cap) {
        int ncap = v->cap ? v->cap * 2 : 2;
        int *ni = malloc((size_t)ncap * sizeof(int));
        check(ni != NULL, "alloc");
        if (v->n)
            memcpy(ni, v->items, (size_t)v->n * sizeof(int));
        if (v->items) memset(v->items, 0xFF, (size_t)v->cap * sizeof(int)); /* poison old storage */
        free(v->items);
        v->items = ni;
        v->cap = ncap;
        v->version++; /* reallocation invalidates all iterators */
    }
    v->items[v->n++] = x;
}

static void erase_at(IntVec *v, int i) {
    memmove(&v->items[i], &v->items[i + 1], (size_t)(v->n - i - 1) * sizeof(int));
    v->n--;
    v->version++; /* positions shifted: invalidates iterators (conservative) */
}

static Iter begin(const IntVec *v) {
    Iter it = {v, 0, v->version};
    return it;
}

static int valid(const Iter *it) { return it->stamp == it->v->version; }

static int deref(Iter *it, int *out) {
    if (!valid(it)) {
        stale_uses++;
        return 0;
    }
    if (it->pos >= it->v->n)
        return 0;
    *out = it->v->items[it->pos];
    return 1;
}

static void advance(Iter *it) { it->pos++; }

/* Re-seek: recover an iterator after invalidation using the last key seen. */
static int reseek(Iter *it, int key) {
    it->stamp = it->v->version;
    for (int i = 0; i < it->v->n; i++)
        if (it->v->items[i] == key) {
            it->pos = i;
            return 1;
        }
    it->pos = it->v->n;
    return 0;
}

int main(void) {
    IntVec v = {0};
    for (int i = 1; i <= 3; i++)
        push(&v, i * 10);
    printf("n=%d cap=%d version=%u\n", v.n, v.cap, v.version);

    Iter it = begin(&v);
    int x;
    int sum = 0;
    while (deref(&it, &x)) {
        sum += x;
        advance(&it);
    }
    printf("clean traversal sum=%d, stale uses=%d\n", sum, stale_uses);

    /* mutate during traversal: erase the element just visited */
    it = begin(&v);
    int visited = 0, last = 0;
    while (deref(&it, &x)) {
        visited++;
        last = x;
        if (x == 20) {
            erase_at(&v, it.pos);
            printf("erased %d during traversal\n", x);
        }
        advance(&it);
    }
    printf("naive traversal visited %d, stopped after %d (stale uses=%d)\n", visited, last,
           stale_uses);
    check(stale_uses == 1, "stale iterator detected once");

    /* correct pattern: re-seek to the successor key after mutation */
    it = begin(&v);
    int seen[8], ns = 0;
    for (int round = 0; round < 8 && deref(&it, &x); round++) {
        seen[ns++] = x;
        if (x == 10)
            push(&v, 40); /* may realloc; version bumps if so */
        if (x == 10)
            push(&v, 50);
        if (!valid(&it)) {
            printf("iterator invalidated after visiting %d, re-seeking\n", x);
            check(reseek(&it, x), "key must still exist");
        }
        advance(&it);
    }
    printf("visited:");
    for (int i = 0; i < ns; i++)
        printf(" %d", seen[i]);
    printf("\nfinal n=%d cap=%d version=%u stale uses=%d\n", v.n, v.cap, v.version, stale_uses);
    free(v.items);
    return 0;
}
