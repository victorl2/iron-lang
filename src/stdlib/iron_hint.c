#include "iron_hint.h"

/* ── Black-box optimization barrier ─────────────────────────────────────── */
/*
 * On GCC/Clang (and clang-cl, which defines __clang__), use an empty
 * inline-asm block with a read-write register constraint. The compiler must
 * materialize x into a register and treat both sides of the asm as live,
 * which defeats constant folding and DCE without emitting any instructions.
 *
 * On compilers without GCC-style inline asm, fall back to a volatile global
 * sink — portable but slightly more expensive (one store + one load per call).
 * Iron currently only builds with clang/clang-cl, so the fallback is dead
 * code in practice but kept for future portability.
 */
#if defined(__GNUC__) || defined(__clang__)
int64_t Iron_hint_black_box(int64_t x) {
    __asm__ volatile("" : "+r"(x) : : "memory");
    return x;
}
#else
static volatile int64_t Iron_hint_sink_;
int64_t Iron_hint_black_box(int64_t x) {
    Iron_hint_sink_ = x;
    return Iron_hint_sink_;
}
#endif
