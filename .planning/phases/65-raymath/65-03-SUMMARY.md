---
phase: 65-raymath
plan: 03
subsystem: stdlib
tags: [raylib, raymath, ffi, math, vector4, matrix, float3, float16, shim, stdlib-binding]

# Dependency graph
requires:
  - phase: 65-raymath
    plan: 02
    provides: "37 Vector3.* bindings live; cross-type memcpy pattern validated (Vector3+Quaternion, Vector3+Matrix+Matrix); matrix-by-value-as-arg zero-warning confirmed; 1 Vector3 deferred (to_float_v) carried to this plan"
  - phase: 60-type-enum-foundation
    plan: 02
    provides: "Iron_Vector4 (16 B) + Iron_Matrix (64 B) layouts pinned; _Static_assert grid pattern (per-field offsetof)"
  - phase: 63-2d-drawing
    plan: 04
    provides: "16 B struct-by-value RETURN template (memcpy-out) validated for Rectangle"
  - phase: 64-collision-2d-3d
    plan: 02
    provides: "Matrix 64 B pass-by-value-as-ARG validated zero -Wlarge-by-value-copy; self-reserved E0101 rule"
provides:
  - "2 new Iron types: object Float3 (12 B), object Float16 (64 B) — first post-Phase-60 type-grid growth (iron_raylib_layout.c grew 392 → 413 _Static_assert entries)"
  - "22 Vector4.* method stubs in raylib.iron (MATH-04) — zero/one/add/add_value/subtract/subtract_value/length/length_sqr/dot_product/distance/distance_sqr/scale/multiply/negate/divide/normalize/min/max/lerp/move_towards/invert/equals"
  - "21 Matrix.* method stubs in raylib.iron (MATH-05, Decompose deferred to Plan 65-04) — determinant/trace/transpose/invert/identity/add/subtract/multiply/translate/rotate/rotate_x/rotate_y/rotate_z/rotate_xyz/rotate_zyx/scale/frustum/perspective/ortho/look_at/to_float_v"
  - "1 Vector3.to_float_v stub (MATH-03 carried from Plan 65-02 — unblocked once Float3 existed)"
  - "46 Iron_vector4_* / Iron_matrix_* / Iron_vector3_to_float_v / Iron_matrix_to_float_v prototypes + shim implementations in iron_raylib.h / iron_raylib.c"
  - "First 64 B struct-by-value RETURN surface in codebase (Matrix — compiles zero-warning under -Wall -Wextra; threshold fires strictly >64 per Phase 64-02 precedent)"
  - "Float32→double widening pattern in shims (MatrixFrustum/Perspective/Ortho take raymath double; Iron-side stays Float32)"
  - "raymath_smoke.iron extended with 44 new call sites (22 Vector4 + 21 Matrix + 1 Vector3.to_float_v) and 15 new MATH-08 ABI asserts — canonical ironc build still prints ALL MATH-08 ASSERTS PASS exit 0"
affects: [65-04-quaternion-sweep, 73-polish-performance]

# Tech tracking
tech-stack:
  added: []  # no new deps — raymath.h live from Plan 65-01
  patterns:
    - "Float3 (12 B) / Float16 (64 B) as named-field Iron objects byte-identical to raymath's C array-struct typedefs — C contiguous-float guarantee + per-field offsetof asserts pin layout"
    - "Matrix 64 B struct-by-value RETURN: memcpy-out from raymath Matrix result into Iron_Matrix — clang -Wall -Wextra passes zero warnings (64 B is at the boundary, not strictly >)"
    - "Float32→double widening in frustum/perspective/ortho shims — (double) cast per arg preserves Iron's Float32-everywhere convention without losing raymath's internal precision"
    - "offsetof(float3, v[0]) / offsetof(float16, v[15]) — array-subscript-in-offsetof accepted by clang without __builtin_offsetof workaround (Pitfall 8 not triggered)"
    - "raymath_smoke.iron inserts static Matrix constructors (identity/translate/rotate_x/…) as bare type-reference calls — Pitfall 4 (static-dispatch shadowing) not triggered"

