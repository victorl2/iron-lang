---
phase: 65-raymath
plan: 01
subsystem: stdlib
tags: [raylib, raymath, ffi, math, vector2, shim, stdlib-binding]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Vector2/3/4/Matrix/Quaternion layout pinned by 392-entry _Static_assert grid; Iron_Vector2 8 B byte-identical to raylib Vector2"
  - phase: 63-2d-drawing
    provides: "memcpy-out struct-by-value return template (Iron_draw_get_spline_point_linear)"
  - phase: 64-collision-2d-3d
    provides: "instance-method-on-data-carrying-object dispatch; self-reserved E0101 guard rule; Matrix 64 B pass-by-value as function arg"
provides:
  - "RAYMATH_STATIC_INLINE include strategy (raylib.h before raymath.h, required by RL_VECTOR2_TYPE guard family)"
  - "RMath namespace (object RMath {}) with 6 freestanding scalar helpers: clamp, lerp, normalize, wrap, remap, float_equals"
  - "30 Vector2.* raymath method bindings (2 static + 28 instance)"
  - "DEG2RAD / RAD2DEG / EPSILON Float32 module-level constants"
  - "tests/manual/raymath_smoke.iron canonical regression test with 36 call sites + 15 ABI round-trip asserts"
  - "Rule-based discovery: emit_c.c:6710 skips auto-prototype emission for Iron_math_* symbols; raymath scalars must use Iron_rmath_ prefix to coexist with iron_math.h"
affects: [65-02-vector3, 65-03-vector4-matrix-float-helpers, 65-04-quaternion-sweep, 73-polish-performance]

# Tech tracking
tech-stack:
  added: ["raymath.h 2.0 (vendored with raylib 5.5) via RAYMATH_STATIC_INLINE"]
  patterns:
    - "Iron_raylib.c includes raylib.h FIRST then raymath.h (RL_VECTOR2_TYPE guards rely on raylib's pre-declaration)"
    - "RMath namespace (not Math) because iron_math.h reserves the Iron_math_* C symbol prefix"
    - "memcpy-in / memcpy-out per raymath argument; two-locals pattern for cross-type args (Vector2 + Matrix)"
    - "int -> Bool coercion via (bool)(FloatEquals/Vector2Equals(...) != 0)"

key-files:
  created:
    - "tests/manual/raymath_smoke.iron"
    - ".planning/phases/65-raymath/65-01-SUMMARY.md"
  modified:
    - "src/stdlib/iron_raylib.c"
    - "src/stdlib/iron_raylib.h"
    - "src/stdlib/raylib.iron"
    - ".gitignore"

key-decisions:
  - "Iron-side namespace is RMath, not Math, to sidestep iron_math.h's Iron_math_* symbol reservation (emit_c.c:6710 skip-prefix)"
  - "raylib.h is included BEFORE raymath.h in iron_raylib.c (raylib.h defines the RL_VECTOR2_TYPE guard family that raymath.h consumes to skip its duplicate typedefs)"
  - "DEG2RAD / RAD2DEG / EPSILON landed as module-level Float32 vals using single-precision-representable literals (0.017453293, 57.29578, 0.000001)"
  - "Vector2Transform takes Iron_Matrix by value (64 B) with a two-locals memcpy pattern; no -Wlarge-by-value-copy warning (threshold is strictly > 64 bytes)"
  - "Iron-side receiver param named v/start (NEVER self, E0101); C-side shim param named self (C does not reserve it — matches Phase 64 shim style)"

patterns-established:
  - "RMath namespace: object RMath {} + freestanding func RMath.foo -> Iron_rmath_foo — any future raylib-side math surface that collides with iron_math.h uses this prefix"
  - "Matrix-by-value as function ARG: two-locals memcpy, confirmed zero-warning (first raymath function exercising this pattern)"
  - "Iron_math_* prefix reservation: codegen auto-skip means any new scalar math helpers must live either (a) inside iron_math.c/h or (b) under a distinct prefix like Iron_rmath_"

requirements-completed: [MATH-01, MATH-02]

# Metrics
duration: 9min
completed: 2026-04-17
---

# Phase 65 Plan 01: Scalars + Vector2 Summary

**raymath.h included under RAYMATH_STATIC_INLINE, RMath namespace + 6 scalar helpers + 30 Vector2.* methods bound, smoke test prints "ALL MATH-08 ASSERTS PASS"**

## Performance

- **Duration:** 9 min
- **Started:** 2026-04-17T11:41:12Z
- **Completed:** 2026-04-17T11:50:00Z
- **Tasks:** 3
- **Files modified:** 4 (1 new: raymath_smoke.iron; 3 modified: iron_raylib.{c,h}, raylib.iron; plus .gitignore one-liner)

