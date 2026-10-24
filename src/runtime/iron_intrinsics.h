#ifndef IRON_INTRINSICS_H
#define IRON_INTRINSICS_H

/* Inline helpers behind the compiler intrinsics (src/analyzer/intrinsics.h):
 * integer bit operations, wide and overflow-aware arithmetic, bit casts,
 * branch hints, byte views and the StringBuilder fast paths.
 *
 * Included at the end of iron_runtime.h. Freestanding like it: only clang
 * builtins, no platform headers. Every helper is `static inline` (or a
 * macro where the builtin must see the caller's expression, as
 * __builtin_expect does), so clang sees through them and a call costs
 * nothing: x.trailing_zeros() is one `tzcnt`/`rbit+clz`, a.mul_wide(b) one
 * `mul`/`umulh`. The suffixes name the C type: i8 i16 i32 i64 u8 u16 u32
 * u64 (Int is i64, UInt is u64). */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IRON_INTR_FN static inline __attribute__((always_inline, unused))

/* Rotates as the shift idiom every compiler turns into one rotate
 * instruction (__builtin_rotateleft* is clang only). */
#define IRON_INTR_ROT(S, T, W)                                                  \
    IRON_INTR_FN T iron_rotl_raw_##S(T x, unsigned n) {                         \
        n &= (W) - 1u;                                                          \
        return (T)((T)(x << n) | (T)(x >> ((-n) & ((W) - 1u))));                \
    }                                                                           \
    IRON_INTR_FN T iron_rotr_raw_##S(T x, unsigned n) {                         \
        n &= (W) - 1u;                                                          \
        return (T)((T)(x >> n) | (T)(x << ((-n) & ((W) - 1u))));                \
    }
IRON_INTR_ROT(8,  uint8_t,  8)
IRON_INTR_ROT(16, uint16_t, 16)
IRON_INTR_ROT(32, uint32_t, 32)
IRON_INTR_ROT(64, uint64_t, 64)
#undef IRON_INTR_ROT

/* Bit casts through memcpy (__builtin_bit_cast is clang only). */
#define IRON_INTR_BITCAST(NAME, TO, FROM)                                       \
    IRON_INTR_FN TO NAME(FROM v) { TO out; __builtin_memcpy(&out, &v, sizeof(out)); return out; }

/* 128-bit integers are a GNU extension: __extension__ keeps -Wpedantic quiet. */
__extension__ typedef __int128 iron_i128_t;
__extension__ typedef unsigned __int128 iron_u128_t;

/* ── Bit operations ──────────────────────────────────────────────────────────
 * leading_zeros / trailing_zeros are defined for 0 (the bit width), which
 * clang folds to lzcnt/tzcnt where available and to bsr/bsf with a preset
 * result on x86-64 baseline; rotations take the count modulo the width
 * (a negative count rotates the other way); prefix_xor sets bit i to the
 * xor of bits 0..i (clmul by all ones, done with log2(width) shift/xor
 * steps here; stage C adds the pclmul / pmull lowering). */
#define IRON_INTR_BITS(S, T, U, W, CLZ, CTZ, POP, ROTL, ROTR)                    \
    IRON_INTR_FN int64_t iron_clz_##S(T x) {                                    \
        U u = (U)x;                                                             \
        return u ? (int64_t)(CLZ) : (int64_t)(W);                               \
    }                                                                           \
    IRON_INTR_FN int64_t iron_ctz_##S(T x) {                                    \
        U u = (U)x;                                                             \
        return u ? (int64_t)(CTZ) : (int64_t)(W);                               \
    }                                                                           \
    IRON_INTR_FN int64_t iron_popcount_##S(T x) {                               \
        U u = (U)x;                                                             \
        return (int64_t)(POP);                                                  \
    }                                                                           \
    IRON_INTR_FN T iron_clear_lowest_##S(T x) {                                 \
        U u = (U)x;                                                             \
        return (T)(U)(u & (U)(u - 1u));                                         \
    }                                                                           \
    IRON_INTR_FN T iron_lowest_bit_##S(T x) {                                   \
        U u = (U)x;                                                             \
        return (T)(U)(u & (U)(0u - u));                                         \
    }                                                                           \
    IRON_INTR_FN T iron_rotl_##S(T x, int64_t k) {                              \
        U u = (U)x;                                                             \
        unsigned r = (unsigned)((uint64_t)k & (uint64_t)((W) - 1));             \
        return (T)(U)(ROTL);                                                    \
    }                                                                           \
    IRON_INTR_FN T iron_rotr_##S(T x, int64_t k) {                              \
        U u = (U)x;                                                             \
        unsigned r = (unsigned)((uint64_t)k & (uint64_t)((W) - 1));             \
        return (T)(U)(ROTR);                                                    \
    }                                                                           \
    IRON_INTR_FN T iron_prefix_xor_##S(T x) {                                   \
        U u = (U)x;                                                             \
        for (unsigned s = 1; s < (W); s <<= 1) u ^= (U)(u << s);                \
        return (T)u;                                                            \
    }