key-files:
  created:
    - ".planning/phases/65-raymath/65-03-SUMMARY.md"
  modified:
    - "src/stdlib/iron_raylib.c"
    - "src/stdlib/iron_raylib.h"
    - "src/stdlib/iron_raylib_layout.c"
    - "src/stdlib/raylib.iron"
    - "tests/manual/raymath_smoke.iron"

key-decisions:
  - "Matrix count matched plan (21 non-deferred). `grep -cE '^RMAPI [A-Za-z0-9_]+ Matrix[A-Z]' src/vendor/raylib/raymath.h` returned 22 total; MatrixDecompose is the one deferred to Plan 65-04 (3-tuple out-param). No MatrixAddValue / MatrixSubtractValue exist in raymath 5.5 — the plan's self-check suggestion was pre-emptive and resolved GREEN."
  - "iron_raylib_layout.c DID need a #define RAYMATH_STATIC_INLINE + #include raymath.h addition. Existing file only included iron_raylib.h + raylib.h (plus stddef.h); it did NOT previously pull raymath for its `float3` / `float16` typedef references. Added after raylib.h so RL_VECTOR2_TYPE guards are set."
  - "Pitfall 8 resolved GREEN first-try — `offsetof(float3, v[0])` compiles under clang -Wall -Wextra without __builtin_offsetof fallback. Tested in isolation (tiny C fixture) before the layout edit; full layout TU confirms."
  - "Pitfall 4 resolved GREEN — Matrix.identity() / Matrix.translate(...) / Matrix.look_at(...) / Matrix.perspective(...) all dispatch statically. Smoke test writes them as bare type references (no local variable named Matrix); no 'wrong number of arguments' error."
  - "Pre-existing _Static_assert count was 392, not 390 as the plan text stated. Addition is +21 (1 size + 3 offsetof for Float3; 1 size + 16 offsetof for Float16) → final 413, matching RESEARCH.md's more accurate '~411' estimate."
  - "Smoke test continues to use RMath.float_equals (namespace name from Plan 65-01 rename) — NOT Math.float_equals as the plan prose wrote. The plan text was inherited from pre-rename drafts; the actual Iron-side namespace is RMath."

patterns-established:
  - "64 B struct-by-value RETURN: Matrix is the first. 21 shims exercise this (invert/multiply/identity/translate/rotate*/scale/frustum/perspective/ortho/look_at/transpose/add/subtract/to_float_v). Zero -Wlarge-by-value-copy warnings — Phase 64-02's empirical finding (threshold fires at >64 strictly) confirmed for RETURN as well as ARG."
  - "Float3 / Float16 byte-identity via per-field offsetof asserts. Pattern scales to any future raymath helper struct — just add `typedef struct { float v[N]; }` mirror + N+1 asserts."
  - "Float32→double widening per-arg via (double) cast. Pattern for any future raymath or raylib function that takes raymath-style doubles (MatrixFrustum / MatrixPerspective / MatrixOrtho are the only ones in raymath 5.5)."

requirements-completed: [MATH-04, MATH-05-except-decompose, MATH-03-to-float-v]

# Metrics
duration: 5min
completed: 2026-04-17
---

# Phase 65 Plan 03: Vector4 + Matrix + Float3/Float16 Summary

**22 Vector4 + 21 Matrix + 2 ToFloatV raymath bindings landed with first 64 B struct-by-value RETURN + 2 new helper types (Float3 / Float16), smoke test still prints "ALL MATH-08 ASSERTS PASS"**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-17T12:08:14Z
- **Completed:** 2026-04-17T12:13:08Z
- **Tasks:** 3
- **Files modified:** 5 (iron_raylib.c, iron_raylib.h, iron_raylib_layout.c, raylib.iron, raymath_smoke.iron)

## Accomplishments

