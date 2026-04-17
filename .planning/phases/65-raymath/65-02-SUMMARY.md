---
phase: 65-raymath
plan: 02
subsystem: stdlib
tags: [raylib, raymath, ffi, math, vector3, quaternion, matrix, shim, stdlib-binding]

# Dependency graph
requires:
  - phase: 65-raymath
    plan: 01
    provides: "RAYMATH_STATIC_INLINE include live in iron_raylib.c (AFTER raylib.h due to RL_VECTOR2_TYPE guard dependency); RMath namespace; memcpy-in/memcpy-out template validated on Vector2 (8 B); Matrix-as-arg pass-by-value validated zero-warning in Iron_vector2_transform"
  - phase: 64-collision-2d-3d
    plan: 02
    provides: "3-memcpy per cross-type shim (Iron_ray_hit_mesh Ray+Mesh+Matrix); Matrix 64 B pass-by-value zero-warning confirmation; self-reserved E0101 rule"
provides:
  - "37 Vector3.* method stubs in raylib.iron (MATH-03) — zero/one/add/add_value/subtract/subtract_value/scale/multiply/cross_product/perpendicular/length/length_sqr/dot_product/distance/distance_sqr/angle/negate/divide/normalize/project/reject/transform/rotate_by_quaternion/rotate_by_axis_angle/move_towards/lerp/cubic_hermite/reflect/min/max/barycenter/unproject/invert/clamp/clamp_value/equals/refract"
  - "37 Iron_vector3_* prototypes in iron_raylib.h"
  - "37 shim implementations in iron_raylib.c forwarding to raymath Vector3* via memcpy-in/memcpy-out"
  - "Cross-type arg marshalling: Vector3+Quaternion (rotate_by_quaternion), Vector3+Matrix (transform), Vector3+Matrix+Matrix (unproject) all compile and runtime-pass"
  - "5-arg cubic_hermite (4×Vector3 + float) and 4-arg barycenter validated end-to-end"
  - "int→Bool coercion confirmed for Vector3Equals (matches Vector2Equals pattern from Plan 65-01)"
  - "raymath_smoke.iron extended with 37 Vector3 call sites + 13 MATH-08 ABI asserts, canonical ironc build still prints ALL MATH-08 ASSERTS PASS exit 0"
affects: [65-03-vector4-matrix-float-helpers, 65-04-quaternion-sweep, 73-polish-performance]

# Tech tracking
tech-stack:
  added: []  # no new deps — all infrastructure live from Plan 65-01
  patterns:
    - "Vector3 (12 B) struct-by-value in/out via memcpy — same template as Vector2 (8 B), scales cleanly"
    - "Cross-type cross-kind args: one memcpy per struct-kind argument (Iron_ray_hit_mesh precedent — Vector3+Quaternion, Vector3+Matrix, Vector3+Matrix+Matrix all clean)"
    - "4-Vector3-arg and 5-arg shims (barycenter, cubic_hermite) — largest arity in Phase 65 before tuple returns; compile clean with -Wall -Wextra"
    - "Matrix-64 B pass-by-value as arg works for multiple args in one signature (Vector3.unproject takes TWO Matrix by value; still zero -Wlarge-by-value-copy warnings because threshold fires at strictly > 64)"

key-files:
  created:
    - ".planning/phases/65-raymath/65-02-SUMMARY.md"
  modified:
    - "src/stdlib/iron_raylib.c"
    - "src/stdlib/iron_raylib.h"
    - "src/stdlib/raylib.iron"
    - "tests/manual/raymath_smoke.iron"

key-decisions:
  - "Plan said 38 non-deferred Vector3 functions, but raymath.h actually has 39 Vector3 RMAPI functions (not 40). Subtract 2 deferred (to_float_v, ortho_normalize) = 37 bound here. Implementation matches plan's explicit 37-stub list verbatim; the '38/40' numbers in plan prose were an off-by-one miscount."
  - "Cross-type args (Vector3+Quaternion, Vector3+Matrix+Matrix) use per-struct-kind memcpy shapes exactly as the Iron_ray_hit_mesh (Ray+Mesh+Matrix) precedent from Phase 64-02 — zero ABI drift, zero warnings."
  - "Vector3Equals int→Bool coercion matches Vector2Equals template from Plan 65-01: `return (bool)(Vector3Equals(a, b) != 0);`. Kept explicit for clarity even though C coerces int → bool implicitly."
  - "No handling changes needed for 4-Vector3 (barycenter) or 5-arg (cubic_hermite) — clang accepted the signatures under -Wall -Wextra with zero warnings; no -Wlarge-by-value-copy at the boundary."

