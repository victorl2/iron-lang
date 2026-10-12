/*
 * title: Move vs copy API with ownership assertions
 * topic: memory
 * covers: explicit ownership transfer, moved-from state, deep copy, owner id checks
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int *data;
    int len;
    int owner; /* id of the owning component, 0 = nobody (moved-from) */
} Vec;

static int allocs, frees;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Vec vec_new(int owner, int len, int seed) {
    Vec v;
    v.data = malloc((size_t)len * sizeof(int));
    check(v.data != NULL, "alloc");
    allocs++;
    v.len = len;
    v.owner = owner;
    for (int i = 0; i < len; i++)
        v.data[i] = seed + i;
    return v;
}

/* Deep copy: the result is a new allocation owned by new_owner. */
static Vec vec_copy(const Vec *src, int new_owner) {
    check(src->owner != 0, "copy from moved-from");
    Vec v = vec_new(new_owner, src->len, 0);
    memcpy(v.data, src->data, (size_t)src->len * sizeof(int));
    return v;
}

/* Move: the destination takes the buffer, the source is emptied. */
static Vec vec_move(Vec *src, int from, int to) {
    check(src->owner == from, "move by non-owner");
    Vec v = *src;
    v.owner = to;
    src->data = NULL;
    src->len = 0;
    src->owner = 0;
    return v;
}

static void vec_drop(Vec *v, int by) {
    check(v->owner == by || v->owner == 0, "drop by non-owner");
    if (v->data) {
        free(v->data);
        frees++;
    }
    v->data = NULL;
    v->len = 0;
    v->owner = 0;
}

static long vec_sum(const Vec *v) {
    long s = 0;
    for (int i = 0; i < v->len; i++)
        s += v->data[i];
    return s;
}

enum { LOADER = 1, PARSER = 2, RENDERER = 3 };

static const char *owner_name(int o) {
    static const char *n[] = {"nobody", "loader", "parser", "renderer"};
    return n[o];
}

static void show(const char *label, const Vec *v) {
    printf("%-14s owner=%-8s len=%d sum=%ld\n", label, owner_name(v->owner), v->len,
           v->data ? vec_sum(v) : 0L);
}

int main(void) {
    Vec a = vec_new(LOADER, 6, 10);
    show("loaded", &a);

    Vec snapshot = vec_copy(&a, LOADER);
    a.data[0] = 999;
    show("a mutated", &a);
    show("snapshot", &snapshot);
    check(snapshot.data[0] == 10, "copy independent");

    Vec b = vec_move(&a, LOADER, PARSER);
    show("a after move", &a);
    show("b in parser", &b);
    check(a.data == NULL && a.owner == 0, "moved-from empty");

    b.data[1] *= 2;
    Vec c = vec_move(&b, PARSER, RENDERER);
    show("c in renderer", &c);

    /* chain of hand-offs, tracking that exactly one owner exists at all times */
    Vec chain = vec_new(LOADER, 3, 1);
    int owners[] = {LOADER, PARSER, RENDERER, PARSER, LOADER};
    for (int i = 0; i + 1 < 5; i++) {
        Vec next = vec_move(&chain, owners[i], owners[i + 1]);
        check(chain.data == NULL, "source cleared");
        chain = next;
        printf("hop %d -> %s\n", i, owner_name(chain.owner));
    }
    show("chain end", &chain);

    vec_drop(&a, LOADER); /* dropping a moved-from value is a harmless no-op */
    vec_drop(&snapshot, LOADER);
    vec_drop(&b, PARSER);
    vec_drop(&c, RENDERER);
    vec_drop(&chain, LOADER);
    printf("allocs=%d frees=%d\n", allocs, frees);
    check(allocs == frees, "balanced");
    return 0;
}