IRON_INTR_BITS(u8,  uint8_t,  uint8_t,  8,
               __builtin_clz((unsigned)u) - 24, __builtin_ctz((unsigned)u),
               __builtin_popcount((unsigned)u),
               iron_rotl_raw_8(u, (uint8_t)r), iron_rotr_raw_8(u, (uint8_t)r))
IRON_INTR_BITS(i8,  int8_t,   uint8_t,  8,
               __builtin_clz((unsigned)u) - 24, __builtin_ctz((unsigned)u),
               __builtin_popcount((unsigned)u),
               iron_rotl_raw_8(u, (uint8_t)r), iron_rotr_raw_8(u, (uint8_t)r))
IRON_INTR_BITS(u16, uint16_t, uint16_t, 16,
               __builtin_clz((unsigned)u) - 16, __builtin_ctz((unsigned)u),
               __builtin_popcount((unsigned)u),
               iron_rotl_raw_16(u, (uint16_t)r), iron_rotr_raw_16(u, (uint16_t)r))
IRON_INTR_BITS(i16, int16_t,  uint16_t, 16,
               __builtin_clz((unsigned)u) - 16, __builtin_ctz((unsigned)u),
               __builtin_popcount((unsigned)u),
               iron_rotl_raw_16(u, (uint16_t)r), iron_rotr_raw_16(u, (uint16_t)r))
IRON_INTR_BITS(u32, uint32_t, uint32_t, 32,
               __builtin_clz(u), __builtin_ctz(u), __builtin_popcount(u),
               iron_rotl_raw_32(u, r), iron_rotr_raw_32(u, r))
IRON_INTR_BITS(i32, int32_t,  uint32_t, 32,
               __builtin_clz(u), __builtin_ctz(u), __builtin_popcount(u),
               iron_rotl_raw_32(u, r), iron_rotr_raw_32(u, r))
IRON_INTR_BITS(u64, uint64_t, uint64_t, 64,
               __builtin_clzll(u), __builtin_ctzll(u), __builtin_popcountll(u),
               iron_rotl_raw_64(u, r), iron_rotr_raw_64(u, r))
IRON_INTR_BITS(i64, int64_t,  uint64_t, 64,
               __builtin_clzll(u), __builtin_ctzll(u), __builtin_popcountll(u),
               iron_rotl_raw_64(u, r), iron_rotr_raw_64(u, r))
#undef IRON_INTR_BITS

IRON_INTR_FN uint8_t  iron_bswap_u8(uint8_t x)   { return x; }
IRON_INTR_FN int8_t   iron_bswap_i8(int8_t x)    { return x; }
IRON_INTR_FN uint16_t iron_bswap_u16(uint16_t x) { return __builtin_bswap16(x); }
IRON_INTR_FN int16_t  iron_bswap_i16(int16_t x)  { return (int16_t)__builtin_bswap16((uint16_t)x); }
IRON_INTR_FN uint32_t iron_bswap_u32(uint32_t x) { return __builtin_bswap32(x); }
IRON_INTR_FN int32_t  iron_bswap_i32(int32_t x)  { return (int32_t)__builtin_bswap32((uint32_t)x); }
IRON_INTR_FN uint64_t iron_bswap_u64(uint64_t x) { return __builtin_bswap64(x); }
IRON_INTR_FN int64_t  iron_bswap_i64(int64_t x)  { return (int64_t)__builtin_bswap64((uint64_t)x); }

/* ── Wide multiply ───────────────────────────────────────────────────────────
 * a.mul_wide(b) is (iron_mul_hi(a, b), iron_mul_lo(a, b)): the high half
 * has the receiver's type (signed high half for a signed type), the low
 * half is the unsigned type of the same width. 64-bit types use the
 * 128-bit product (`mul`/`umulh`/`smulh` on arm64, one `mulq`/`imulq` on
 * x86-64, including x86_64-pc-windows-msvc where clang supports __int128
 * without a runtime library call); targets without __int128 use the
 * schoolbook 32x32 split. Narrower types multiply in 64 bits. */
