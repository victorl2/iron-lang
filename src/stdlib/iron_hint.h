#ifndef IRON_HINT_H
#define IRON_HINT_H

#include <stdint.h>

/* ── Compiler hints / optimization barriers ─────────────────────────────── */
/*
 * Iron_hint_black_box(x) returns x unchanged at runtime but acts as a
 * one-way fence to the backend optimizer: clang (and gcc) must treat the
 * input as consumed and the output as freshly materialized, preventing
 * constant folding, dead-store elimination, and loop-invariant hoisting
 * of the value through this call. Primarily used by benchmarks so that a
 * hot loop over a pure function of a compile-time constant does not get
 * replaced with a single precomputed result.
 *
 * Zero-cost on GCC/Clang: the implementation is an empty inline-asm block
 * with a register constraint on x, so no instructions are emitted.
 */
int64_t Iron_hint_black_box(int64_t x);

#endif /* IRON_HINT_H */
