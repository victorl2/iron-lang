#ifndef IRON_ANALYZER_INTRINSICS_H
#define IRON_ANALYZER_INTRINSICS_H

/* Compiler intrinsic methods: methods on the built-in types that have no
 * Iron declaration and lower straight to an inline C helper of the runtime
 * header (src/runtime/iron_intrinsics.h) or to a few LIR instructions.
 *
 *   integers   x.leading_zeros(), x.trailing_zeros(), x.count_ones(),
 *              x.clear_lowest_bit(), x.lowest_bit(), x.rotate_left(k),
 *              x.rotate_right(k), x.byte_swap(), x.prefix_xor(),
 *              a.mul_wide(b), a.mul_high(b), a.add_overflow(b),
 *              a.sub_overflow(b), a.mul_overflow(b), a.add_carry(b, c),
 *              x.to_bits() and T.from_bits(u) on the signed types
 *   floats     f.to_bits(), Float.from_bits(u), Float32.from_bits(u)
 *   Hint       Hint.likely(c), Hint.unlikely(c)
 *   views      see manual section 2.15 (Bytes, View[T], ViewMut[T])
 *
 * The type checker (typecheck.c, check_intrinsic_method) records the
 * intrinsic on the method call (Iron_MethodCallExpr.intrinsic), HIR keeps
 * it (method_call.intrinsic) and hir_to_lir.c lowers it. The table below
 * is also what ironls lists for completion and shows on hover. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum {
    IRON_INTR_NONE = 0,

    /* Integer bit operations (every integer type). */
    IRON_INTR_LEADING_ZEROS,
    IRON_INTR_TRAILING_ZEROS,
    IRON_INTR_COUNT_ONES,
    IRON_INTR_CLEAR_LOWEST_BIT,
    IRON_INTR_LOWEST_BIT,
    IRON_INTR_ROTATE_LEFT,
    IRON_INTR_ROTATE_RIGHT,
    IRON_INTR_BYTE_SWAP,
    IRON_INTR_PREFIX_XOR,

    /* Wide and overflow-aware arithmetic (every integer type). */
    IRON_INTR_MUL_WIDE,
    IRON_INTR_MUL_HIGH,
    IRON_INTR_ADD_OVERFLOW,
    IRON_INTR_SUB_OVERFLOW,
    IRON_INTR_MUL_OVERFLOW,
    IRON_INTR_ADD_CARRY,

    /* Bit casts: x.to_bits() on floats and signed integers,
     * T.from_bits(u) on the same types. */
    IRON_INTR_TO_BITS,
    IRON_INTR_FROM_BITS,

    /* Hint.likely(c) / Hint.unlikely(c). */
    IRON_INTR_HINT_LIKELY,
    IRON_INTR_HINT_UNLIKELY,

    /* Views (manual 2.15). */
    IRON_INTR_STRING_BYTES,      /* s.bytes() -> Bytes                     */
    IRON_INTR_LIST_VIEW,         /* xs.view() -> View[T]                   */
    IRON_INTR_LIST_VIEW_MUT,     /* xs.view_mut() -> ViewMut[T]            */
    IRON_INTR_LIST_SPARE,        /* xs.spare(n) -> ViewMut[T]              */
    IRON_INTR_LIST_COMMIT,       /* xs.commit(n)                           */
    IRON_INTR_VIEW_LEN,          /* v.len() -> Int                         */
    IRON_INTR_VIEW_SLICE,        /* v.slice(a, z) -> same view type        */
    IRON_INTR_VIEW_INDEX_OF,     /* v.index_of(x) -> Int                   */
    IRON_INTR_VIEW_STARTS_WITH,  /* v.starts_with(other) -> Bool           */
    IRON_INTR_VIEW_ENDS_WITH,    /* v.ends_with(other) -> Bool             */
    IRON_INTR_VIEW_EQUALS,       /* v.equals(other) -> Bool                */
    IRON_INTR_VIEW_TO_LIST,      /* v.to_list() -> [T]                     */
    IRON_INTR_VIEW_TO_STRING,    /* b.to_string() -> String (Bytes only)   */
    IRON_INTR_VIEW_FILL,         /* w.fill(x) (ViewMut only)               */
    IRON_INTR_VIEW_COPY_FROM,    /* w.copy_from(src) (ViewMut only)        */
    IRON_INTR_VIEW_LOAD_U64_LE_OR, /* b.load_u64_le_or(i, fill) -> UInt64  */
    IRON_INTR_VIEW_AS_READ,      /* w.as_view() -> View[T] (ViewMut only)  */
    IRON_INTR_STRING_FROM_UTF8,  /* String.from_utf8(b) -> String?         */

    /* StringBuilder (stdlib/string.iron, manual 9.3). */
    IRON_INTR_SB_NEW,            /* StringBuilder.new()                    */
    IRON_INTR_SB_WITH_CAPACITY,  /* StringBuilder.with_capacity(n)         */
    IRON_INTR_SB_PUSH_BYTE,      /* sb.push_byte(b: UInt8)                 */
    IRON_INTR_SB_PUSH_BYTES,     /* sb.push_bytes(b: Bytes)                */
    IRON_INTR_SB_PUSH_STR,       /* sb.push_str(s: String)                 */
    IRON_INTR_SB_PUSH_INT,       /* sb.push_int(n: Int)                    */
    IRON_INTR_SB_PUSH_UINT,      /* sb.push_uint(n: UInt64)                */
    IRON_INTR_SB_SPARE,          /* sb.spare(n) -> ViewMut[UInt8]          */
    IRON_INTR_SB_COMMIT,         /* sb.commit(n)                           */
    IRON_INTR_SB_RESERVE,        /* sb.reserve(n)                          */
    IRON_INTR_SB_FINISH,         /* sb.finish() -> String                  */
    IRON_INTR_SB_LEN,            /* sb.len() -> Int                        */
    IRON_INTR_SB_CLEAR,          /* sb.clear()                             */
    IRON_INTR_SB_BYTES,          /* sb.bytes() -> Bytes                    */

    IRON_INTR__COUNT
} Iron_Intrinsic;