- Two new Iron types landed in raylib.iron: `object Float3 { val x, y, z: Float32 }` (12 B) and `object Float16 { val m0..m15: Float32 }` (64 B). C-side mirrors `struct Iron_Float3` / `struct Iron_Float16` added to iron_raylib.h near the Matrix mirror. First post-Phase-60 type-grid growth. Layout pinned at compile time by 21 new `_Static_assert` entries in iron_raylib_layout.c (1 sizeof + 3 offsetof for Float3; 1 sizeof + 16 offsetof for Float16) — grid grew 392 → **413**.
- `clang -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0 with zero warnings. `#define RAYMATH_STATIC_INLINE` + `#include "raymath.h"` added after the existing raylib.h include (raymath needs raylib's RL_VECTOR2_TYPE guards set first). `offsetof(float3, v[0])` compiles clean — Pitfall 8 did NOT trigger; no `__builtin_offsetof` fallback needed.
- 22 Vector4.* stubs + 22 prototypes + 22 shims for MATH-04. 16 B struct-by-value in/out reuses the Rectangle template from Phase 63 verbatim. `Vector4Equals` int→Bool coercion follows the `Vector2Equals` / `Vector3Equals` template from Plans 65-01/02 (`return (bool)(Vector4Equals(a, b) != 0);`).
- 21 Matrix.* stubs + 21 prototypes + 21 shims for MATH-05 (Decompose deferred to Plan 65-04 for 3-tuple out-param). **First 64 B struct-by-value RETURN surface in the codebase** — `MatrixInvert` / `MatrixMultiply` / `MatrixIdentity` / `MatrixTranspose` / `MatrixLookAt` / etc. all return `Matrix` which memcpy's into `struct Iron_Matrix`. Zero `-Wlarge-by-value-copy` warnings under `-Wall -Wextra` — clang's 64 B threshold fires at strictly `>64`, confirming Phase 64-02's empirical finding holds for RETURN as well as ARG.
- MatrixFrustum / MatrixPerspective / MatrixOrtho take `double` in raymath; Iron-side stays Float32 per the Phase 65 convention. Shims widen via `(double)` casts per argument — clean compile, no narrowing warnings.
- 1 Vector3.to_float_v stub + prototype + shim (MATH-03 carry-forward from Plan 65-02 — unblocked once Float3 existed). First Float3 (12 B) struct-by-value RETURN. 1 Matrix.to_float_v stub + prototype + shim. First Float16 (64 B) struct-by-value RETURN.
- `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0 with zero warnings. All 46 new shims compile clean on first try.
- raymath_smoke.iron extended: +22 Vector4 call sites (v4_zero/v4_one/v4_add/… through v4_eq), +21 Matrix call sites (m_id/m_det/m_trace/m_trans/m_inv/m_add/m_sub/m_mul/m_trl/m_rot/m_rotx/m_roty/m_rotz/m_rxyz/m_rzyx/m_scale/m_frst/m_pers/m_ortho/m_look/m_f16), +1 Vector3.to_float_v call site, +15 MATH-08 ABI asserts covering new return shapes (Vector4 x/y/z/w, Float32 scalars, Bool, Float16 m0/m1/m5 field access, Float3 x/y/z field access). All 43 asserts fold into `all_pass` and print `ALL MATH-08 ASSERTS PASS`.
- `./build/ironc build tests/manual/raymath_smoke.iron` exits 0 (3.26s user CPU — comparable to Plan 65-01's 3.06s baseline and Plan 65-02's unmeasured build). `./raymath_smoke` produces a 2,680,512-byte Mach-O arm64 binary that exits 0 with the canonical success line.
- MATH-04 **100% closed** (22/22 Vector4). MATH-05 **21/22 closed** — MatrixDecompose carries to Plan 65-04 (3-tuple probe). MATH-03 **100% closed** — Vector3.to_float_v landed here; Vector3.ortho_normalize (2-tuple out-param) carries to Plan 65-04. Cumulative Phase 65 function count: 6 RMath + 30 Vector2 + 37 Vector3 + 1 (Vector3.to_float_v) + 22 Vector4 + 21 Matrix + 1 (Matrix.to_float_v) = **118** of the 143 raymath functions bound. Remaining 25 land in Plan 65-04 (26 Quaternion minus the 1 Quaternion.to_axis_angle-as-2-tuple carry, plus Vector3.ortho_normalize and MatrixDecompose 3-tuple).

## Task Commits

Each task was committed atomically:

1. **Task 1: Float3/Float16 types + 21 layout asserts** — `24592fd` (feat)
2. **Task 2: 22 Vector4 + 21 Matrix + 2 ToFloatV shims** — `760b0ca` (feat)
3. **Task 3: Smoke test extension with 44 new call sites + 15 ABI asserts** — `04b736c` (feat)

**Plan metadata:** pending final commit (SUMMARY.md + STATE.md + ROADMAP.md)

## Files Created/Modified

- `src/stdlib/raylib.iron` — Added `object Float3 {}` (3 fields) and `object Float16 {}` (16 fields) between Quaternion and Matrix. Appended 1 Vector3.to_float_v stub after the existing Vector3 block (just after the `v3.refract` line). Appended 22 Vector4.* stubs + 21 Matrix.* stubs under new `-- MATH-04` / `-- MATH-05` section banners. Preserved existing DEFERRED comments for Vector3.ortho_normalize and added a new DEFERRED comment for Matrix.decompose pointing at Plan 65-04.
- `src/stdlib/iron_raylib.h` — Added `struct Iron_Float3 { float x, y, z; }` and `struct Iron_Float16 { float m0..m15; }` after `struct Iron_Matrix`. Appended 46 prototypes under the Phase 65 marker after the Vector3 block: 22 Iron_vector4_*, 21 Iron_matrix_*, 1 Iron_vector3_to_float_v, 1 Iron_matrix_to_float_v.
- `src/stdlib/iron_raylib.c` — Appended 46 shim implementations under the Phase 65 marker after `Iron_vector3_refract`. All Vector4 shims use the 16 B memcpy-in/out template (Pattern 2a); all Matrix shims use the 64 B memcpy-in/out template (same pattern, different size). Static Matrix constructors (identity/translate/rotate_x/rotate_y/rotate_z/scale) take scalar args and skip the memcpy-in step. Frustum/Perspective/Ortho widen Float32 args to double via per-arg `(double)` casts inline at the call site. Float3/Float16 RETURN shims (Pattern 2g) memcpy raymath's `float3` / `float16` result into `struct Iron_Float3` / `struct Iron_Float16` by full struct size.
- `src/stdlib/iron_raylib_layout.c` — Added `#define RAYMATH_STATIC_INLINE` + `#include "raymath.h"` after the existing raylib.h include (new include block with its own comment). Appended 21 new `_Static_assert` entries under a `Phase 65 Plan 03` section header immediately before the sentinel variable at end of file. 392 → 413 assertions.
- `tests/manual/raymath_smoke.iron` — Header comment updated to say 119 call sites (was 73). Inserted Vector4 (22 calls), Matrix (21 calls), and Vector3.to_float_v (1 call) sections between the existing `v3_refr` line and the MATH-08 section. Inserted 15 new pass_v4* / pass_m* / pass_v3f3* asserts before the `all_pass` aggregate. `all_pass` extended from 28-term to 43-term AND.