#if defined(__SIZEOF_INT128__)
IRON_INTR_FN uint64_t iron_mul_hi_u64(uint64_t a, uint64_t b) {
    return (uint64_t)(((iron_u128_t)a * b) >> 64);
}
IRON_INTR_FN int64_t iron_mul_hi_i64(int64_t a, int64_t b) {
    return (int64_t)(((iron_i128_t)a * b) >> 64);
}
#else
IRON_INTR_FN uint64_t iron_mul_hi_u64(uint64_t a, uint64_t b) {
    uint64_t a_lo = a & 0xffffffffu, a_hi = a >> 32;
    uint64_t b_lo = b & 0xffffffffu, b_hi = b >> 32;
    uint64_t p0 = a_lo * b_lo, p1 = a_lo * b_hi, p2 = a_hi * b_lo, p3 = a_hi * b_hi;
    uint64_t mid = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
    return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}
IRON_INTR_FN int64_t iron_mul_hi_i64(int64_t a, int64_t b) {
    uint64_t hi = iron_mul_hi_u64((uint64_t)a, (uint64_t)b);
    if (a < 0) hi -= (uint64_t)b;
    if (b < 0) hi -= (uint64_t)a;
    return (int64_t)hi;
}
#endif
IRON_INTR_FN uint64_t iron_mul_lo_u64(uint64_t a, uint64_t b) { return a * b; }
IRON_INTR_FN uint64_t iron_mul_lo_i64(int64_t a, int64_t b)  { return (uint64_t)a * (uint64_t)b; }

#define IRON_INTR_MULW(S, T, U, W, WT)                                          \
    IRON_INTR_FN T iron_mul_hi_##S(T a, T b) {                                  \
        return (T)(((WT)a * (WT)b) >> (W));                                     \
    }                                                                           \
    IRON_INTR_FN U iron_mul_lo_##S(T a, T b) {                                  \
        return (U)((uint64_t)(WT)a * (uint64_t)(WT)b);                          \
    }
IRON_INTR_MULW(u8,  uint8_t,  uint8_t,  8,  uint64_t)
IRON_INTR_MULW(i8,  int8_t,   uint8_t,  8,  int64_t)
IRON_INTR_MULW(u16, uint16_t, uint16_t, 16, uint64_t)
IRON_INTR_MULW(i16, int16_t,  uint16_t, 16, int64_t)
IRON_INTR_MULW(u32, uint32_t, uint32_t, 32, uint64_t)
IRON_INTR_MULW(i32, int32_t,  uint32_t, 32, int64_t)
#undef IRON_INTR_MULW

/* ── Overflow-aware arithmetic ───────────────────────────────────────────────
 * a.add_overflow(b) is (iron_add_wrap(a, b), iron_add_ovf(a, b)); after
 * inlining clang merges the two into one add and a flag read (`adds` +
 * `cset`, `add` + `seto`/`setb`). add_carry adds a carry in and reports
 * whether the exact a + b + carry fits: for unsigned types that is the
 * carry out of an `adc` chain. */
#define IRON_INTR_OVF(S, T, U)                                                  \
    IRON_INTR_FN T iron_add_wrap_##S(T a, T b) {                                \
        return (T)(U)((uint64_t)(U)a + (uint64_t)(U)b);                         \
    }                                                                           \
    IRON_INTR_FN bool iron_add_ovf_##S(T a, T b) {                              \
        T r; return __builtin_add_overflow(a, b, &r);                           \
    }                                                                           \
    IRON_INTR_FN T iron_sub_wrap_##S(T a, T b) {                                \
        return (T)(U)((uint64_t)(U)a - (uint64_t)(U)b);                         \
    }                                                                           \
    IRON_INTR_FN bool iron_sub_ovf_##S(T a, T b) {                              \
        T r; return __builtin_sub_overflow(a, b, &r);                           \
    }                                                                           \
    IRON_INTR_FN T iron_mul_wrap_##S(T a, T b) {                                \
        return (T)(U)((uint64_t)(U)a * (uint64_t)(U)b);                         \
    }                                                                           \
    IRON_INTR_FN bool iron_mul_ovf_##S(T a, T b) {                              \
        T r; return __builtin_mul_overflow(a, b, &r);                           \
    }                                                                           \
    IRON_INTR_FN T iron_add_carry_wrap_##S(T a, T b, bool c) {                  \
        return (T)(U)((uint64_t)(U)a + (uint64_t)(U)b + (uint64_t)(c ? 1 : 0)); \
    }
