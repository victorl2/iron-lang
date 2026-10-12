/*
 * title: Struct layout report with offsetof and alignof
 * topic: memory
 * covers: offsetof, sizeof, _Alignof, padding gaps, nested structs, unions, arrays in structs
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Only fixed-width members and pointers: identical on every LP64 target. */
typedef struct {
    uint8_t a;
    uint32_t b;
    uint8_t c;
    uint64_t d;
    uint16_t e;
} Mixed;

typedef struct {
    uint64_t d;
    uint32_t b;
    uint16_t e;
    uint8_t a;
    uint8_t c;
} Sorted;

typedef struct {
    uint8_t tag;
    union {
        uint8_t byte;
        uint32_t word;
        uint64_t dword;
        double real;
    } u;
} Tagged;

typedef struct {
    uint16_t x;
    struct {
        uint8_t p;
        uint32_t q;
    } inner;
    uint8_t tail[3];
} Nested;

typedef struct {
    void *ptr;
    uint8_t flag;
    uint8_t buf[5];
    uint16_t n;
} WithPtr;

typedef struct {
    uint8_t a[3];
    uint16_t b[3];
    uint32_t c[3];
} Arrays;

typedef struct {
    uint8_t flag;
    _Alignas(16) uint8_t vec[16];
    uint8_t after;
} Over;

typedef struct {
    double x, y;
    float w;
} Vec;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define FIELD(T, f) printf("  %-8s offset %2zu size %2zu\n", #f, offsetof(T, f), sizeof(((T *)0)->f))
#define HEAD(T) printf("%s: size %zu align %zu\n", #T, sizeof(T), _Alignof(T))

/* sum of the gaps between consecutive members, given offsets and sizes */
static size_t padding_of(const size_t *off, const size_t *sz, int n, size_t total) {
    size_t end = 0, pad = 0;
    for (int i = 0; i < n; i++) {
        pad += off[i] - end;
        end = off[i] + sz[i];
    }
    return pad + (total - end);
}

int main(void) {
    HEAD(Mixed);
    FIELD(Mixed, a);
    FIELD(Mixed, b);
    FIELD(Mixed, c);
    FIELD(Mixed, d);
    FIELD(Mixed, e);
    {
        size_t off[] = {offsetof(Mixed, a), offsetof(Mixed, b), offsetof(Mixed, c), offsetof(Mixed, d),
                        offsetof(Mixed, e)};
        size_t sz[] = {1, 4, 1, 8, 2};
        printf("  padding %zu\n", padding_of(off, sz, 5, sizeof(Mixed)));
    }
    HEAD(Sorted);
    {
        size_t off[] = {offsetof(Sorted, d), offsetof(Sorted, b), offsetof(Sorted, e), offsetof(Sorted, a),
                        offsetof(Sorted, c)};
        size_t sz[] = {8, 4, 2, 1, 1};
        printf("  padding %zu\n", padding_of(off, sz, 5, sizeof(Sorted)));
    }
    check(sizeof(Sorted) < sizeof(Mixed), "sorting shrinks the struct");

    HEAD(Tagged);
    FIELD(Tagged, tag);
    FIELD(Tagged, u);
    check(sizeof(Tagged) == 16, "tagged size");

    HEAD(Nested);
    FIELD(Nested, x);
    FIELD(Nested, inner);
    FIELD(Nested, tail);
    printf("  inner.q at %zu\n", offsetof(Nested, inner.q));

    HEAD(WithPtr);
    FIELD(WithPtr, ptr);
    FIELD(WithPtr, flag);
    FIELD(WithPtr, buf);
    FIELD(WithPtr, n);

    HEAD(Arrays);
    FIELD(Arrays, a);
    FIELD(Arrays, b);
    FIELD(Arrays, c);

    HEAD(Over);
    FIELD(Over, flag);
    FIELD(Over, vec);
    FIELD(Over, after);

    HEAD(Vec);
    FIELD(Vec, x);
    FIELD(Vec, y);
    FIELD(Vec, w);

    /* every offset is a multiple of the member alignment */
    check(offsetof(Mixed, b) % _Alignof(uint32_t) == 0, "b aligned");
    check(offsetof(Mixed, d) % _Alignof(uint64_t) == 0, "d aligned");
    check(offsetof(Over, vec) % 16 == 0, "over vec aligned");
    /* sizeof is a multiple of alignof, so arrays of structs stay aligned */
    check(sizeof(Mixed) % _Alignof(Mixed) == 0, "array stride");
    check(sizeof(Over) % _Alignof(Over) == 0, "over stride");

    Mixed arr[3];
    printf("array stride Mixed: %zu\n", (size_t)((char *)&arr[1] - (char *)&arr[0]));
    printf("array total Mixed[3]: %zu\n", sizeof arr);
    return 0;
}
