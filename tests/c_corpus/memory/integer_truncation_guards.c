/*
 * title: Integer narrowing and signedness guards
 * topic: memory
 * covers: checked casts, range tables, sign conversion, length-field truncation, exhaustive 8/16-bit checks
 * deps: libc
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int to_u8(long long v, uint8_t *o) { if (v < 0 || v > UINT8_MAX) return 0; *o = (uint8_t)v; return 1; }
static int to_i8(long long v, int8_t *o) { if (v < INT8_MIN || v > INT8_MAX) return 0; *o = (int8_t)v; return 1; }
static int to_u16(long long v, uint16_t *o) { if (v < 0 || v > UINT16_MAX) return 0; *o = (uint16_t)v; return 1; }
static int to_i16(long long v, int16_t *o) { if (v < INT16_MIN || v > INT16_MAX) return 0; *o = (int16_t)v; return 1; }
static int to_int(size_t v, int *o) { if (v > (size_t)INT_MAX) return 0; *o = (int)v; return 1; }
static int size_from_int(int v, size_t *o) { if (v < 0) return 0; *o = (size_t)v; return 1; }
static int u64_to_size_lim(uint64_t v, uint32_t lim, uint32_t *o) { if (v > lim) return 0; *o = (uint32_t)v; return 1; }

static uint64_t s = 88172645463325252ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

/* A serializer whose length prefix is 16-bit: must refuse payloads over 65535 bytes. */
static int encode_len16(size_t payload, unsigned char *out2) {
    uint16_t l;
    if (payload > UINT16_MAX) return 0;
    l = (uint16_t)payload;
    out2[0] = (unsigned char)(l >> 8);
    out2[1] = (unsigned char)(l & 0xFF);
    return 1;
}

int main(void) {
    /* exhaustive: every value in [-40000, 70000] against the reference "round trip" test */
    long ok_u8 = 0, ok_i8 = 0, ok_u16 = 0, ok_i16 = 0;
    for (long long v = -40000; v <= 70000; v++) {
        uint8_t a; int8_t b; uint16_t c; int16_t d;
        if (to_u8(v, &a)) { if (a != v) return 1; ok_u8++; } else if (v >= 0 && v <= 255) return 1;
        if (to_i8(v, &b)) { if (b != v) return 1; ok_i8++; } else if (v >= -128 && v <= 127) return 1;
        if (to_u16(v, &c)) { if (c != v) return 1; ok_u16++; } else if (v >= 0 && v <= 65535) return 1;
        if (to_i16(v, &d)) { if (d != v) return 1; ok_i16++; } else if (v >= -32768 && v <= 32767) return 1;
    }
    printf("fits: u8=%ld i8=%ld u16=%ld i16=%ld of 110001 values\n", ok_u8, ok_i8, ok_u16, ok_i16);

    /* the unguarded truncations a naive cast would silently produce */
    printf("naive (uint8_t)300 = %u, (uint16_t)70000 = %u, (int8_t)200 = %d\n",
           (unsigned)(uint8_t)(unsigned)300, (unsigned)(uint16_t)(unsigned)70000, (int)(int8_t)(unsigned)200);

    /* size_t <-> int */
    int i;
    printf("size_t 2^31-1 -> int: %d\n", to_int((size_t)INT_MAX, &i));
    printf("size_t 2^31   -> int: %d\n", to_int((size_t)INT_MAX + 1, &i));
    printf("size_t max    -> int: %d\n", to_int(SIZE_MAX, &i));
    size_t z;
    printf("int -1 -> size_t: %d, int 0 -> size_t: %d\n", size_from_int(-1, &z), size_from_int(0, &z));

    /* comparison pitfalls: signed vs unsigned */
    int neg = -1;
    unsigned big = 1;
    printf("naive unsigned compare (-1 < 1u) = %d, guarded = %d\n", (unsigned)neg < big, (neg < 0) || (unsigned)neg < big);

    /* random 64-bit inputs against a 32-bit limit */
    int passed = 0, refused = 0;
    for (int n = 0; n < 5000; n++) {
        uint64_t v = rnd();
        int shift = (int)(rnd() % 64);
        v >>= shift;
        uint32_t o;
        int ok = u64_to_size_lim(v, 1000000u, &o);
        if (ok != (v <= 1000000u)) return 1;
        if (ok) { if (o != v) return 1; passed++; } else refused++;
    }
    printf("random u64 -> 32 with limit 1e6: passed=%d refused=%d\n", passed, refused);

    unsigned char pre[2];
    size_t sizes[] = {0, 1, 255, 256, 65535, 65536, 100000};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int ok = encode_len16(sizes[k], pre);
        if (ok) printf("payload %6zu -> prefix %02x %02x\n", sizes[k], pre[0], pre[1]);
        else printf("payload %6zu -> refused\n", sizes[k]);
    }
    return 0;
}