IRON_INTR_OVF(u8,  uint8_t,  uint8_t)
IRON_INTR_OVF(i8,  int8_t,   uint8_t)
IRON_INTR_OVF(u16, uint16_t, uint16_t)
IRON_INTR_OVF(i16, int16_t,  uint16_t)
IRON_INTR_OVF(u32, uint32_t, uint32_t)
IRON_INTR_OVF(i32, int32_t,  uint32_t)
IRON_INTR_OVF(u64, uint64_t, uint64_t)
IRON_INTR_OVF(i64, int64_t,  uint64_t)
#undef IRON_INTR_OVF

/* Carry out of a + b + c: for unsigned types either partial sum wraps; for
 * signed types the exact sum is checked in a wider type (the 128-bit one
 * for 64-bit operands), since a + b may overflow and + c bring it back
 * (-128 + -1 + 1 fits Int8). */
#define IRON_INTR_CARRY_U(S, T)                                                 \
    IRON_INTR_FN bool iron_add_carry_ovf_##S(T a, T b, bool c) {                \
        T t, r;                                                                 \
        bool o1 = __builtin_add_overflow(a, b, &t);                             \
        bool o2 = __builtin_add_overflow(t, (T)(c ? 1 : 0), &r);                \
        return o1 | o2;                                                         \
    }
#define IRON_INTR_CARRY_S(S, T, LO, HI)                                         \
    IRON_INTR_FN bool iron_add_carry_ovf_##S(T a, T b, bool c) {                \
        int64_t s = (int64_t)a + (int64_t)b + (c ? 1 : 0);                      \
        return s < (LO) || s > (HI);                                            \
    }
IRON_INTR_CARRY_U(u8,  uint8_t)
IRON_INTR_CARRY_U(u16, uint16_t)
IRON_INTR_CARRY_U(u32, uint32_t)
IRON_INTR_CARRY_U(u64, uint64_t)
IRON_INTR_CARRY_S(i8,  int8_t,  INT8_MIN,  INT8_MAX)
IRON_INTR_CARRY_S(i16, int16_t, INT16_MIN, INT16_MAX)
IRON_INTR_CARRY_S(i32, int32_t, INT32_MIN, INT32_MAX)
#undef IRON_INTR_CARRY_U
#undef IRON_INTR_CARRY_S
IRON_INTR_FN bool iron_add_carry_ovf_i64(int64_t a, int64_t b, bool c) {
#if defined(__SIZEOF_INT128__)
    iron_i128_t s = (iron_i128_t)a + (iron_i128_t)b + (c ? 1 : 0);
    return s < (iron_i128_t)INT64_MIN || s > (iron_i128_t)INT64_MAX;
#else
    int64_t t, r;
    bool o1 = __builtin_add_overflow(a, b, &t);
    if (!c) return o1;
    bool o2 = __builtin_add_overflow(t, (int64_t)1, &r);
    /* a + b wrapped down to INT64_MAX and the + 1 wrapped back: the exact
     * sum is INT64_MIN, which fits. */
    if (o1 && o2) return false;
    return o1 | o2;
#endif
}

/* ── Bit casts ───────────────────────────────────────────────────────────── */
IRON_INTR_BITCAST(iron_f64_to_bits, uint64_t, double)
IRON_INTR_BITCAST(iron_f64_from_bits, double, uint64_t)
IRON_INTR_BITCAST(iron_f32_to_bits, uint32_t, float)
IRON_INTR_BITCAST(iron_f32_from_bits, float, uint32_t)
IRON_INTR_FN uint8_t  iron_i8_to_bits(int8_t x)   { return (uint8_t)x; }
IRON_INTR_FN int8_t   iron_i8_from_bits(uint8_t u)  { return (int8_t)u; }
IRON_INTR_FN uint16_t iron_i16_to_bits(int16_t x) { return (uint16_t)x; }
IRON_INTR_FN int16_t  iron_i16_from_bits(uint16_t u) { return (int16_t)u; }
IRON_INTR_FN uint32_t iron_i32_to_bits(int32_t x) { return (uint32_t)x; }
IRON_INTR_FN int32_t  iron_i32_from_bits(uint32_t u) { return (int32_t)u; }
IRON_INTR_FN uint64_t iron_i64_to_bits(int64_t x) { return (uint64_t)x; }
IRON_INTR_FN int64_t  iron_i64_from_bits(uint64_t u) { return (int64_t)u; }

/* ── Branch hints ────────────────────────────────────────────────────────────
 * Macros, so __builtin_expect sees the condition the branch tests. */
#define iron_hint_likely(c)   __builtin_expect(!!(c), 1)
#define iron_hint_unlikely(c) __builtin_expect(!!(c), 0)

/* ── Alignment ───────────────────────────────────────────────────────────────
 * `//iron:align=cacheline`: 128 on Apple arm64 (its cores prefetch 128-byte
 * line pairs), 64 everywhere else. */
