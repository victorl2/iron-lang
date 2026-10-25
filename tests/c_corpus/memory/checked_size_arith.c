/*
 * title: Overflow-checked size arithmetic and calloc-style allocation
 * topic: memory
 * covers: add/mul overflow checks, flexible array sizing, checked calloc, brute-force cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}

/* Overflow checks against an arbitrary ceiling so they can be cross-checked exhaustively. */
static int add_ok(size_t a, size_t b, size_t lim, size_t *out) {
    if (a > lim - b && b <= lim) return 0;
    if (b > lim) return 0;
    *out = a + b;
    return *out <= lim;
}
static int mul_ok(size_t a, size_t b, size_t lim, size_t *out) {
    if (a != 0 && b > lim / a) return 0;
    *out = a * b;
    return 1;
}

typedef struct {
    size_t count;
    unsigned char data[]; /* flexible array member */
} Blob;

static int blob_bytes(size_t n, size_t elem, size_t *total) {
    size_t body;
    if (!mul_ok(n, elem, SIZE_MAX, &body)) return 0;
    return add_ok(offsetof(Blob, data), body, SIZE_MAX, total);
}

#define MAX_ALLOC ((size_t)1 << 20)

static int calls_refused;
static void *checked_calloc(size_t n, size_t elem) {
    size_t total;
    if (!mul_ok(n, elem, MAX_ALLOC, &total) || total > MAX_ALLOC) {
        calls_refused++;
        return NULL;
    }
    return calloc(n ? n : 1, elem ? elem : 1);
}

int main(void) {
    /* exhaustive over a small ceiling */
    size_t lim = 200;
    long mul_fail = 0, add_fail = 0, mul_pass = 0, add_pass = 0;
    for (size_t a = 0; a <= 300; a++)
        for (size_t b = 0; b <= 300; b++) {
            size_t r = 0;
            int ok = mul_ok(a, b, lim, &r);
            int want = (a * b) <= lim;
            if (ok != want || (ok && r != a * b)) { fprintf(stderr, "mul mismatch %zu %zu\n", a, b); return 1; }
            if (ok) mul_pass++; else mul_fail++;
            ok = add_ok(a, b, lim, &r);
            want = (a + b) <= lim;
            if (ok != want || (ok && r != a + b)) { fprintf(stderr, "add mismatch %zu %zu\n", a, b); return 1; }
            if (ok) add_pass++; else add_fail++;
        }
    printf("ceiling 200: mul pass=%ld fail=%ld, add pass=%ld fail=%ld\n", mul_pass, mul_fail, add_pass, add_fail);

    /* edges at SIZE_MAX */
    size_t r;
    printf("SIZE_MAX+1 add: %d\n", add_ok(SIZE_MAX, 1, SIZE_MAX, &r));
    printf("SIZE_MAX+0 add: %d\n", add_ok(SIZE_MAX, 0, SIZE_MAX, &r));
    printf("2^32 * 2^32 mul: %d\n", mul_ok((size_t)1 << 32, (size_t)1 << 32, SIZE_MAX, &r));
    printf("2^32 * (2^32-1) mul: %d\n", mul_ok((size_t)1 << 32, ((size_t)1 << 32) - 1, SIZE_MAX, &r));
    r = 1;
    int zok = mul_ok(0, SIZE_MAX, SIZE_MAX, &r);
    printf("0 * SIZE_MAX mul: %d r=%zu\n", zok, r);

    /* random 64-bit cross-check through 32x32->64 exact math */
    int refused = 0, accepted = 0;
    for (int i = 0; i < 20000; i++) {
        uint64_t x = rnd(), sx = rnd() % 40, y = rnd(), sy = rnd() % 40;
        x >>= sx; y >>= sy;
        size_t a = (size_t)x, b = (size_t)y;
        int ok = mul_ok(a, b, SIZE_MAX, &r);
        int want;
        if (a == 0 || b == 0) want = 1;
        else {
            /* split into halves: a*b overflows iff hi parts product nonzero or cross sum overflows */
            uint64_t ah = a >> 32, al = a & 0xFFFFFFFFu, bh = b >> 32, bl = b & 0xFFFFFFFFu;
            if (ah && bh) want = 0;
            else {
                uint64_t cross = ah * bl + al * bh; /* one of ah,bh is zero: no overflow */
                if (cross >> 32) want = 0;
                else {
                    uint64_t low = al * bl;
                    uint64_t sum = (cross << 32) + low;
                    want = sum >= low;
                }
            }
        }
        if (ok != want) { fprintf(stderr, "random mul mismatch\n"); return 1; }
        if (ok) { if (r != a * b) return 1; accepted++; } else refused++;
    }
    printf("random mul: accepted=%d refused=%d\n", accepted, refused);

    /* flexible array sizing */
    size_t t;
    int bok = blob_bytes(10, 4, &t);
    printf("blob(10,4) = %d total=%zu\n", bok, t);
    printf("blob(SIZE_MAX/2,4) = %d\n", blob_bytes(SIZE_MAX / 2, 4, &t));
    printf("blob(SIZE_MAX-7,1) = %d\n", blob_bytes(SIZE_MAX - 7, 1, &t));

    /* checked calloc */
    void *p1 = checked_calloc(1000, 8);
    void *p2 = checked_calloc(SIZE_MAX / 2 + 1, 2);
    void *p3 = checked_calloc((size_t)1 << 21, 1);
    void *p4 = checked_calloc(0, 16);
    printf("calloc results: %d %d %d %d refused=%d\n", p1 != NULL, p2 != NULL, p3 != NULL, p4 != NULL, calls_refused);
    unsigned char *z = p1;
    for (int i = 0; i < 8000; i++) if (z[i]) { fprintf(stderr, "not zeroed\n"); return 1; }
    Blob *b = malloc(t = 0 + offsetof(Blob, data) + 16);
    if (!b) return 1;
    b->count = 16; memset(b->data, 7, 16);
    printf("blob header=%zu payload=%zu\n", (size_t)(offsetof(Blob, data) == sizeof(size_t)), b->count);
    free(b); free(p1); free(p4);
    return 0;
}