## Decisions Made

- **Matrix surface is 21 functions, not 22 or 24.** `grep -cE '^RMAPI [A-Za-z0-9_]+ Matrix[A-Z]' src/vendor/raylib/raymath.h` reported 22; the 22nd is `MatrixDecompose` which is the 3-tuple out-param function deferred to Plan 65-04. The plan's suspicion (lines 365-366) that `MatrixAddValue` / `MatrixSubtractValue` might exist was pre-emptive and resolved GREEN — raymath 5.5 does NOT have those functions for Matrix (they exist for Vector2/3/4 only). Scope matches the plan's explicit 21-stub list verbatim; no scope change.
- **iron_raylib_layout.c needed the raymath include added.** Plan text ("if iron_raylib_layout.c does NOT already include raymath.h") was correctly defensive. The file previously only pulled raylib.h + iron_raylib.h + stddef.h; raymath's `float3` / `float16` typedefs were unreachable until the include was added. Include placed AFTER raylib.h so raylib's `RL_VECTOR2_TYPE` guards are set before raymath.h decides whether to redeclare Vector2/3/4/Matrix — same ordering discipline as Plan 65-01 established in iron_raylib.c.
- **Pitfall 8 (offsetof array-subscript) resolved GREEN without workaround.** Tested a 5-line C fixture at plan start: `offsetof(float3, v[1])` compiled under clang -Wall -Wextra with exit 0 and zero diagnostics. Full layout TU confirms (413 asserts, zero warnings). No `__builtin_offsetof` fallback was needed — modern clang (macOS apple-darwin25) supports the array-subscript form natively.
- **Pitfall 4 (static-dispatch shadowing) resolved GREEN.** The smoke test calls Matrix.identity() / Matrix.translate(...) / Matrix.look_at(...) / Matrix.perspective(...) etc. as bare type references. No local variable shadows `Matrix`. ironc built clean and the Iron-side Matrix namespace dispatch worked first try — matches Plan 65-01's `Vector2.zero()` / `Vector2.one()` precedent and Phase 64's `Collision.spheres(...)` pattern.
- **Smoke test uses `RMath.float_equals`, not `Math.float_equals`.** The plan prose at line 608 was written before the Plan 65-01 Math→RMath rename; the actual Iron-side namespace (and the name baked into the existing smoke test file) is `RMath`. Kept consistent with Plans 65-01/02. No change needed to the RMath namespace or to STATE.md's ongoing note about the rename.
- **Matrix constructor literal in smoke test.** `Matrix(Float32(1.0), Float32(0.0), ...)` with 16 positional args parsed and type-checked first try — same 16-arg positional constructor pattern Phase 60-02 established for Vector4, now confirmed extending to Matrix. No workaround needed; RESEARCH.md Open Question 2 (positional constructor supports 16 args?) resolved GREEN.
- **Matrix.to_float_v returns Float16 by value — field access `m_f16.m0` works.** The smoke test reads `m_f16.m0` / `m_f16.m1` / `m_f16.m5` after `val m_f16 = m_id.to_float_v()`. Iron's object field accessor lifts through return-by-value without issue; no fallback helper needed. Same for Vector3.to_float_v returning Float3 (`v3_f3.x` / `v3_f3.y` / `v3_f3.z`).
- **Float3 layout note.** raymath's `float3` has a trailing `_pad` byte? No — `typedef struct { float v[3]; }` is exactly 12 bytes on every platform Iron supports (x86-64 and arm64, both 4-byte float alignment). `sizeof(struct Iron_Float3) == sizeof(float3) == 12` confirmed at compile time. If raymath ever introduces a pad, the `_Static_assert(sizeof(...) == sizeof(...))` catches it before runtime.