#if defined(__APPLE__) && defined(__aarch64__)
#define IRON_CACHELINE 128
#else
#define IRON_CACHELINE 64
#endif

/* Over-aligned storage: list buffers of an element type aligned above 16
 * bytes (an `//iron:align=N` object) come from these (iron_os.c:
 * posix_memalign, or _aligned_malloc on the Windows UCRT) and go back
 * through iron_mem_free_aligned. The IRON_LIST_* macros pick the plain or
 * aligned allocator from _Alignof(T) at compile time. */
void *iron_mem_alloc_aligned(size_t size, size_t align);
void  iron_mem_free_aligned(void *p);
void *iron_mem_realloc_aligned(void *p, size_t old_size, size_t new_size, size_t align);
#define IRON_LIST_ALLOC(T, bytes) \
    (_Alignof(T) > 16 ? iron_mem_alloc_aligned((bytes), _Alignof(T)) : iron_mem_alloc(bytes))
#define IRON_LIST_REALLOC(T, p, old_bytes, new_bytes) \
    (_Alignof(T) > 16 ? iron_mem_realloc_aligned((p), (old_bytes), (new_bytes), _Alignof(T)) \
                      : iron_mem_realloc((p), (new_bytes)))
#define IRON_LIST_FREE(T, p) \
    do { if (_Alignof(T) > 16) iron_mem_free_aligned(p); else iron_mem_free(p); } while (0)

/* ── Views ───────────────────────────────────────────────────────────────────
 * View[T] and ViewMut[T] are { T *ptr; int64_t len; } passed by value; the
 * emitter declares the struct for each element type it meets, the scalar
 * ones are here so the helpers below can name them. */
typedef struct Iron_View_int8_t   { int8_t   *ptr; int64_t len; } Iron_View_int8_t;
typedef struct Iron_View_int16_t  { int16_t  *ptr; int64_t len; } Iron_View_int16_t;
typedef struct Iron_View_int32_t  { int32_t  *ptr; int64_t len; } Iron_View_int32_t;
typedef struct Iron_View_int64_t  { int64_t  *ptr; int64_t len; } Iron_View_int64_t;
typedef struct Iron_View_uint8_t  { uint8_t  *ptr; int64_t len; } Iron_View_uint8_t;
typedef struct Iron_View_uint16_t { uint16_t *ptr; int64_t len; } Iron_View_uint16_t;
typedef struct Iron_View_uint32_t { uint32_t *ptr; int64_t len; } Iron_View_uint32_t;
typedef struct Iron_View_uint64_t { uint64_t *ptr; int64_t len; } Iron_View_uint64_t;
typedef struct Iron_View_float    { float    *ptr; int64_t len; } Iron_View_float;
typedef struct Iron_View_double   { double   *ptr; int64_t len; } Iron_View_double;
typedef struct Iron_View_bool     { bool     *ptr; int64_t len; } Iron_View_bool;
typedef struct Iron_View_Iron_String { Iron_String *ptr; int64_t len; } Iron_View_Iron_String;

/* A slice [a, z) of a view of `len` elements: 0 <= a <= z <= len, else an
 * Iron panic. One unsigned compare per bound; hoisted out of loops by clang
 * when the view is loop invariant. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_view_range(const char *site_file, int site_line,
                           int64_t start, int64_t end, int64_t len);
#if defined(IRON_DEBUG_TRAPS) && defined(__clang__)
static inline __attribute__((always_inline, nodebug, unused, noreturn))
void iron_dbg_panic_view_range(const char *f, int l, int64_t a, int64_t z, int64_t n) {
    if (iron_debug_panic_stop(f, l, "slice out of bounds (start %lld, end %lld, length %lld)",
                              (long long)a, (long long)z, (long long)n))
        __builtin_debugtrap();
    iron_panic_view_range(f, l, a, z, n);
}
#define iron_panic_view_range(...) iron_dbg_panic_view_range(__VA_ARGS__)
#endif
#define IRON_VIEW_CHECK_RANGE(a, z, n, ...)                                     \
    do {                                                                        \
        if ((uint64_t)(a) > (uint64_t)(z) || (uint64_t)(z) > (uint64_t)(n))     \
            iron_panic_view_range(__VA_ARGS__, (a), (z), (n));                  \
    } while (0)

/* The bytes of a String: the inline buffer or the heap characters. The
 * view points into *s, so the emitter passes the binding's own storage. */
