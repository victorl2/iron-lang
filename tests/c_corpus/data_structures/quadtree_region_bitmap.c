/*
 * title: Region quadtree over a bitmap with merging and set operations
 * topic: data_structures
 * covers: region quadtree, uniform block compression, split on write, merge on write, union and intersection, black area, brute force bitmap
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define SIDE 32 /* 2^5 */

/* leaf: kind 0 (white) or 1 (black); kind 2 = gray with four children NW NE SW SE */
typedef struct Q {
    int kind;
    struct Q *kid[4];
} Q;

static int live_nodes;

static Q *leaf(int color) {
    Q *q = calloc(1, sizeof *q);
    check(q != NULL, "alloc");
    q->kind = color;
    live_nodes++;
    return q;
}

static void destroy(Q *q) {
    if (!q)
        return;
    for (int i = 0; i < 4; i++)
        destroy(q->kid[i]);
    free(q);
    live_nodes--;
}

static Q *from_bitmap(const unsigned char *bm, int x, int y, int size) {
    int first = bm[y * SIDE + x], uniform = 1;
    for (int j = 0; j < size && uniform; j++)
        for (int i = 0; i < size; i++)
            if (bm[(y + j) * SIDE + x + i] != first) {
                uniform = 0;
                break;
            }
    if (uniform)
        return leaf(first);
    Q *q = leaf(2);
    int h = size / 2;
    q->kid[0] = from_bitmap(bm, x, y, h);
    q->kid[1] = from_bitmap(bm, x + h, y, h);
    q->kid[2] = from_bitmap(bm, x, y + h, h);
    q->kid[3] = from_bitmap(bm, x + h, y + h, h);
    return q;
}

static int get(const Q *q, int x, int y, int size) {
    while (q->kind == 2) {
        size /= 2;
        int idx = (x >= size ? 1 : 0) + (y >= size ? 2 : 0);
        if (x >= size)
            x -= size;
        if (y >= size)
            y -= size;
        q = q->kid[idx];
    }
    return q->kind;
}

/* returns the (possibly new) subtree; splits a leaf on demand and merges equal children back */
static Q *set(Q *q, int x, int y, int size, int color) {
    if (q->kind == color)
        return q;
    if (size == 1) {
        q->kind = color;
        return q;
    }
    if (q->kind != 2) {
        int old = q->kind;
        q->kind = 2;
        for (int i = 0; i < 4; i++)
            q->kid[i] = leaf(old);
    }
    int h = size / 2;
    int idx = (x >= h ? 1 : 0) + (y >= h ? 2 : 0);
    q->kid[idx] = set(q->kid[idx], x >= h ? x - h : x, y >= h ? y - h : y, h, color);
    int c0 = q->kid[0]->kind;
    if (c0 != 2 && q->kid[1]->kind == c0 && q->kid[2]->kind == c0 && q->kid[3]->kind == c0) {
        for (int i = 0; i < 4; i++) {
            destroy(q->kid[i]);
            q->kid[i] = NULL;
        }
        q->kind = c0;
    }
    return q;
}

static long area(const Q *q, int size) {
    if (q->kind != 2)
        return q->kind ? (long)size * size : 0;
    long s = 0;
    for (int i = 0; i < 4; i++)
        s += area(q->kid[i], size / 2);
    return s;
}

static Q *clone(const Q *q) {
    Q *c = leaf(q->kind);
    for (int i = 0; i < 4; i++)
        if (q->kid[i])
            c->kid[i] = clone(q->kid[i]);
    return c;
}

/* op 0 = union, 1 = intersection */
static Q *combine(const Q *a, const Q *b, int op) {
    if (a->kind != 2 && b->kind != 2) {
        int v = op == 0 ? (a->kind | b->kind) : (a->kind & b->kind);
        return leaf(v);
    }
    if (a->kind != 2) {
        int absorbs = op == 0 ? a->kind == 1 : a->kind == 0;
        return absorbs ? leaf(a->kind) : clone(b);
    }
    if (b->kind != 2) {
        int absorbs = op == 0 ? b->kind == 1 : b->kind == 0;
        return absorbs ? leaf(b->kind) : clone(a);
    }
    Q *q = leaf(2);
    for (int i = 0; i < 4; i++)
        q->kid[i] = combine(a->kid[i], b->kid[i], op);
    int c0 = q->kid[0]->kind;
    if (c0 != 2 && q->kid[1]->kind == c0 && q->kid[2]->kind == c0 && q->kid[3]->kind == c0) {
        for (int i = 0; i < 4; i++) {
            destroy(q->kid[i]);
            q->kid[i] = NULL;
        }
        q->kind = c0;
    }
    return q;
}