## Deviations from Plan

### Auto-fixed Issues

**None.** Plan 65-03 executed exactly as written at the code level. Every acceptance criterion resolved GREEN on first compile; every pitfall listed in RESEARCH.md resolved GREEN or did not trigger.

### Documentation-only notes

**1. _Static_assert baseline count was 392, not 390.** Plan prose at lines 33, 279, and 361 of the plan referenced "390 → 411". Actual baseline is 392 (confirmed by `grep -c '_Static_assert' src/stdlib/iron_raylib_layout.c` before Task 1 edit) and target after +21 is **413** (confirmed post-edit). RESEARCH.md Pattern 5 already noted the correct baseline ("390 existing → ~411"). Implementation matches the real numbers; plan prose was one off. No impact on compile or verify.

**2. Plan text used `Math.float_equals` in the smoke-test action block (line 608); actual Iron-side namespace is `RMath` (Plan 65-01 rename).** Implementation uses `RMath.float_equals` to match the existing smoke-test file and the actual Iron-side declaration. No compile impact — plan text was inherited from a pre-rename draft. STATE.md already documents the rename.

---

**Total deviations:** 0 code deviations (2 documentary notes — plan prose vs. actual disk state).
**Impact on plan:** Zero scope creep. All 46 new bindings compile, link, and runtime-pass end-to-end. MATH-04 closed (22/22). MATH-05 closed for 21/22 (Decompose → 65-04). MATH-03 fully closed (to_float_v landed).