IRON_INTR_FN Iron_View_uint8_t iron_string_bytes(const Iron_String *s) {
    Iron_View_uint8_t v;
    if (s->heap.flags & 0x01) {
        v.ptr = (uint8_t *)s->heap.data;
        v.len = (int64_t)s->heap.byte_length;
    } else {
        v.ptr = (uint8_t *)s->sso.data;
        v.len = (int64_t)s->sso.len;
    }
    return v;
}

/* Out-of-line bodies (iron_string.c). */
Iron_String iron_bytes_to_string(Iron_View_uint8_t b);   /* lossy: U+FFFD */
bool        iron_bytes_valid_utf8(Iron_View_uint8_t b);
Iron_String iron_bytes_to_string_exact(Iron_View_uint8_t b);

IRON_INTR_FN int64_t iron_bytes_index_of(Iron_View_uint8_t b, uint8_t x) {
    if (b.len <= 0) return -1;
    const uint8_t *hit = (const uint8_t *)__builtin_memchr(b.ptr, x, (size_t)b.len);
    return hit ? (int64_t)(hit - b.ptr) : -1;
}

/* Eight bytes from b[i] as a little-endian word; bytes past the end are
 * `fill`. i must be inside the view (checked by the caller's emitted
 * bounds check). The fast path is one unaligned load. */
IRON_INTR_FN uint64_t iron_bytes_load_u64_le_or(Iron_View_uint8_t b, int64_t i, uint8_t fill) {
    uint8_t buf[8];
    if (b.len - i >= 8) {
        __builtin_memcpy(buf, b.ptr + i, 8);
    } else {
        for (int k = 0; k < 8; k++) buf[k] = (i + k < b.len) ? b.ptr[i + k] : fill;
    }
    uint64_t w = 0;
    for (int k = 7; k >= 0; k--) w = (w << 8) | buf[k];
    return w;
}

/* ── StringBuilder ───────────────────────────────────────────────────────────
 * The Iron object StringBuilder (stdlib/string.iron) has the layout of
 * Iron_SBRT: `data` is the address of a growing string buffer laid out like
 * a heap String's characters (a refcount header in front), so finish()
 * hands it to the String without copying. Helpers take the object by
 * address as void * (the generated struct type is not visible here). */
typedef struct {
    int64_t data;      /* char * after the header, 0 when empty */
    int64_t length;
    int64_t capacity;
} Iron_SBRT;

void        iron_sb_grow(Iron_SBRT *sb, int64_t extra);      /* iron_string.c */
Iron_String iron_sb_finish(Iron_SBRT *sb);                   /* iron_string.c */
void        iron_sb_push_int(void *sb, int64_t n);           /* iron_string.c */
void        iron_sb_push_uint(void *sb, uint64_t n);          /* iron_string.c */

IRON_INTR_FN void iron_sb_reserve(void *p, int64_t extra) {
    Iron_SBRT *sb = (Iron_SBRT *)p;
    if (sb->capacity < 0) sb->capacity = sb->length;
    if (extra > sb->capacity - sb->length) iron_sb_grow(sb, extra);
}
IRON_INTR_FN void iron_sb_push_byte(void *p, uint8_t b) {
    Iron_SBRT *sb = (Iron_SBRT *)p;
    if (sb->capacity < 0) sb->capacity = sb->length;
    if (sb->length == sb->capacity) iron_sb_grow(sb, 1);
    ((uint8_t *)(uintptr_t)sb->data)[sb->length++] = b;
}
IRON_INTR_FN void iron_sb_push_bytes(void *p, Iron_View_uint8_t b) {
    Iron_SBRT *sb = (Iron_SBRT *)p;
    if (sb->capacity < 0) sb->capacity = sb->length;
    if (b.len > sb->capacity - sb->length) iron_sb_grow(sb, b.len);
    if (b.len > 0)
        __builtin_memcpy((uint8_t *)(uintptr_t)sb->data + sb->length, b.ptr, (size_t)b.len);
    sb->length += b.len;
}
IRON_INTR_FN void iron_sb_push_str(void *p, Iron_String s) {
    iron_sb_push_bytes(p, iron_string_bytes(&s));
}
/* Spare capacity: n zeroed bytes past the length, written through the
 * returned view and kept with commit(k). */