/* Which receivers an intrinsic applies to (Iron_IntrinsicInfo.recv). */
enum {
    IRON_INTR_RECV_INT       = 1u << 0,  /* every integer type              */
    IRON_INTR_RECV_SIGNED    = 1u << 1,  /* signed integer types only       */
    IRON_INTR_RECV_FLOAT     = 1u << 2,  /* Float, Float32, Float64         */
    IRON_INTR_RECV_HINT      = 1u << 3,  /* the Hint namespace              */
    IRON_INTR_RECV_STRING    = 1u << 4,  /* String values                   */
    IRON_INTR_RECV_LIST      = 1u << 5,  /* [T], [T; N], [T; <=N]           */
    IRON_INTR_RECV_VIEW      = 1u << 6,  /* View[T] and ViewMut[T]          */
    IRON_INTR_RECV_VIEW_MUT  = 1u << 7,  /* ViewMut[T] only                 */
    IRON_INTR_RECV_BYTES     = 1u << 8,  /* Bytes (View[UInt8]) only        */
    IRON_INTR_RECV_STATIC    = 1u << 9,  /* called on the type: T.f(...)    */
    IRON_INTR_RECV_SB        = 1u << 10, /* StringBuilder                   */
};

typedef struct {
    const char     *name;       /* method name                              */
    Iron_Intrinsic  id;
    unsigned        recv;       /* IRON_INTR_RECV_* mask                    */
    const char     *signature;  /* `func trailing_zeros() -> Int`; T is the
                                 * receiver type, E the element type, U the
                                 * unsigned type of T's width                */
    const char     *doc;        /* one sentence for hover and completion    */
} Iron_IntrinsicInfo;

