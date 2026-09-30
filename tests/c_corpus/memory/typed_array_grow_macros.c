/*
 * title: Type-safe checked array grow and push macros
 * topic: memory
 * covers: sizeof(*p) macros, overflow-checked element counts, statement macros, failure-preserving realloc, generic containers without void*
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Grow *p (a T*) to at least `need` elements, doubling; sizes derive from sizeof(*p) so the element size can never mismatch. */
static int grow_impl(void **p, size_t *cap, size_t need, size_t esz, size_t max_bytes) {
    if (need <= *cap) return 1;
    size_t nc = *cap ? *cap : 4;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) return 0;
        nc *= 2;
    }
    if (esz == 0 || nc > SIZE_MAX / esz) return 0;
    if (nc * esz > max_bytes) {
        nc = need;                              /* try the exact size once before failing */
        if (nc > SIZE_MAX / esz || nc * esz > max_bytes) return 0;
    }
    void *np = realloc(*p, nc * esz);
    if (!np) return 0;                          /* *p untouched */
    *p = np;
    *cap = nc;
    return 1;
}

#define ARR(T) struct { T *v; size_t n, cap; }
#define ARR_GROW(a, need, maxb) grow_impl((void **)&(a).v, &(a).cap, (need), sizeof(*(a).v), (maxb))
#define ARR_PUSH(a, x, maxb) (ARR_GROW((a), (a).n + 1, (maxb)) ? ((a).v[(a).n++] = (x), 1) : 0)
#define ARR_FREE(a) (free((a).v), (a).v = NULL, (a).n = (a).cap = 0)

typedef struct { int id; double w; } Item;

int main(void) {
    ARR(int) ints = {0};
    ARR(Item) items = {0};
    ARR(unsigned char) bytes = {0};

    for (int i = 0; i < 100; i++) ARR_PUSH(ints, i * i, (size_t)1 << 20);
    printf("ints: n=%zu cap=%zu last=%d\n", ints.n, ints.cap, ints.v[ints.n - 1]);

    for (int i = 0; i < 37; i++) {
        Item it = {i, i * 0.5};
        ARR_PUSH(items, it, (size_t)1 << 20);
    }
    printf("items: n=%zu cap=%zu sizeof(Item)=%zu id36=%d w=%.1f\n", items.n, items.cap, sizeof(Item), items.v[36].id, items.v[36].w);

    /* byte array with a 100-byte ceiling: pushes succeed until the ceiling then are refused, content preserved */
    int pushed = 0, refused = 0;
    for (int i = 0; i < 150; i++) {
        if (ARR_PUSH(bytes, (unsigned char)i, 100)) pushed++; else refused++;
    }
    printf("bytes with 100-byte ceiling: pushed=%d refused=%d n=%zu cap=%zu\n", pushed, refused, bytes.n, bytes.cap);
    int intact = 1;
    for (size_t i = 0; i < bytes.n; i++) if (bytes.v[i] != (unsigned char)i) intact = 0;
    printf("contents intact after refusals: %d\n", intact);

    /* the ceiling check for wide elements: 40 items * 16 bytes = 640 > 512 */
    ARR(Item) small = {0};
    int ok_small = 0;
    for (int i = 0; i < 40; i++) { Item it = {i, 0}; ok_small += ARR_PUSH(small, it, 512); }
    printf("Item array with 512-byte ceiling: accepted=%d (of 40), cap=%zu\n", ok_small, small.cap);

    /* overflow attempt: need so large that need*esz wraps must fail without touching the array */
    size_t before_cap = ints.cap;
    int r1 = ARR_GROW(ints, SIZE_MAX / 2 + 5, SIZE_MAX);
    int r2 = ARR_GROW(ints, SIZE_MAX / sizeof(int) + 1, SIZE_MAX);
    printf("absurd grow requests: %d %d cap unchanged=%d n=%zu\n", r1, r2, ints.cap == before_cap, ints.n);

    ARR_FREE(ints); ARR_FREE(items); ARR_FREE(bytes); ARR_FREE(small);
    printf("freed: %d\n", ints.v == NULL && items.v == NULL && bytes.v == NULL && small.v == NULL);
    return (intact && ints.cap == 0) ? 0 : 1;
}