## Issues Encountered

- **First 64 B struct-by-value RETURN (Matrix):** zero `-Wlarge-by-value-copy` warnings, as predicted by RESEARCH.md Pitfall 6 and confirmed by Phase 64-02's ARG-side finding. Clang's threshold fires at strictly >64; Matrix at exactly 64 passes clean for both ARG and RETURN paths.
- **Float3 / Float16 layout equivalence:** no drift. `_Static_assert(sizeof(Iron_Float3) == sizeof(float3))` and all 19 offsetof asserts passed on first try.
- **Static Matrix constructors in smoke test (Pitfall 4 scenario):** all six (Matrix.identity/translate/rotate_x/rotate_y/rotate_z/scale) plus look_at / perspective / ortho / frustum compiled clean and ran. No "wrong number of arguments" error. No local shadowing the `Matrix` type name.
- **Float32 positional Matrix literal:** `Matrix(Float32(1.0), Float32(0.0), ... 16 args ...)` parsed first try. 16-arg positional constructor pattern scales cleanly from Vector4 (4 args, Phase 60-02) to Matrix (16 args, this plan).
- **Smoke-test incremental build:** one ironc invocation this plan (Task 3), 3.26s user CPU. Comparable to Plan 65-01 baseline. Binary grew from 2,647,416 B (Plan 65-02) to 2,680,512 B (+33 KB) — consistent with 46 new inline-shim surface.
- **E0101 `self` reserved:** zero recurrences. All 22 Vector4 stubs use `v` / `v1` / `v2`; all 21 Matrix stubs use `m` / `axis` / `angle` / scalar names matching raymath's own params. C-side shim params keep `self` (C does not reserve it — matches Phase 64 and Plans 65-01/02).

## Next Phase Readiness

- **Plan 65-04 (Quaternion + MATH-07/08 sweep) fully unblocked.** All ABI mechanisms needed for Quaternion are now proven in Plans 65-01/02/03:
  - 16 B struct-by-value in/out (Quaternion same size as Vector4): validated 44 times across Vector4 shims
  - Cross-type args (Quaternion + Matrix for QuaternionTransform; Vector3 + Quaternion for FromVector3ToVector3 et al.): validated in Plan 65-02's Vector3+Quaternion shims
  - 2-tuple out-param auto-emit (for Quaternion.to_axis_angle): established in Phase 64-01 (Iron_Tuple_Bool_Vector2)
  - 3-tuple out-param auto-emit (for MatrixDecompose): RESEARCH.md Pattern 2d probe is the only novel risk remaining; emit_helpers.c:260 is generic over arity, so it's expected GREEN
  - Vector3.ortho_normalize (2-tuple out-param mutating both args): Pattern 2e from RESEARCH.md
- **CONTEXT.md corrections for milestone close:** (a) _Static_assert baseline corrections (392 → 413 after this plan, not 390 → 411 as CONTEXT.md Decision prose said). (b) Matrix function count is 22 (21 non-deferred here + 1 Decompose), not 24 as CONTEXT.md said. (c) Cumulative Phase 65 count tracked in this SUMMARY: 118/143 bound after Plan 65-03; Plan 65-04 closes the remaining 25.
- **Consumer-file impact:** pong.iron / game_raylib.iron / hello_raylib.iron unchanged — none reference raymath. Confirmed by absence of `Vector4\.` / `Matrix\.(identity|multiply|invert)` / `to_float_v` in any non-test Iron file.