static const Iron_IntrinsicInfo iron_intrinsic_table[] = {
    { "leading_zeros", IRON_INTR_LEADING_ZEROS, IRON_INTR_RECV_INT,
      "func leading_zeros() -> Int",
      "Number of zero bits above the highest set bit; the bit width for 0." },
    { "trailing_zeros", IRON_INTR_TRAILING_ZEROS, IRON_INTR_RECV_INT,
      "func trailing_zeros() -> Int",
      "Number of zero bits below the lowest set bit; the bit width for 0." },
    { "count_ones", IRON_INTR_COUNT_ONES, IRON_INTR_RECV_INT,
      "func count_ones() -> Int",
      "Number of set bits (population count)." },
    { "clear_lowest_bit", IRON_INTR_CLEAR_LOWEST_BIT, IRON_INTR_RECV_INT,
      "func clear_lowest_bit() -> T",
      "The value with its lowest set bit cleared: x & (x - 1)." },
    { "lowest_bit", IRON_INTR_LOWEST_BIT, IRON_INTR_RECV_INT,
      "func lowest_bit() -> T",
      "Only the lowest set bit of the value (0 for 0): x & -x." },
    { "rotate_left", IRON_INTR_ROTATE_LEFT, IRON_INTR_RECV_INT,
      "func rotate_left(k: Int) -> T",
      "The bits rotated left by k modulo the bit width." },
    { "rotate_right", IRON_INTR_ROTATE_RIGHT, IRON_INTR_RECV_INT,
      "func rotate_right(k: Int) -> T",
      "The bits rotated right by k modulo the bit width." },
    { "byte_swap", IRON_INTR_BYTE_SWAP, IRON_INTR_RECV_INT,
      "func byte_swap() -> T",
      "The bytes in reverse order (endianness swap)." },
    { "prefix_xor", IRON_INTR_PREFIX_XOR, IRON_INTR_RECV_INT,
      "func prefix_xor() -> T",
      "Bit i of the result is the xor of bits 0 to i: the string-region mask of quote bits." },
    { "mul_wide", IRON_INTR_MUL_WIDE, IRON_INTR_RECV_INT,
      "func mul_wide(b: T) -> (T, U)",
      "The full double-width product as (high, low) halves." },
    { "mul_high", IRON_INTR_MUL_HIGH, IRON_INTR_RECV_INT,
      "func mul_high(b: T) -> T",
      "The high half of the double-width product." },
    { "add_overflow", IRON_INTR_ADD_OVERFLOW, IRON_INTR_RECV_INT,
      "func add_overflow(b: T) -> (T, Bool)",
      "The wrapped sum and whether the exact sum does not fit in T." },
    { "sub_overflow", IRON_INTR_SUB_OVERFLOW, IRON_INTR_RECV_INT,
      "func sub_overflow(b: T) -> (T, Bool)",
      "The wrapped difference and whether the exact difference does not fit in T." },
    { "mul_overflow", IRON_INTR_MUL_OVERFLOW, IRON_INTR_RECV_INT,
      "func mul_overflow(b: T) -> (T, Bool)",
      "The wrapped product and whether the exact product does not fit in T." },
    { "add_carry", IRON_INTR_ADD_CARRY, IRON_INTR_RECV_INT,
      "func add_carry(b: T, carry: Bool) -> (T, Bool)",
      "The wrapped sum a + b + carry and whether the exact sum does not fit in T (the carry out)." },
    { "to_bits", IRON_INTR_TO_BITS, IRON_INTR_RECV_FLOAT | IRON_INTR_RECV_SIGNED,
      "func to_bits() -> U",
      "The bit pattern as the unsigned integer of the same width." },
    { "from_bits", IRON_INTR_FROM_BITS,
      IRON_INTR_RECV_FLOAT | IRON_INTR_RECV_SIGNED | IRON_INTR_RECV_STATIC,
      "func from_bits(bits: U) -> T",
      "The value whose bit pattern is bits (T.from_bits(u))." },
    { "likely", IRON_INTR_HINT_LIKELY, IRON_INTR_RECV_HINT | IRON_INTR_RECV_STATIC,
      "func likely(c: Bool) -> Bool",
      "Returns c and tells the optimizer it is usually true." },
    { "unlikely", IRON_INTR_HINT_UNLIKELY, IRON_INTR_RECV_HINT | IRON_INTR_RECV_STATIC,
      "func unlikely(c: Bool) -> Bool",
      "Returns c and tells the optimizer it is usually false." },
    { "bytes", IRON_INTR_STRING_BYTES, IRON_INTR_RECV_STRING,
      "func bytes() -> Bytes",
      "The string's UTF-8 bytes as a read-only view, without copying." },
    { "view", IRON_INTR_LIST_VIEW, IRON_INTR_RECV_LIST,
      "func view() -> View[E]",
      "A read-only view of the elements, without copying." },
    { "view_mut", IRON_INTR_LIST_VIEW_MUT, IRON_INTR_RECV_LIST,
      "func view_mut() -> ViewMut[E]",
      "A writable view of the elements (needs a var list; its length is fixed while the view lives)." },
    { "spare", IRON_INTR_LIST_SPARE, IRON_INTR_RECV_LIST,
      "func spare(n: Int) -> ViewMut[E]",
      "Reserves room for n more elements and returns a writable view of them, zeroed; commit(k) adds the first k." },
    { "commit", IRON_INTR_LIST_COMMIT, IRON_INTR_RECV_LIST,
      "func commit(n: Int)",
      "Adds n elements written through spare() to the list (n at most the spare length)." },
    { "len", IRON_INTR_VIEW_LEN, IRON_INTR_RECV_VIEW,
      "func len() -> Int",
      "Number of elements in the view." },
    { "slice", IRON_INTR_VIEW_SLICE, IRON_INTR_RECV_VIEW,
      "func slice(start: Int, end: Int) -> Self",
      "The elements [start, end) as a view of the same kind; checked once, no copy." },
    { "index_of", IRON_INTR_VIEW_INDEX_OF, IRON_INTR_RECV_VIEW,
      "func index_of(x: E) -> Int",
      "Position of the first element equal to x, or -1." },
    { "starts_with", IRON_INTR_VIEW_STARTS_WITH, IRON_INTR_RECV_VIEW,
      "func starts_with(prefix: View[E]) -> Bool",
      "Whether the view begins with the elements of prefix." },
    { "ends_with", IRON_INTR_VIEW_ENDS_WITH, IRON_INTR_RECV_VIEW,
      "func ends_with(suffix: View[E]) -> Bool",
      "Whether the view ends with the elements of suffix." },
    { "equals", IRON_INTR_VIEW_EQUALS, IRON_INTR_RECV_VIEW,
      "func equals(other: View[E]) -> Bool",
      "Whether both views have the same elements." },
    { "to_list", IRON_INTR_VIEW_TO_LIST, IRON_INTR_RECV_VIEW,
      "func to_list() -> [E]",
      "A new list with a copy of the elements." },
    { "to_string", IRON_INTR_VIEW_TO_STRING, IRON_INTR_RECV_BYTES,
      "func to_string() -> String",
      "A new String with a copy of the bytes; invalid UTF-8 sequences become U+FFFD." },
    { "fill", IRON_INTR_VIEW_FILL, IRON_INTR_RECV_VIEW_MUT,
      "func fill(x: E)",
      "Sets every element of the view to x." },
    { "copy_from", IRON_INTR_VIEW_COPY_FROM, IRON_INTR_RECV_VIEW_MUT,
      "func copy_from(src: View[E])",
      "Copies src into the start of the view (src.len() must not exceed len())." },
    { "load_u64_le_or", IRON_INTR_VIEW_LOAD_U64_LE_OR, IRON_INTR_RECV_BYTES,
      "func load_u64_le_or(i: Int, fill: UInt8) -> UInt64",
      "Bytes [i, i+8) as a little-endian word; bytes past the end read as fill (i must be in range)." },
    { "as_view", IRON_INTR_VIEW_AS_READ, IRON_INTR_RECV_VIEW_MUT,
      "func as_view() -> View[E]",
      "The same elements as a read-only view." },
    { "from_utf8", IRON_INTR_STRING_FROM_UTF8, IRON_INTR_RECV_STRING | IRON_INTR_RECV_STATIC,
      "func from_utf8(b: Bytes) -> String?",
      "A new String with a copy of the bytes, or null when they are not valid UTF-8." },
    { "new", IRON_INTR_SB_NEW, IRON_INTR_RECV_SB | IRON_INTR_RECV_STATIC,
      "func new() -> StringBuilder",
      "An empty builder; the first push allocates." },
    { "with_capacity", IRON_INTR_SB_WITH_CAPACITY, IRON_INTR_RECV_SB | IRON_INTR_RECV_STATIC,
      "func with_capacity(n: Int) -> StringBuilder",
      "An empty builder with room for n bytes before it grows." },
    { "push_byte", IRON_INTR_SB_PUSH_BYTE, IRON_INTR_RECV_SB,
      "func push_byte(b: UInt8)",
      "Appends one byte." },
    { "push_bytes", IRON_INTR_SB_PUSH_BYTES, IRON_INTR_RECV_SB,
      "func push_bytes(b: Bytes)",
      "Appends the bytes of a view (a String or [UInt8] argument is lent as one)." },
    { "push_str", IRON_INTR_SB_PUSH_STR, IRON_INTR_RECV_SB,
      "func push_str(s: String)",
      "Appends the bytes of a string." },
    { "push_int", IRON_INTR_SB_PUSH_INT, IRON_INTR_RECV_SB,
      "func push_int(n: Int)",
      "Appends n in decimal, without a temporary String." },
    { "push_uint", IRON_INTR_SB_PUSH_UINT, IRON_INTR_RECV_SB,
      "func push_uint(n: UInt64)",
      "Appends n in decimal, without a temporary String." },
    { "spare", IRON_INTR_SB_SPARE, IRON_INTR_RECV_SB,
      "func spare(n: Int) -> ViewMut[UInt8]",
      "Room for n more bytes as a writable view, zeroed; commit(k) appends the first k written." },
    { "commit", IRON_INTR_SB_COMMIT, IRON_INTR_RECV_SB,
      "func commit(n: Int)",
      "Appends n bytes written through spare() (n at most the spare length)." },
    { "reserve", IRON_INTR_SB_RESERVE, IRON_INTR_RECV_SB,
      "func reserve(n: Int)",
      "Makes room for n more bytes, so the next pushes do not reallocate." },
    { "finish", IRON_INTR_SB_FINISH, IRON_INTR_RECV_SB,
      "func finish() -> String",
      "The built String (the buffer moves into it, no copy); the builder is empty afterwards." },
    { "len", IRON_INTR_SB_LEN, IRON_INTR_RECV_SB,
      "func len() -> Int",
      "Number of bytes built so far." },
    { "clear", IRON_INTR_SB_CLEAR, IRON_INTR_RECV_SB,
      "func clear()",
      "Drops the content and keeps the buffer for reuse." },
    { "bytes", IRON_INTR_SB_BYTES, IRON_INTR_RECV_SB,
      "func bytes() -> Bytes",
      "The bytes built so far, as a view (no push while it is in use)." },
    { NULL, IRON_INTR_NONE, 0, NULL, NULL },
};