## Accomplishments
- raymath.h is now included in iron_raylib.c under `#define RAYMATH_STATIC_INLINE`. The include comes AFTER `#include "raylib.h"` because raylib.h pre-declares the `RL_VECTOR2_TYPE` guard family (line 167) that raymath.h checks to skip duplicate typedefs. Zero redefinition errors; zero link-time conflict with rcore.c's `RAYMATH_IMPLEMENTATION`.
- `object RMath {}` + 6 freestanding scalar helpers (`clamp`, `lerp`, `normalize`, `wrap`, `remap`, `float_equals`) bound with C shims forwarding to raymath's `Clamp/Lerp/Normalize/Wrap/Remap/FloatEquals`. FloatEquals int→Bool coercion via `(bool)(FloatEquals(x,y) != 0)`.
- `DEG2RAD`, `RAD2DEG`, `EPSILON` Float32 module-level constants landed using single-precision-representable literals.
- 30 Vector2.* method bindings (`zero/one` static + 28 instance: add/subtract/length/dot_product/distance/angle/scale/multiply/negate/divide/normalize/transform/lerp/reflect/min/max/rotate/move_towards/invert/clamp/clamp_value/equals/refract etc.).
- `tests/manual/raymath_smoke.iron` created with 36 call sites (6 RMath.* + 30 Vector2.*) and 15 MATH-08 ABI round-trip assertions. `./build/ironc build` produces a 2,661,744-byte Mach-O arm64 binary; runtime prints `ALL MATH-08 ASSERTS PASS` and exits 0.
- MATH-01 + MATH-02 requirements closed.

## Task Commits

Each task was committed atomically:

1. **Task 1: raymath.h include + RMath namespace + 6 scalar helpers** — `52b6a01` (feat)
2. **Task 2: 30 Vector2 raymath function bindings** — `aafb795` (feat)
3. **Task 3: raymath_smoke.iron + rename Math → RMath** — `de8461f` (feat)

**Plan metadata:** pending final commit (SUMMARY.md + STATE.md + ROADMAP.md)

## Files Created/Modified

- `src/stdlib/iron_raylib.c` — Added `#define RAYMATH_STATIC_INLINE` + `#include "raymath.h"` after raylib.h (lines 30-38). Added 6 Iron_rmath_* scalar shims and 30 Iron_vector2_* method shims under the Phase 65 marker (~310 lines).
- `src/stdlib/iron_raylib.h` — Added 6 + 30 = 36 C prototypes under the Phase 65 marker.
- `src/stdlib/raylib.iron` — Added `object RMath {}`, 3 Float32 constants (DEG2RAD/RAD2DEG/EPSILON), 6 RMath.* scalar stubs, and 30 Vector2.* method stubs.
- `tests/manual/raymath_smoke.iron` — **New file.** Phase 65 canonical regression test with 36 call sites + 15 ABI round-trip assertions.
- `.gitignore` — Added `/raymath_smoke` (same convention as `/collision_smoke` from Phase 64).

## Decisions Made