IRON_INTR_FN Iron_View_uint8_t iron_sb_spare(void *p, int64_t n, const char *site_file, int site_line) {
    Iron_SBRT *sb = (Iron_SBRT *)p;
    if (n < 0) iron_panic_view_range(site_file, site_line, 0, n, 0);
    if (sb->capacity < 0) sb->capacity = sb->length;
    if (n > sb->capacity - sb->length) iron_sb_grow(sb, n);
    Iron_View_uint8_t v;
    v.ptr = (uint8_t *)(uintptr_t)sb->data + sb->length;
    v.len = n;
    if (n > 0) __builtin_memset(v.ptr, 0, (size_t)n);
    /* A negative capacity marks one active reservation and encodes its
     * exclusive end. Normal mutators cancel it before using capacity. */
    sb->capacity = -(sb->length + n) - 1;
    return v;
}
IRON_INTR_FN void iron_sb_commit(void *p, int64_t n, const char *site_file, int site_line) {
    Iron_SBRT *sb = (Iron_SBRT *)p;
    int64_t reserved = sb->capacity < 0 ? -(sb->capacity + 1) - sb->length : 0;
    if (sb->capacity >= 0 || (uint64_t)n > (uint64_t)reserved)
        iron_panic_view_range(site_file, site_line, 0, n, reserved);
    sb->length += n;
    sb->capacity = sb->length;  /* consume the reservation */
}
IRON_INTR_FN int64_t iron_sb_len(const void *p) { return ((const Iron_SBRT *)p)->length; }
IRON_INTR_FN void iron_sb_clear(void *p) {
    Iron_SBRT *sb = (Iron_SBRT *)p;
    if (sb->capacity < 0) sb->capacity = sb->length;
    sb->length = 0;
}
IRON_INTR_FN Iron_View_uint8_t iron_sb_bytes(const void *p) {
    const Iron_SBRT *sb = (const Iron_SBRT *)p;
    Iron_View_uint8_t v;
    v.ptr = (uint8_t *)(uintptr_t)sb->data;
    v.len = sb->length;
    return v;
}

#define iron_sb_spare_at(p, n)   iron_sb_spare((p), (n), IRON_SITE)
#define iron_sb_commit_at(p, n)  iron_sb_commit((p), (n), IRON_SITE)
#define iron_sb_finish_at(p)     iron_sb_finish((Iron_SBRT *)(void *)(p))
#define iron_t_sb_new(ST)        ((ST){ 0 })
#define iron_t_sb_with_capacity(ST, n) \
    ({ ST _sb = { 0 }; int64_t _n = (n); if (_n > 0) iron_sb_reserve(&_sb, _n); _sb; })
/* The drop of a StringBuilder (string.iron): frees an unfinished buffer. */
void Iron_stringbuilder__release(int64_t data);

/* ── View operations ─────────────────────────────────────────────────────────
 * Generic over the element type through __typeof__; every view struct is
 * { T *ptr; int64_t len; }. The macros that check a range report the Iron
 * line through IRON_SITE, which generated code defines per statement. */
#define iron_t_list_view(VT, lp) ((VT){ (lp)->items, (lp)->count })
#define iron_t_bvec_view(VT, bp) ((VT){ (bp)->data, (int64_t)(bp)->len })
#define iron_view_len(v)         ((v).len)
#define iron_view_as_read(v)     (v)
#define iron_view_slice(v, a, z)                                                \
    ({ __typeof__(v) _v = (v); int64_t _a = (a), _z = (z);                      \
       IRON_VIEW_CHECK_RANGE(_a, _z, _v.len, IRON_SITE);                        \
       _v.ptr += _a; _v.len = _z - _a; _v; })
/* Spare room in a dynamic list: grow to hold n more (doubling), zero the
 * n slots past the count and view them; commit(k) adds the first k. */
#define iron_t_list_spare(VT, lp, n)                                            \
    ({ __typeof__(lp) _l = (lp); int64_t _n = (n);                              \
       if (_n < 0) iron_panic_view_range(IRON_SITE, 0, _n, 0);                  \
       if (_l->capacity < 0) _l->capacity = _l->count;                          \
       if (_n > INT64_MAX - _l->count) iron_oom_abort("spare");                \
       if (_n > _l->capacity - _l->count) {                                     \
           int64_t _cap = _l->capacity ? _l->capacity * 2 : 8;                  \
           if (_cap < _l->count + _n) _cap = _l->count + _n;                    \
           void *_ni = IRON_LIST_REALLOC(__typeof__(*_l->items), _l->items,     \
               (size_t)_l->capacity * sizeof(*_l->items), (size_t)_cap * sizeof(*_l->items)); \
           if (!_ni) iron_oom_abort("spare");                                   \
           _l->items = (__typeof__(_l->items))_ni; _l->capacity = _cap;         \
       }                                                                        \
       if (_n > 0) iron_mem_set(_l->items + _l->count, 0, (size_t)_n * sizeof(*_l->items)); \
       _l->capacity = -(_l->count + _n) - 1;                                    \
       ((VT){ _l->items + _l->count, _n }); })