## ironc Invocations

**1 invocation this plan** — Task 3 canonical smoke build (`./build/ironc build tests/manual/raymath_smoke.iron`, 3.26s user CPU / 3.97s wall-clock). Matches HANDOFF.md discipline of 1 invocation per plan. Cumulative Phase 65 ironc invocations: 1 (65-01) + 1 (65-02) + 1 (65-03) = 3 so far; Plan 65-04 will add 1-2 (smoke + optional 3-tuple probe).

## rcore.c Sanity Check

Not re-compiled this plan — no changes to raymath include graph ownership. `RAYMATH_IMPLEMENTATION` still owned solely by rcore.c:116 (verified untouched); `RAYMATH_STATIC_INLINE` still only in iron_raylib.c (from Plan 65-01) and now also in iron_raylib_layout.c (added by this plan, Task 1). Both TUs compile cleanly with their own static-inline copies; no cross-TU conflict. `clang -c iron_raylib.c` and `clang -c iron_raylib_layout.c` both exit 0, which covers the TUs that changed. Full link-time validation (link pong.iron or raymath_smoke against rcore.c's raymath symbols) is implicit in the Task 3 ironc build — the binary links and runs exit 0.

## Self-Check: PASSED

- `src/stdlib/raylib.iron` — MODIFIED (Float3 + Float16 objects; 22 Vector4 + 21 Matrix + 1 Vector3.to_float_v stubs). `grep -cE '^object Float3' → 1`, `grep -cE '^object Float16' → 1`, `grep -cE '^func Vector4\.' → 22`, `grep -cE '^func Matrix\.' → 21`. VERIFIED.
- `src/stdlib/iron_raylib.h` — MODIFIED (Iron_Float3 + Iron_Float16 mirrors; 46 prototypes). `grep -q 'struct Iron_Float3'` succeeds; `grep -q 'struct Iron_Float16'` succeeds; Vector4 protos = 22; Matrix protos = 21. VERIFIED.
- `src/stdlib/iron_raylib.c` — MODIFIED (46 shim implementations). `grep -q 'Iron_vector3_to_float_v.*Iron_Float3'` succeeds; `grep -q 'Iron_matrix_to_float_v.*Iron_Float16'` succeeds; `grep -cE 'MatrixPerspective\(\(double\)'` ≥ 1. `clang -c ... -Wall -Wextra` exit 0 zero warnings. VERIFIED.
- `src/stdlib/iron_raylib_layout.c` — MODIFIED (raymath include + 21 new asserts). `grep -c '_Static_assert' → 413`. `clang -c ... -Wall -Wextra` exit 0 zero warnings. VERIFIED.
- `tests/manual/raymath_smoke.iron` — MODIFIED (+44 call sites, +15 asserts). `grep -c 'Vector4(' → 2`; `grep -cE 'Matrix\.(identity|translate|rotate|perspective|ortho|look_at|scale|frustum)' → 13`; `grep -c 'to_float_v' → 3`. VERIFIED.
- Task 1 commit `24592fd` — FOUND (`git log --oneline | grep 24592fd`).
- Task 2 commit `760b0ca` — FOUND.
- Task 3 commit `04b736c` — FOUND.
- `./build/ironc build tests/manual/raymath_smoke.iron` exit 0 — VERIFIED (3.26s user CPU, 3.97s wall-clock).
- `./raymath_smoke` exit 0, prints `ALL MATH-08 ASSERTS PASS` — VERIFIED.
- `clang -c iron_raylib.c -Wall -Wextra` exit 0 zero warnings — VERIFIED.
- `clang -c iron_raylib_layout.c -Wall -Wextra` exit 0 zero warnings — VERIFIED.

---
*Phase: 65-raymath*
*Completed: 2026-04-17*