static int count_nodes(const Q *q) {
    int n = 1;
    if (q->kind == 2)
        for (int i = 0; i < 4; i++)
            n += count_nodes(q->kid[i]);
    return n;
}

static void paint_rect(unsigned char *bm, int x, int y, int w, int h) {
    for (int j = y; j < y + h && j < SIDE; j++)
        for (int i = x; i < x + w && i < SIDE; i++)
            bm[j * SIDE + i] = 1;
}

int main(void) {
    static unsigned char A[SIDE * SIDE], B[SIDE * SIDE];
    for (int r = 0; r < 6; r++) {
        int x = (int)rnd(SIDE), y = (int)rnd(SIDE);
        int w = 1 + (int)rnd(14), h = 1 + (int)rnd(14);
        paint_rect(A, x, y, w, h);
    }
    for (int r = 0; r < 6; r++) {
        int x = (int)rnd(SIDE), y = (int)rnd(SIDE);
        int w = 1 + (int)rnd(14), h = 1 + (int)rnd(14);
        paint_rect(B, x, y, w, h);
    }
    Q *qa = from_bitmap(A, 0, 0, SIDE), *qb = from_bitmap(B, 0, 0, SIDE);
    long areaA = 0, areaB = 0;
    for (int i = 0; i < SIDE * SIDE; i++) {
        areaA += A[i];
        areaB += B[i];
    }
    check(area(qa, SIDE) == areaA && area(qb, SIDE) == areaB, "area after build");
    for (int y = 0; y < SIDE; y++)
        for (int x = 0; x < SIDE; x++)
            check(get(qa, x, y, SIDE) == A[y * SIDE + x], "pixel readback");
    printf("A: area=%ld nodes=%d   B: area=%ld nodes=%d (raw pixels %d)\n", areaA, count_nodes(qa), areaB,
           count_nodes(qb), SIDE * SIDE);
    Q *un = combine(qa, qb, 0), *in = combine(qa, qb, 1);
    long au = 0, ai = 0;
    for (int i = 0; i < SIDE * SIDE; i++) {
        au += A[i] | B[i];
        ai += A[i] & B[i];
    }
    check(area(un, SIDE) == au && area(in, SIDE) == ai, "set operation areas");
    for (int y = 0; y < SIDE; y++)
        for (int x = 0; x < SIDE; x++) {
            check(get(un, x, y, SIDE) == (A[y * SIDE + x] | B[y * SIDE + x]), "union pixel");
            check(get(in, x, y, SIDE) == (A[y * SIDE + x] & B[y * SIDE + x]), "intersection pixel");
        }
    check(area(un, SIDE) + area(in, SIDE) == areaA + areaB, "inclusion-exclusion");
    printf("union area=%ld nodes=%d, intersection area=%ld nodes=%d\n", au, count_nodes(un), ai,
           count_nodes(in));
    /* random single-pixel writes with merging */
    int changes = 0;
    for (int step = 0; step < 4000; step++) {
        int x = (int)rnd(SIDE), y = (int)rnd(SIDE), c = (int)rnd(2);
        A[y * SIDE + x] = (unsigned char)c;
        qa = set(qa, x, y, SIDE, c);
        changes++;
        if (step % 500 == 0) {
            long ar = 0;
            for (int i = 0; i < SIDE * SIDE; i++)
                ar += A[i];
            check(area(qa, SIDE) == ar, "area during writes");
        }
    }
    for (int y = 0; y < SIDE; y++)
        for (int x = 0; x < SIDE; x++)
            check(get(qa, x, y, SIDE) == A[y * SIDE + x], "pixel after writes");
    /* fill uniformly: tree must collapse to a single leaf */
    for (int y = 0; y < SIDE; y++)
        for (int x = 0; x < SIDE; x++)
            qa = set(qa, x, y, SIDE, 1);
    check(qa->kind == 1 && count_nodes(qa) == 1, "fully merged");
    printf("after %d writes and a full fill: nodes=%d area=%ld\n", changes, count_nodes(qa), area(qa, SIDE));
    destroy(qa);
    destroy(qb);
    destroy(un);
    destroy(in);
    check(live_nodes == 0, "no leaks");
    return 0;
}
