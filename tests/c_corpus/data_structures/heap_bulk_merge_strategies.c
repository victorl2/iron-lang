/*
 * title: Merging array heaps by insertion versus rebuild
 * topic: data_structures
 * covers: binary heap merge, repeated insertion, concatenate and Floyd heapify, smaller-into-larger, comparison counting, crossover point
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0xB01Cull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;

static void sift_up(int *a, int i) {
    int x = a[i];
    while (i > 0) {
        int p = (i - 1) / 2;
        cmps++;
        if (a[p] <= x)
            break;
        a[i] = a[p];
        i = p;
    }
    a[i] = x;
}

static void sift_down(int *a, int n, int i) {
    int x = a[i];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n)
            break;
        if (c + 1 < n) {
            cmps++;
            if (a[c + 1] < a[c])
                c++;
        }
        cmps++;
        if (a[c] >= x)
            break;
        a[i] = a[c];
        i = c;
    }
    a[i] = x;
}

static void make_heap(int *a, int n) {
    for (int i = 0; i < n; i++)
        a[i] = (int)(rng() % 100000);
    for (int i = n / 2 - 1; i >= 0; i--)
        sift_down(a, n, i);
}

/* merge b (nb items) into a (na items) by inserting one by one; result has na+nb items in a */
static void merge_insert(int *a, int na, const int *b, int nb) {
    for (int i = 0; i < nb; i++) {
        a[na + i] = b[i];
        sift_up(a, na + i);
    }
}

static void merge_rebuild(int *a, int na, const int *b, int nb) {
    memcpy(a + na, b, sizeof(int) * (size_t)nb);
    int n = na + nb;
    for (int i = n / 2 - 1; i >= 0; i--)
        sift_down(a, n, i);
}

static int is_heap(const int *a, int n) {
    for (int i = 1; i < n; i++)
        if (a[(i - 1) / 2] > a[i])
            return 0;
    return 1;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

typedef struct {
    long ins_b_into_a, ins_a_into_b, ins_small_into_large, rebuild;
} Costs;

static Costs run(const int *a0, int na, const int *b0, int nb, int b_is_heap) {
    int *r = malloc(sizeof(int) * (size_t)(na + nb + 1)), *ref = malloc(sizeof(int) * (size_t)(na + nb + 1));
    Costs c;
    memcpy(ref, a0, sizeof(int) * (size_t)na);
    memcpy(ref + na, b0, sizeof(int) * (size_t)nb);
    qsort(ref, (size_t)(na + nb), sizeof(int), cmp_int);
    for (int variant = 0; variant < 4; variant++) {
        if (!b_is_heap && (variant == 1 || variant == 2)) {
            c.ins_a_into_b = c.ins_small_into_large = c.ins_b_into_a; /* b is only a stream here */
            continue;
        }
        cmps = 0;
        if (variant == 0) {
            memcpy(r, a0, sizeof(int) * (size_t)na);
            merge_insert(r, na, b0, nb);
        } else if (variant == 1) {
            memcpy(r, b0, sizeof(int) * (size_t)nb);
            merge_insert(r, nb, a0, na);
        } else if (variant == 2) {
            if (na >= nb) {
                memcpy(r, a0, sizeof(int) * (size_t)na);
                merge_insert(r, na, b0, nb);
            } else {
                memcpy(r, b0, sizeof(int) * (size_t)nb);
                merge_insert(r, nb, a0, na);
            }
        } else {
            memcpy(r, a0, sizeof(int) * (size_t)na);
            merge_rebuild(r, na, b0, nb);
        }
        check(is_heap(r, na + nb), "result is a heap");
        int *s = malloc(sizeof(int) * (size_t)(na + nb + 1));
        memcpy(s, r, sizeof(int) * (size_t)(na + nb));
        qsort(s, (size_t)(na + nb), sizeof(int), cmp_int);
        check(memcmp(s, ref, sizeof(int) * (size_t)(na + nb)) == 0, "same multiset");
        free(s);
        long v = cmps;
        if (variant == 0)
            c.ins_b_into_a = v;
        else if (variant == 1)
            c.ins_a_into_b = v;
        else if (variant == 2)
            c.ins_small_into_large = v;
        else
            c.rebuild = v;
    }
    free(r);
    free(ref);
    return c;
}

int main(void) {
    static int A[4000], B[4000];
    int shapes[][2] = {{1000, 1}, {1000, 10}, {1000, 100}, {1000, 500}, {1000, 1000}, {100, 1000}, {10, 1000}, {1, 1000}, {0, 300}, {300, 0}};
    printf("%-6s %-6s %-9s %-9s %-9s %-9s\n", "na", "nb", "b->a", "a->b", "small->big", "rebuild");
    for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        int na = shapes[s][0], nb = shapes[s][1];
        make_heap(A, na);
        make_heap(B, nb);
        Costs c = run(A, na, B, nb, 1);
        check(c.ins_small_into_large == (na >= nb ? c.ins_b_into_a : c.ins_a_into_b), "smaller-into-larger picks the matching direction");
        printf("%-6d %-6d %-9ld %-9ld %-9ld %-9ld\n", na, nb, c.ins_b_into_a, c.ins_a_into_b, c.ins_small_into_large, c.rebuild);
    }
    /* crossover: smallest nb (with na = 1000) where rebuilding beats inserting */
    for (int mode = 0; mode < 2; mode++) {
        int na = 1000, cross = -1;
        make_heap(A, na);
        for (int nb = 1; nb <= 1000; nb++) {
            if (mode == 0)
                make_heap(B, nb);
            else
                for (int i = 0; i < nb; i++)
                    B[i] = -1 - i; /* every new item is a new minimum: worst case for sift-up */
            Costs c = run(A, na, B, nb, mode == 0);
            if (c.rebuild < c.ins_small_into_large) {
                cross = nb;
                break;
            }
        }
        if (cross < 0)
            printf("%s extra items: insertion always wins up to 1000\n", mode ? "record-breaking" : "random");
        else
            printf("%s extra items: rebuilding wins from %d on\n", mode ? "record-breaking" : "random", cross);
        if (mode == 1)
            check(cross > 0, "record-breaking inserts make rebuilding pay off");
    }
    return 0;
}