patterns-established:
  - "Vector3 shim pattern (37 shims): exact scale-up of Plan 65-01's Vector2 pattern — identical structure, only struct size (8 B → 12 B) differs. Validates that the memcpy template is dimension-agnostic."
  - "Cross-type multi-Matrix shim: Vector3.unproject takes TWO Matrix (64 B each, 128 B total across the two args). First Phase 65 shim exercising this. Clean under -Wall -Wextra. Pattern scales to Plan 65-03 Matrix.multiply (2 Matrix args) and similar."

requirements-completed: [MATH-03]

# Metrics
duration: 4min
completed: 2026-04-17
---

# Phase 65 Plan 02: Vector3 Summary

**37 Vector3.* raymath method bindings landed with cross-type args (Quaternion, Matrix, Matrix+Matrix), smoke test still prints "ALL MATH-08 ASSERTS PASS"**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-17T11:57:23Z
- **Completed:** 2026-04-17T12:01:26Z
- **Tasks:** 2
- **Files modified:** 4 (iron_raylib.c, iron_raylib.h, raylib.iron, raymath_smoke.iron)

## Accomplishments
- 37 Vector3 method stubs + 37 prototypes + 37 shim implementations appended to the Phase 65 section markers in raylib.iron / iron_raylib.h / iron_raylib.c. Every stub uses Iron-side receiver param `v` / `v1` / `source` / `p` per the E0101 rule; C shims use `self` as receiver per Phase 64 convention.
- `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0 with zero warnings (including no `-Wlarge-by-value-copy` on `Vector3.unproject` which passes TWO 64 B Matrix by value).
- `clang -c src/stdlib/iron_raylib_layout.c …` exits 0 (still 392 asserts — no layout changes this plan).
- raymath_smoke.iron extended with 37 Vector3 call sites (plus 1 `mat_id3` Matrix constructor and 1 `q_id` Quaternion constructor) and 13 new Vector3 ABI round-trip assertions: v3_add.x/y/z, v3_len, v3_lensq, v3_dot, v3_distsq, v3_cross.x/y/z, v3_rotq.x, v3_xform.x, v3_eq. All 28 asserts (15 from Plan 65-01 + 13 new) fold into `all_pass` and print `ALL MATH-08 ASSERTS PASS`.
- `./build/ironc build tests/manual/raymath_smoke.iron` produces a runnable Mach-O arm64 binary; `./raymath_smoke` exits 0.
- MATH-03 requirement closed for the 37 non-deferred Vector3 functions. Remaining 2 (to_float_v, ortho_normalize) carry to Plans 65-03 and 65-04 respectively (already called out in raylib.iron as `-- DEFERRED` comments).

## Task Commits

Each task was committed atomically:

1. **Task 1: 37 Vector3 stubs + prototypes + shims** — `5117fed` (feat)
2. **Task 2: Vector3 call sites + 13 ABI asserts in smoke test** — `47947b9` (feat)

**Plan metadata:** pending final commit (SUMMARY.md + STATE.md + ROADMAP.md)

## Files Created/Modified

- `src/stdlib/iron_raylib.c` — 37 new `Iron_vector3_*` shim functions appended after Plan 65-01's Vector2 block (lines 1615+). Every shim follows memcpy-in/memcpy-out forwarding to raymath; cross-type shims (rotate_by_quaternion, transform, unproject, rotate_by_axis_angle) do one memcpy per struct-kind arg.
- `src/stdlib/iron_raylib.h` — 37 new `Iron_vector3_*` prototypes appended under the Phase 65 marker after the Vector2 prototype block. Three signatures are cross-type: `Iron_vector3_transform(..., struct Iron_Matrix mat)`, `Iron_vector3_rotate_by_quaternion(..., struct Iron_Quaternion q)`, `Iron_vector3_unproject(..., struct Iron_Matrix projection, struct Iron_Matrix view)`.
- `src/stdlib/raylib.iron` — 37 new `func Vector3.*` empty-body method stubs appended after Plan 65-01's Vector2 block. Two explicit `-- DEFERRED` comments preserve the carry-forward link to 65-03 (to_float_v) and 65-04 (ortho_normalize). `Vector3.rotate_by_quaternion` is the first method in Phase 65 to take a Quaternion argument; `Vector3.unproject` is the first to take two Matrix arguments.
- `tests/manual/raymath_smoke.iron` — Header comment updated to say 73 call sites (was 36); 37 Vector3 call sites inserted between the Vector2 block and the MATH-08 section; 13 new pass_v3* asserts inserted before the `all_pass` aggregate; `all_pass` extended from 15-term AND to 28-term AND. Binary still exits 0 printing ALL MATH-08 ASSERTS PASS.

## Decisions Made

- **37 non-deferred Vector3 functions, not 38 as the plan prose stated.** raymath.h contains 39 Vector3 RMAPI functions in total (zero/one/add/add_value/subtract/subtract_value/scale/multiply/cross_product/perpendicular/length/length_sqr/dot_product/distance/distance_sqr/angle/negate/divide/normalize/project/reject/ortho_normalize/transform/rotate_by_quaternion/rotate_by_axis_angle/move_towards/lerp/cubic_hermite/reflect/min/max/barycenter/unproject/to_float_v/invert/clamp/clamp_value/equals/refract = 39). Subtract the 2 deferred (ortho_normalize → 65-04 for out-param tuple; to_float_v → 65-03 for Float3 type) = 37 functions in this plan. The plan's interfaces block says "Total: 40 functions" and the acceptance criteria says "exactly 38" — both are off by one from the actual raymath.h count. The plan's explicit 37-stub list (lines 154-193) is correct, and my implementation matches it verbatim. No scope change — just a count-reconciliation note for ROADMAP.md / Plan 65-04 verification.
- **Matrix-64B-by-value as an arg in TWO positions (Vector3.unproject)** — clean under -Wall -Wextra with zero `-Wlarge-by-value-copy` warnings. First Phase 65 shim exercising this shape. Matches the Phase 64-02 empirical finding that the warning threshold fires at strictly > 64 bytes (not ≥). Pattern is now proven for Plan 65-03 Matrix.multiply / Matrix.invert.
- **Plan 65-01 already committed `val mat_id` in the smoke test**, but to keep the Vector3 block self-contained and avoid shadowing, I added a separate `val mat_id3` identity matrix for Vector3 use. Same identity values; zero behavioural difference. Avoids any risk of one block's edit disturbing the other.

## Deviations from Plan

### Count reconciliation (documentation only — no implementation change)

**1. [Plan self-inconsistency] Plan prose says 38 non-deferred functions; actual raymath.h Vector3 RMAPI count is 39, minus 2 deferred = 37**
- **Found during:** Task 1 verification (grep count returned 37 not 38)
- **Issue:** The plan's acceptance criteria claim `grep -cE '^func Vector3\.' === 38` but the plan's own `<action>` block enumerates 37 Iron stubs. Reading raymath.h lines 621-1140 directly (the plan's stated source range) confirms raymath has exactly 39 Vector3 RMAPI functions (not 40), and 2 are deferred, so 37 is the correct count. Plan prose at lines 47, 122, 152, 313 ("Total: 40 functions" / "38 non-deferred" / "Bind all 40 raymath Vector3 functions" / "exactly 38") all appear to stem from a single upstream miscount.
- **Fix:** None — implementation matches the plan's explicit stub list (37 stubs, all mapped to genuine raymath Vector3 functions). Documentation adjusted in this SUMMARY (`requirements-completed: [MATH-03]` with 37/39 noted).
- **Files modified:** N/A (documentation only)
- **Verification:** `grep -cE '^func Vector3\.' src/stdlib/raylib.iron === 37`, `grep -cE '^struct Iron_Vector3|^float|^bool Iron_vector3_' iron_raylib.c === 37`, all 37 map to existing raymath symbols via `clang -c ... exit 0`.
- **Committed in:** N/A — scope is implementation-accurate; just flagging for ROADMAP prose correction at milestone close.
- **Impact on future plans:** None. Plan 65-04 will bind the remaining Vector3OrthoNormalize (tuple return), bringing the cumulative Vector3 count to 38 (37 + 1). Plan 65-03 will bind Vector3ToFloatV (Float3 return), bringing it to 39 — which is the full raymath Vector3 surface. Summary MATH-03 count should read "39 functions" at milestone close, not "40".

### Auto-fixed Issues

**None.** Plan executed exactly as written at the code level. The only deviation was the documentary count-reconciliation above, which required zero code change.

---

**Total deviations:** 0 code deviations (1 documentation reconciliation — plan's "38/40" prose vs actual raymath count of 39).
**Impact on plan:** Zero scope creep. All 37 bindings compile, link, and runtime-pass end-to-end. MATH-03 closed for 37 of 39 Vector3 functions; remaining 2 carry to 65-03 and 65-04 as planned.

## Issues Encountered

- **Matrix pass-by-value with TWO Matrix args in one shim (`Vector3.unproject`):** zero `-Wlarge-by-value-copy` warnings, as predicted by RESEARCH.md Pitfall 6 and Plan 65-01's confirmation. Clang's threshold fires at strictly > 64 bytes; Matrix is exactly 64 and passes clean even when used twice in a single signature.
- **4-Vector3 (`barycenter`) and 5-arg (`cubic_hermite`) shims:** compile clean. No warnings about argument count or stack usage. Local variable naming (4 vectors named `pp/aa/bb/cc` for barycenter, `v1/t1/vv2/t2` for cubic_hermite) avoids any name collision with the incoming parameters.
- **Vector3Equals int→Bool coercion:** explicit `(bool)(Vector3Equals(a, b) != 0)` matches the Plan 65-01 Vector2Equals template. Iron-side `Bool` return type works correctly (runtime assert `pass_v3eq` passes).
- **E0101 `self` reserved:** zero recurrences. All 37 Vector3 stubs use `v` / `v1` / `source` / `p` as Iron-side receiver names per the established pattern.
- **ironc build wall-clock:** single invocation per memory discipline. Comparable to Plan 65-01's 3.06s baseline (not precisely timed, but the build completed well within the normal budget and the binary ran immediately).

## Next Phase Readiness

- **Plan 65-03 (Vector4 + Matrix + Float3/Float16) fully unblocked.** Matrix-by-value-as-arg pattern is now double-validated (Vector2.transform in 65-01; Vector3.unproject twice and Vector3.transform once in 65-02). Matrix-as-RETURN is the new surface 65-03 introduces — template will memcpy-out from the raymath result just like Vector2/3 returns.
- **Plan 65-04 (Quaternion + MATH-07/08 sweep) fully unblocked.** Quaternion-as-arg pattern is now validated (Vector3.rotate_by_quaternion uses Iron_Quaternion by value). 3-tuple auto-emit probe (MatrixDecompose) remains the lone novel risk.
- **CONTEXT.md corrections for milestone close:** (a) raymath Vector3 count is 39 not 40; update MATH-03 enumeration accordingly. (b) The 4-plan function-count totals should sum to 141 (6 + 30 + 39 + 22 + 24 + 26 — but note plan's earlier total claimed 143; the delta is the Vector3 overcount). This is milestone prose housekeeping, not a Phase 65 blocker.
- **Consumer-file impact:** pong.iron / game_raylib.iron / hello_raylib.iron unchanged — none reference raymath. Confirmed Phase 64-style by absence of `Vector3\.add` / `Vector3\.cross_product` in any non-test Iron file.

## ironc Invocations

**1 invocation this plan** — Task 2 canonical smoke build (`./build/ironc build tests/manual/raymath_smoke.iron`). Matches HANDOFF.md discipline of 1 invocation per plan. Previous plans' builds (60-xx, 61-xx, 62-xx, 63-xx, 64-xx, 65-01) remain in their respective SUMMARY.md counts.

## rcore.c Sanity Check

Not re-compiled this plan — no changes to raymath include graph; `RAYMATH_IMPLEMENTATION` still owned solely by rcore.c (line 116), `RAYMATH_STATIC_INLINE` still only in iron_raylib.c (unchanged from Plan 65-01). `clang -c iron_raylib.c` and `clang -c iron_raylib_layout.c` both exit 0, which covers the TUs that changed. Full link-time validation is deferred to Plan 65-04's end-to-end sweep as scheduled.

## Self-Check: PASSED

- `src/stdlib/iron_raylib.c` — MODIFIED (37 Vector3 shims). FOUND: Iron_vector3_add/rotate_by_quaternion/unproject/cubic_hermite/barycenter all present via `grep -c 'Iron_vector3_'`.
- `src/stdlib/iron_raylib.h` — MODIFIED (37 Vector3 prototypes). FOUND.
- `src/stdlib/raylib.iron` — MODIFIED (37 Vector3 stubs + 2 DEFERRED comments). FOUND.
- `tests/manual/raymath_smoke.iron` — MODIFIED (+37 call sites, +13 asserts). FOUND.
- Task 1 commit `5117fed` — FOUND (`git log --oneline | grep 5117fed`).
- Task 2 commit `47947b9` — FOUND.
- Smoke test runtime exit 0 with `ALL MATH-08 ASSERTS PASS` — VERIFIED.
- `clang -c iron_raylib.c -Wall -Wextra` exit 0 with zero warnings — VERIFIED.
- `clang -c iron_raylib_layout.c -Wall -Wextra` exit 0 (392 asserts unchanged) — VERIFIED.

---
*Phase: 65-raymath*
*Completed: 2026-04-17*