#define iron_list_commit(lp, n)                                                 \
    do { __typeof__(lp) _l = (lp); int64_t _n = (n);                            \
         int64_t _r = _l->capacity < 0 ? -(_l->capacity + 1) - _l->count : 0;   \
         if (_l->capacity >= 0 || (uint64_t)_n > (uint64_t)_r)                  \
             iron_panic_view_range(IRON_SITE, 0, _n, _r);                       \
         _l->count += _n; _l->capacity = _l->count; } while (0)
/* Element equality: bit equality for integers and Bool, == for floats
 * (so -0.0 equals 0.0 and NaN equals nothing, as with ==). */
#define IRON_VIEW_IS_FLOAT(v) _Generic((v).ptr, float *: 1, double *: 1, default: 0)
#define iron_view_index_of(v, x)                                                \
    ({ __typeof__(v) _v = (v); __typeof__(*_v.ptr) _x = (x); int64_t _r = -1;   \
       for (int64_t _i = 0; _i < _v.len; _i++) if (_v.ptr[_i] == _x) { _r = _i; break; } \
       _r; })
#define IRON_VIEW_SAME(p, q, n, isf)                                            \
    ({ bool _s = true;                                                          \
       if (isf) { for (int64_t _i = 0; _i < (n); _i++) if (!((p)[_i] == (q)[_i])) { _s = false; break; } } \
       else if ((n) > 0) _s = __builtin_memcmp((p), (q), (size_t)(n) * sizeof(*(p))) == 0; \
       _s; })
#define iron_view_starts_with(v, o)                                             \
    ({ __typeof__(v) _v = (v); __typeof__(v) _o = (o);                          \
       _o.len <= _v.len && IRON_VIEW_SAME(_v.ptr, _o.ptr, _o.len, IRON_VIEW_IS_FLOAT(_v)); })
#define iron_view_ends_with(v, o)                                               \
    ({ __typeof__(v) _v = (v); __typeof__(v) _o = (o);                          \
       _o.len <= _v.len &&                                                      \
       IRON_VIEW_SAME(_v.ptr + (_v.len - _o.len), _o.ptr, _o.len, IRON_VIEW_IS_FLOAT(_v)); })
#define iron_view_equals(v, o)                                                  \
    ({ __typeof__(v) _v = (v); __typeof__(v) _o = (o);                          \
       _o.len == _v.len && IRON_VIEW_SAME(_v.ptr, _o.ptr, _o.len, IRON_VIEW_IS_FLOAT(_v)); })
#define iron_view_fill(v, x)                                                    \
    do { __typeof__(v) _v = (v); __typeof__(*_v.ptr) _x = (x);                  \
         for (int64_t _i = 0; _i < _v.len; _i++) _v.ptr[_i] = _x; } while (0)
#define iron_view_copy_from(v, src)                                             \
    do { __typeof__(v) _v = (v); __typeof__(v) _s = (src);                      \
         if (_s.len > _v.len) iron_panic_view_range(IRON_SITE, 0, _s.len, _v.len); \
         if (_s.len > 0) iron_mem_move(_v.ptr, _s.ptr, (size_t)_s.len * sizeof(*_v.ptr)); \
    } while (0)
#define iron_t_view_to_list(LT, v)                                              \
    ({ __typeof__(v) _v = (v); LT _l; _l.count = _v.len; _l.capacity = _v.len;  \
       _l.items = NULL;                                                         \
       if (_v.len > 0) {                                                        \
           _l.items = (__typeof__(_l.items))IRON_LIST_ALLOC(__typeof__(*_v.ptr), (size_t)_v.len * sizeof(*_v.ptr)); \
           if (!_l.items) iron_oom_abort("to_list");                            \
           iron_mem_copy(_l.items, _v.ptr, (size_t)_v.len * sizeof(*_v.ptr));   \
       }                                                                        \
       _l; })
#define iron_view_load_u64_le_or(v, i, fill)                                    \
    ({ Iron_View_uint8_t _v = (v); int64_t _i = (i);                            \
       if ((uint64_t)_i >= (uint64_t)_v.len) iron_panic_index_oob(IRON_SITE, _i, _v.len); \
       iron_bytes_load_u64_le_or(_v, _i, (fill)); })
#define iron_t_string_from_utf8(OT, b)                                          \
    ({ Iron_View_uint8_t _b = (b); OT _r;                                       \
       iron_mem_set(&_r, 0, sizeof _r);                                         \
       if (iron_bytes_valid_utf8(_b)) { _r.value = iron_bytes_to_string_exact(_b); _r.has_value = true; } \
       _r; })

#endif /* IRON_INTRINSICS_H */