- **Namespace rename: Math → RMath.** The plan specified `object Math {}`, but Iron's stdlib already has `object Math` in `src/stdlib/math.iron` (with PI/TAU/E + sin/cos/lerp/etc.), and `emit_c.c:6710` auto-skips prototype emission for any symbol starting with `Iron_math_` because `iron_math.h` is always auto-included by the codegen. Naming our namespace `Math` would have caused (a) undeclared-function errors for clamp/wrap/remap/normalize/float_equals (no prototype emitted by codegen; no declaration in iron_math.h) and (b) a signature collision for `Iron_math_lerp` (iron_math.h declares `double Iron_math_lerp(double, double, double)` and our stub would emit `float Iron_math_lerp(float, float, float)`). `RMath` lowers to `Iron_rmath_*`, which sidesteps both issues and preserves the raylib R-prefix symmetry (raylib/raymath/rlgl).
- **Include order: raylib.h BEFORE raymath.h.** The plan's written include order was `define RAYMATH_STATIC_INLINE → include raymath.h → include raylib.h`. At compile time this produced 8 redefinition errors because raymath.h declares Vector2/3/4/Matrix typedefs inside `#if !defined(RL_VECTOR2_TYPE)` guards that only raylib.h sets (line 167). The correct order includes raylib.h first (sets the guards), then the define, then raymath.h (guards trip → typedefs skipped). Verified: `clang -c` exits 0 with zero warnings.
- **DEG2RAD/RAD2DEG/EPSILON constants landed.** Single-precision-representable literals (0.017453293, 57.29578, 0.000001) accepted by `Float32(...)` cast without narrowing warnings. Plan had a fallback to defer these to Phase 73 — not triggered.
- **Smoke test reference values.** Hand-picked 15 assertions spanning every return-shape exercised by Plan 65-01: Float32 scalar (lerped/clamped/wrapped/remap1/normd/v_len/v_lensq/v_dot/v_distsq), Bool (feq/v_eq), Vector2 field access (v_add.x/v_add.y/v_zero.x/v_one.x). Covers MATH-08 ABI round-trip for every primitive return shape this plan introduces.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Reversed include order: raylib.h before raymath.h**
- **Found during:** Task 1 (raymath.h header inclusion)
- **Issue:** Plan-specified order `#define RAYMATH_STATIC_INLINE → #include "raymath.h" → #include "raylib.h"` caused 8 clang redefinition errors (Vector2, Vector3, Vector4, Matrix typedefs declared in both headers). raymath.h guards its typedefs with `#if !defined(RL_VECTOR2_TYPE)` etc., but raylib.h only SETS those guards at line 167 — so including raymath.h first means the guards are not yet set, both headers declare the typedefs, and raylib.h's subsequent unconditional declarations collide.
- **Fix:** Reordered to `#include "raylib.h"` first (declares typedefs + sets RL_*_TYPE guards), then `#define RAYMATH_STATIC_INLINE`, then `#include "raymath.h"` (sees the guards set, skips its duplicate typedefs cleanly).
- **Files modified:** src/stdlib/iron_raylib.c
- **Verification:** `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0 with zero warnings.
- **Committed in:** 52b6a01 (Task 1 commit)
- **Impact on future plans:** Plans 65-02/03/04 inherit this order (no re-edit needed). RESEARCH.md claimed "raylib.h does NOT include raymath.h" which is true, but missed the bidirectional dependency: raymath.h includes raylib.h-defined macros indirectly via RL_*_TYPE guards.

**2. [Rule 3 - Blocking] Renamed `object Math` → `object RMath` to avoid iron_math.h collision**
- **Found during:** Task 3 (ironc build of raymath_smoke.iron)
- **Issue:** First ironc build failed with 5 errors: `call to undeclared function 'Iron_math_clamp' / 'Iron_math_wrap' / 'Iron_math_remap' / 'Iron_math_normalize' / 'Iron_math_float_equals'` and a signature-conflict note for `Iron_math_lerp` ("declared here" in iron_math.h:31 as `double Iron_math_lerp(double, double, double)`). Root cause: `emit_c.c:6710` maintains a prefix skip-list (`Iron_math_`, `Iron_io_`, `Iron_time_`, `Iron_log_`, `Iron_hint_`, `Iron_string_`, `Iron_list_`, `Iron_array_`) that suppresses auto-prototype emission for any stub whose mangled C name starts with one of those prefixes — the assumption is that the corresponding header (e.g., `stdlib/iron_math.h`) declares them. Our new 6 shims used names `Iron_math_clamp` etc., which matched the skip prefix but were NOT actually declared in iron_math.h → codegen silently dropped the prototype and clang failed to find them. Additionally, iron_math.h already declares `Iron_math_lerp` with a **double-precision** signature (for `math.iron`'s `Math.lerp(a: Float, b: Float, t: Float)`), so even if our prototype had been emitted, it would have clashed.
- **Fix:** Renamed Iron-side namespace `Math` → `RMath` (raylib-math). C symbols lowered from `Iron_math_*` to `Iron_rmath_*`, which does NOT match any prefix in the `k_header_declared_prefixes` list, so codegen auto-emits prototypes normally. The `R` prefix matches raylib's own convention (raylib/raymath/rlgl). Updated raylib.iron stubs, iron_raylib.h prototypes, iron_raylib.c shims, and the smoke test call sites consistently.
- **Files modified:** src/stdlib/raylib.iron, src/stdlib/iron_raylib.h, src/stdlib/iron_raylib.c, tests/manual/raymath_smoke.iron
- **Verification:** `./build/ironc build tests/manual/raymath_smoke.iron` exits 0 (no undeclared errors); `./raymath_smoke` prints `ALL MATH-08 ASSERTS PASS` and exits 0.
- **Committed in:** de8461f (Task 3 commit — combined with smoke test creation)
- **Impact on future plans:** Plans 65-02/03/04 add Vector3/4/Matrix/Quaternion methods, none of which use the Math/RMath namespace — no downstream rename needed. The RMath naming is permanent; future raymath phases that add scalar helpers will continue to use `object RMath {}` / `func RMath.foo -> Iron_rmath_foo`. CONTEXT.md should be updated (next planner pass) to document the `Iron_math_` prefix reservation.

---

**Total deviations:** 2 auto-fixed (both Rule 3 - blocking)
**Impact on plan:** Both auto-fixes were strictly necessary to make the plan compile at all. No scope creep — renames preserved the 36-call-site surface the plan specified. Novel discovery: the `Iron_math_` symbol prefix reservation in emit_c.c:6710 is now documented for future phases.

## Issues Encountered

- **Clang `-Wlarge-by-value-copy` on Matrix (64 B):** Zero warnings, as predicted by RESEARCH.md Pitfall 6. Matrix is exactly 64 bytes and the default threshold fires at strictly > 64. `Iron_vector2_transform` passes `struct Iron_Matrix mat` by value without triggering the warning.
- **Float32 literal narrowing:** No warnings on `Float32(0.017453293)` / `Float32(57.29578)` / `Float32(0.000001)`. RESEARCH.md Pitfall 9's fallback (deferring DEG2RAD constants to Phase 73) was NOT triggered — constants landed first try.
- **E0101 `self` reserved:** Zero recurrences. All 30 Vector2 method stubs use `v` / `start` / `v1` / `v2` as receiver names; Iron parser never complained. The plan's explicit receiver-naming guidance in the `<read_first>` block for Task 2 was effective.

## Next Phase Readiness

- **Plan 65-02 (Vector3, 40 functions) fully unblocked.** RAYMATH_STATIC_INLINE inclusion is live; the memcpy-in/memcpy-out template scales to Vector3 (12 B) verbatim; the cross-type arg pattern (Matrix as second arg) is already proven in `Iron_vector2_transform`.
- **Plan 65-03 (Vector4 + Matrix + Float3/Float16) unblocked.** First Matrix RETURN type lives there (not exercised this plan; only Matrix as ARG). Float3/Float16 type additions are the first post-Phase-60 type-grid growth.
- **Plan 65-04 (Quaternion + MATH-07/08 sweep) unblocked.** 3-tuple auto-emit probe (`MatrixDecompose -> (Vector3, Quaternion, Vector3)`) lives there.
- **CONTEXT.md + RESEARCH.md gaps to flag:** (a) `Iron_math_` prefix reservation (emit_c.c:6710) was missed by research; RMath rename is now permanent. (b) raylib.h include-order requirement (RL_VECTOR2_TYPE guards) was documented as "raylib.h does NOT include raymath.h" (true but incomplete — raymath.h DOES read macros that raylib.h sets). Both should feed into Plan 65-02's CONTEXT inheritance.
- **Consumer-file impact:** pong.iron / game_raylib.iron / hello_raylib.iron unchanged — none reference raymath. Confirmed by: no `PHASE 65` markers exist in consumer Iron files (plan-verified prerequisite).

## ironc Invocations

**1 invocation** — Task 3 canonical smoke build (`./build/ironc build tests/manual/raymath_smoke.iron`, 3.06s wall-clock, within budget). Matches HANDOFF.md discipline of 1 invocation per plan.

## rcore.c Sanity Check

Not explicitly re-compiled this plan — `RAYMATH_IMPLEMENTATION` still defined at line 116 (unchanged), `RAYMATH_STATIC_INLINE` only in iron_raylib.c. Since iron_raylib.c `clang -c` and iron_raylib_layout.c `clang -c` both exit 0, and rcore.c's include graph doesn't intersect iron_raylib.c at the TU level, a separate rcore.c compile is redundant for this plan. Full link-time validation happens in Phase 65-04's end-to-end sweep.

## Self-Check: PASSED

- `src/stdlib/iron_raylib.c` — MODIFIED (raymath include + 36 shims). FOUND.
- `src/stdlib/iron_raylib.h` — MODIFIED (36 C protos). FOUND.
- `src/stdlib/raylib.iron` — MODIFIED (object RMath + 6 scalar stubs + 30 Vector2 stubs + 3 constants). FOUND.
- `tests/manual/raymath_smoke.iron` — CREATED. FOUND.
- `.gitignore` — MODIFIED (+/raymath_smoke). FOUND.
- Task 1 commit `52b6a01` — FOUND (`git log --oneline --all | grep 52b6a01`).
- Task 2 commit `aafb795` — FOUND.
- Task 3 commit `de8461f` — FOUND.
- Smoke test runtime exit 0 with `ALL MATH-08 ASSERTS PASS` — VERIFIED.
- `clang -c iron_raylib.c -Wall -Wextra` exit 0 — VERIFIED.
- `clang -c iron_raylib_layout.c -Wall -Wextra` exit 0 (392 asserts unchanged) — VERIFIED.

---
*Phase: 65-raymath*
*Completed: 2026-04-17*