/* The table entry for intrinsic `id` (NULL for IRON_INTR_NONE). */
static inline const Iron_IntrinsicInfo *iron_intrinsic_info(Iron_Intrinsic id) {
    for (int i = 0; iron_intrinsic_table[i].name; i++)
        if (iron_intrinsic_table[i].id == id) return &iron_intrinsic_table[i];
    return NULL;
}

/* The table entry named `name` for a receiver of category `recv` (one of
 * the IRON_INTR_RECV_* kinds, plus IRON_INTR_RECV_SIGNED for a signed
 * integer). `on_type` selects the functions called on the type
 * (`T.from_bits(u)`) instead of on a value. NULL when there is none. */
static inline const Iron_IntrinsicInfo *iron_intrinsic_find(const char *name,
                                                            unsigned recv,
                                                            bool on_type) {
    if (!name) return NULL;
    for (int i = 0; iron_intrinsic_table[i].name; i++) {
        const Iron_IntrinsicInfo *e = &iron_intrinsic_table[i];
        if (((e->recv & IRON_INTR_RECV_STATIC) != 0) != on_type) continue;
        if (!(e->recv & recv & ~(unsigned)IRON_INTR_RECV_STATIC)) continue;
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

#endif /* IRON_ANALYZER_INTRINSICS_H */
