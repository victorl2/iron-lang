# Phase 65: raymath - Context

**Gathered:** 2026-04-17
**Status:** Ready for planning
**Source:** Smart discuss (autonomous)

<domain>
## Phase Boundary

Bind **all 143 raymath functions** as idiomatic Iron methods on their natural receivers (`Vector2`, `Vector3`, `Vector4`, `Matrix`, `Quaternion`) plus 6 freestanding scalar helpers (`Lerp`, `Clamp`, `Normalize`, `Wrap`, `Remap`, `FloatEquals`). Closes requirements **MATH-01..08**.

**Function-count breakdown (raymath 5.5, 143 total):**
- **Scalar helpers (MATH-01):** 6 — `Clamp`, `Lerp`, `Normalize` (float→float), `Wrap`, `Remap`, `FloatEquals`
- **Vector2 (MATH-02):** 30 — `Vector2Zero/One/Add/AddValue/Subtract/SubtractValue/Length/LengthSqr/DotProduct/Distance/DistanceSqr/Angle/LineAngle/Scale/Multiply/Negate/Divide/Normalize/Transform/Lerp/Reflect/Min/Max/Clamp/ClampValue/Equals/Refract/Rotate/MoveTowards/Invert`
- **Vector3 (MATH-03):** 40 — includes all above plus `CrossProduct/Perpendicular/Project/Reject/OrthoNormalize/RotateByQuaternion/RotateByAxisAngle/CubicHermite/Barycenter/Unproject/ToFloatV`
- **Vector4 (MATH-04):** 22 — subset of Vector3's surface minus cross-product and 3D-specific ops
- **Matrix (MATH-05):** 24 — `Determinant/Trace/Transpose/Invert/Identity/Add/Subtract/Multiply/Translate/Rotate{,X,Y,Z,XYZ,ZYX}/Scale/Frustum/Perspective/Ortho/LookAt/ToFloatV/Decompose`
- **Quaternion (MATH-06):** 26 — all lifecycle + `FromAxisAngle/ToAxisAngle/FromEuler/ToEuler/FromMatrix/ToMatrix/FromVector3ToVector3/CubicHermiteSpline/Slerp/Nlerp/Transform`

**In scope:** every RMAPI function in `src/vendor/raylib/raymath.h` lines 122–2170. Exceptions: (a) raymath's `QuaternionCubicHermiteSpline` name collides semantically with Phase 63's `Draw.spline_*` but lives on different receivers — bind both.

**Out of scope:**
- `MatrixToFloatV` / `Vector3ToFloatV` / `Matrix.toFloatV` / `Vector3.toFloatV` return types (`float16`, `float3` named structs in raymath.h) are PASSED THROUGH as structs — callers receive an Iron struct with named fields. No further conversion (to `[Float32]` arrays) in Phase 65; any needed convenience happens in Phase 73 polish.
- Raymath DEG2RAD / RAD2DEG macros — they're C preprocessor macros; expose as Iron `val DEG2RAD: Float32 = 0.017453292...` module-level constants if convenient, or defer to Phase 73.
- Raymath EPSILON (`#define EPSILON 0.000001f`) — same treatment; Iron side mirrors the value via a module-level constant if callers need it.
- Raymath C++ operator overloads (`RAYMATH_DISABLE_CPP_OPERATORS`). Iron has no operator overloading yet; method form only. No change.
- New types. Every type needed by Phase 65 exists from Phase 60-02 (Vector2/3/4, Matrix, Quaternion).
- Performance tuning. raymath is already SIMD-amenable on the C side; Iron shims are straight memcpy pass-through.

</domain>

<decisions>
## Implementation Decisions

### Inherited from Phase 60/61/62/63/64 (locked — no discussion)

- **Binding architecture:** shim-only. Every raymath call goes through `src/stdlib/iron_raylib.c` under the `/* ── raymath (Phase 65) ──── */` section markers that Plan 60-01 already scaffolded (at `iron_raylib.h:687` and `iron_raylib.c:1293`). Iron `func Vector2.normalize(v: Vector2) -> Vector2 {}` lowers to `Iron_vector2_normalize(Iron_Vector2 v)` in C, which forwards to raymath's `Vector2Normalize(v)` via memcpy in/out.
- **Method casing:** `snake_case`. ROADMAP prose uses camelCase (`.dotProduct`, `.lookAt`, `.fromAxisAngle`) — treat as narrative, not API spec. Implementation: `v.dot_product(w)`, `Matrix.look_at(eye, target, up)`, `Quaternion.from_axis_angle(axis, angle)`.
- **Instance-method dispatch on data-carrying objects** — validated in Phase 64 (`hir_to_lir.c:1096-1297` dispatches uniformly on `obj_type->object.decl->name`). Phase 65 reuses the pattern across 5 distinct receiver types.
- **`self` is reserved** (E0101) per Phase 64 Rule 1. Receiver params use semantic names: `v` / `v1` / `v2` for vectors, `m` / `m1` / `m2` for matrices, `q` / `q1` / `q2` for quaternions — matching raymath's own C-side conventions.
- **Static-method convention** (`Matrix.identity()`, `Vector2.zero()`, `Quaternion.identity()`, `Matrix.look_at(eye, target, up)`) uses the namespace-form dispatch already validated for `object Collision {}` in Phase 64. Static methods take no `self` and live on the type-namespace position.
- **Float32 vs Float:** raymath uses C `float` everywhere for both params and returns. All Iron-side types/params/returns are `Float32` per PROJECT.md's established ABI convention. Scalar helpers (`Lerp`, `Clamp`, etc.) take/return `Float32`.
- **Struct pass-by-value in/out:** validated across Phase 63–64. Vector2 (8B), Vector3 (12B), Vector4 (16B), Matrix (64B), Quaternion (16B, layout-identical to Vector4). Memcpy template from `Iron_draw_get_spline_point_linear` scales uniformly.
- **Layout verification:** no new types. 392 `_Static_assert` entries (Phase 60-02 through 60-07 carry-forward) already pin Vector2/3/4 + Matrix + Quaternion layout. No new asserts needed.
- **Section marker:** `/* ── raymath (Phase 65) ──── */` in both `iron_raylib.h` (line 687, ALREADY PRESENT) and `iron_raylib.c` (line 1293, ALREADY PRESENT). Phase 65 fills these in rather than scaffolding.
- **Auto-generated prototypes** (commit e3e5eee, 2026-04-14): `emit_c` auto-generates stdlib foreign-method prototypes. Planner does NOT hand-maintain prototype sync between `raylib.iron` stubs and `iron_raylib.c` wrappers.
- **Clangd false-positive workaround:** `iron_raylib.c` is not in CMake `iron_stdlib` target. Verify with `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` directly, not clangd.
- **Memory discipline:** ironc ~10 GB per invocation. Use `clang -c` for ABI round-trip verification; reserve `ironc build` for the canonical smoke test (1-2 invocations per plan, tops).
- **Consumer-file impact: none.** Neither `pong.iron`, `game_raylib.iron`, nor `hello_raylib.iron` has a `-- PHASE 65:` marker. Phase 65 introduces a dedicated smoke test file.

### Phase 65 specific

#### raymath.h inclusion strategy — **`#define RAYMATH_STATIC_INLINE`**

Before the current `#include "raylib.h"` at `iron_raylib.c:30`, add:
```c
#define RAYMATH_STATIC_INLINE
#include "raymath.h"
```

This gives `static inline` definitions — each iron_raylib.c TU gets its own inlined copies of all 143 raymath functions. The linker strips unused ones; LTO further flattens. Zero-cost for users; zero cross-TU coordination concerns.

**Alternative considered (rejected):** `#define RAYMATH_IMPLEMENTATION` in exactly one TU — saves code size marginally but complicates the build (iron_raylib.c must be the one TU, and it already compiles standalone). STATIC_INLINE wins on simplicity.

**Compiler flag discipline:** `RAYMATH_STATIC_INLINE` must be defined BEFORE the `#include`. No other TU should define `RAYMATH_IMPLEMENTATION` simultaneously (`raymath.h` has a `#error` if both are defined). Since raylib's vendored `rcore.c` and `rmodels.c` include raymath.h without either define, they get the default `inline` mode — safe, no conflict.

#### Method naming (snake_case)

| raymath C | Iron method |
|-----------|-------------|
| `Vector2Add(a, b)` | `v.add(other)` → `Vector2.add(v, other)` (static form) |
| `Vector2DotProduct(a, b)` | `v.dot_product(other)` |
| `Vector2AddValue(v, s)` | `v.add_value(s)` |
| `Vector2LineAngle(a, b)` | `v.line_angle(other)` |
| `Vector3CrossProduct(a, b)` | `v.cross_product(other)` |
| `Vector3OrthoNormalize(a, b)` | `v.ortho_normalize(other)` — mutates both via out-params; see Out-Params below |
| `Vector3RotateByQuaternion(v, q)` | `v.rotate_by_quaternion(q)` |
| `Vector3RotateByAxisAngle(v, a, ang)` | `v.rotate_by_axis_angle(axis, angle)` |
| `Vector3CubicHermite(v1, tan1, v2, tan2, t)` | `v1.cubic_hermite(tan1, v2, tan2, t)` |
| `Vector3Barycenter(p, a, b, c)` | `p.barycenter(a, b, c)` |
| `Vector3Unproject(src, proj, view)` | `v.unproject(projection, view)` |
| `Vector3ToFloatV(v)` | `v.to_float_v()` returns Iron `float3 { val x: Float32; val y: Float32; val z: Float32 }` — see "Helper-struct returns" |
| `QuaternionFromAxisAngle(axis, angle)` | `Quaternion.from_axis_angle(axis, angle)` (static) |
| `QuaternionFromEuler(pitch, yaw, roll)` | `Quaternion.from_euler(pitch, yaw, roll)` (static) |
| `QuaternionFromVector3ToVector3(a, b)` | `Quaternion.from_vector3_to_vector3(a, b)` (static) |
| `QuaternionToAxisAngle(q, out_axis, out_angle)` | `q.to_axis_angle() -> (Vector3, Float32)` — TUPLE RETURN |
| `QuaternionCubicHermiteSpline(q1, tan1, q2, tan2, t)` | `q1.cubic_hermite_spline(tan1, q2, tan2, t)` |
| `MatrixLookAt(eye, target, up)` | `Matrix.look_at(eye, target, up)` (static) |
| `MatrixPerspective(fovy, aspect, near, far)` | `Matrix.perspective(fovy, aspect, near, far)` (static) |
| `MatrixDecompose(m, out_t, out_r, out_s)` | `m.decompose() -> (Vector3, Quaternion, Vector3)` — TUPLE RETURN (3-tuple) |
| `MatrixToFloatV(m)` | `m.to_float_v()` returns Iron `float16` (16-field object) — see "Helper-struct returns" |

#### Out-param handling — **tuple returns, reusing Phase 64's `Iron_Tuple_*` auto-emission**

raymath has 4 out-param functions:
1. `Vector3OrthoNormalize(Vector3 *v1, Vector3 *v2)` — mutates both. Iron: `v1.ortho_normalize(v2: Vector3) -> (Vector3, Vector3)` (returns new v1, new v2).
2. `QuaternionToAxisAngle(Quaternion q, Vector3 *outAxis, float *outAngle)` — Iron: `q.to_axis_angle() -> (Vector3, Float32)`.
3. `MatrixDecompose(Matrix mat, Vector3 *translation, Quaternion *rotation, Vector3 *scale)` — Iron: `m.decompose() -> (Vector3, Quaternion, Vector3)` — 3-tuple.

Phase 64 verified that `(Bool, Vector2)` auto-emits as `Iron_Tuple_Bool_Vector2`. Phase 65 extends the pattern to:
- `Iron_Tuple_Vector3_Vector3`
- `Iron_Tuple_Vector3_Float32` (used for `to_axis_angle` after Iron strips `Iron_` prefix from object types in tuple composition — Float32 is a primitive, no strip)
- `Iron_Tuple_Vector3_Quaternion_Vector3` (3-tuple for decompose)

**Planner must verify 3-tuples work** — Phase 64 only exercised 2-tuples. If 3-tuples fail to auto-emit, the fallback for `MatrixDecompose` is a named return object `object MatrixDecomposition { val translation: Vector3; val rotation: Quaternion; val scale: Vector3 }`. Probe at start of Plan 65-04 Task 1.

#### Helper-struct returns — **mirror raymath's `float3` and `float16`**

raymath.h exposes two helper output structs:
- `typedef struct float3 { float v[3]; } float3;` — from `Vector3ToFloatV`
- `typedef struct float16 { float v[16]; } float16;` — from `MatrixToFloatV`

Iron mirrors these with named-field objects (since Iron doesn't yet support fixed-array-in-struct syntax):
```iron
object Float3 { val x: Float32; val y: Float32; val z: Float32 }
object Float16 {
    val m0: Float32; val m1: Float32; val m2: Float32; val m3: Float32
    val m4: Float32; val m5: Float32; val m6: Float32; val m7: Float32
    val m8: Float32; val m9: Float32; val m10: Float32; val m11: Float32
    val m12: Float32; val m13: Float32; val m14: Float32; val m15: Float32
}
```

The C shim memcpy's raymath's `float3`/`float16` into the Iron-side `Iron_Float3`/`Iron_Float16` — layout is identical (3 × 4 B contiguous floats / 16 × 4 B contiguous floats). `_Static_assert(sizeof(Iron_Float3) == sizeof(float3))` + per-field offsetof guards pin byte-identity.

These two tiny types are ADDED to `raylib.iron` in Plan 65-03 (alongside Matrix bindings since `Matrix.to_float_v` depends on Float16). They are NOT in the Phase 60 type grid — this is the first post-Phase-60 type addition, and intentional: Float3/Float16 are raymath-private helper types, not first-class raylib types.

#### Scalar freestanding helpers — **module-level `func` declarations**

The 6 scalar helpers (`Lerp, Clamp, Normalize, Wrap, Remap, FloatEquals`) are NOT methods — they're freestanding. Iron module-level `func` declarations:
```iron
func Math.lerp(start: Float32, end: Float32, amount: Float32) -> Float32 {}
func Math.clamp(value: Float32, min: Float32, max: Float32) -> Float32 {}
func Math.normalize(value: Float32, start: Float32, end: Float32) -> Float32 {}
func Math.wrap(value: Float32, min: Float32, max: Float32) -> Float32 {}
func Math.remap(value: Float32, in_start: Float32, in_end: Float32, out_start: Float32, out_end: Float32) -> Float32 {}
func Math.float_equals(x: Float32, y: Float32) -> Bool {}   // raymath returns int; Iron wraps to Bool
```

New namespace `object Math {}` added to `raylib.iron` (sits alongside `Collision {}` from Phase 64). Rationale: naming `lerp(a, b, t)` bare at module level risks collision with a future user `lerp` function; scoping under `Math.*` is safer and matches the `Collision.*` pattern. Users write `Math.lerp(0.0, 10.0, 0.5)` which is slightly less sugar-y than `lerp(0.0, 10.0, 0.5)` but wins on namespace hygiene.

**FloatEquals Bool coercion:** raymath returns `int` (1/0); the shim wraps with `(bool)(FloatEquals(x, y) != 0)` so the Iron return is a clean `Bool`, matching Phase 62's pattern for int-to-Bool conversions.

#### MATH-07 + MATH-08 validation — **incremental `tests/manual/raymath_smoke.iron`**

Each plan appends its category's call sites to a shared `tests/manual/raymath_smoke.iron`:
- Plan 65-01 creates the file, adds scalar + Vector2 call sites (~36 calls)
- Plan 65-02 appends Vector3 call sites (~40)
- Plan 65-03 appends Vector4 + Matrix + Float3/Float16 (~46)
- Plan 65-04 appends Quaternion + final canonical build (~26) and verifies ALL 143 call sites round-trip

**MATH-07 "every function exercised":** each call site passes non-trivial inputs (not all zeros/identities) and stores the result in a `val` to force the compiler to keep the call alive. Final total: 148 call sites (matches the 148-fn raw count across categories; 5 of them overlap categories in raymath.h's grep count).

**MATH-08 "ABI round-trip":** A subset (~20 call sites, picked to span every return-type shape: `float`, `Vector2`, `Vector3`, `Vector4`, `Matrix`, `Quaternion`, tuple, Float3, Float16) asserts the Iron result matches a pre-computed reference value by printing both and diffing via `Math.float_equals(result, expected)`. Canonical `./build/ironc build tests/manual/raymath_smoke.iron` produces a runnable Mach-O arm64 executable that prints "ALL MATH-08 ASSERTS PASS" on success.

#### Plan slicing — **4 plans**

- **Plan 65-01 — Scalars + Vector2 (MATH-01, MATH-02):** 36 functions. Adds `object Math {}` namespace + 6 scalar helpers + 30 Vector2 methods. Includes `#define RAYMATH_STATIC_INLINE` / `#include "raymath.h"` in iron_raylib.c — this is the FIRST plan to include the raymath header, so Task 1 is a header-inclusion probe that verifies `clang -c iron_raylib.c` still exits 0 after the header addition. Creates `tests/manual/raymath_smoke.iron` with 36 call sites. Wave 1.
- **Plan 65-02 — Vector3 (MATH-03):** 40 functions. Largest single category. Exercises `Vector3CrossProduct`, `Vector3RotateByQuaternion` (FIRST time Quaternion flows through a Vector3 method — validates cross-type arg passing), and `Vector3CubicHermite` (4 Vector3 arguments, largest-arity function in Phase 65). Wave 2 (depends_on 65-01 for the raymath.h include to already be live).
- **Plan 65-03 — Vector4 + Matrix + Float3/Float16 (MATH-04, MATH-05):** 46 functions + 2 new helper objects. Adds `object Float3 {}` and `object Float16 {}` to `raylib.iron` with `_Static_assert` layout pins in `iron_raylib_layout.c` (first type additions since Phase 60 — grid goes from 392 to ~395). Exercises first `Matrix` pass-by-value RETURN (raymath generates a 64 B Matrix result). Wave 3.
- **Plan 65-04 — Quaternion (MATH-06) + MATH-07/08 sweep:** 26 functions + final validation. First 3-tuple return (`MatrixDecompose → (Vector3, Quaternion, Vector3)`). Validates all 143 call sites compile and run end-to-end. Wave 4.

### Claude's Discretion

- **Exact shim signatures.** Byval vs const-ref (default: byval per Phase 63–64 convention). Inline vs out-of-line (moot — raymath is all inline; iron_raylib.c wrappers are one-liner `memcpy; forward; memcpy`).
- **Instance-vs-static form per function.** Most are instance methods; a few are naturally static (`Vector2.zero()`, `Matrix.identity()`, `Quaternion.identity()`, `Matrix.look_at()`, `Matrix.perspective()`, `Matrix.ortho()`, `Matrix.frustum()`, `Quaternion.from_axis_angle()`, `Quaternion.from_euler()`). Planner picks the form that reads naturally; consumer tests validate the chosen shape.
- **Parameter naming.** `v1`/`v2` vs `a`/`b` vs `other`; `axis`/`angle` vs `ax`/`ang`. Pick readable names; match raymath C param names when unambiguous.
- **Whether to define `Math.DEG2RAD` / `Math.RAD2DEG` / `Math.EPSILON` constants in this phase.** Plan 65-01 default: YES (tiny, useful, cheap). If planner disagrees, defer to Phase 73.
- **Smoke-test reference values.** Hand-compute or cross-check against calculator — planner picks representative inputs that avoid floating-point edge cases. Use `Math.float_equals` with raymath's default `EPSILON` tolerance.
- **Method placement order in `raylib.iron`.** Recommended: group by receiver (all `Vector2.*` together under the Vector2 object's section, all `Matrix.*` under Matrix, etc.), with `Math.*` scalars near the end before the `Collision.*` block.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Phase 60/61/62/63/64 foundation (authoritative design)
- `.planning/phases/60-type-enum-foundation/60-02-SUMMARY.md` — Vector2/3/4/Matrix/Quaternion layout, `_Static_assert` grid entry count (48 entries for these)
- `.planning/phases/60-type-enum-foundation/60-CONTEXT.md` — shim-only architecture, snake_case, opaque-ptr convention
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-CONTEXT.md` — namespace `object` pattern, snake_case precedent, Bool/int coercion pattern
- `.planning/phases/63-2d-drawing/63-04-SUMMARY.md` — struct-by-value RETURN pattern (memcpy-out), emit_structs.c auto-emission of Iron_List_<T> typedefs (compiler-wide improvement)
- `.planning/phases/64-collision-2d-3d/64-CONTEXT.md` — instance-method-on-data-carrying-object dispatch, `self` reserved (E0101), tuple return auto-typedef mechanism
- `.planning/phases/64-collision-2d-3d/64-01-SUMMARY.md` — canonical tuple typedef name format: `Iron_Tuple_<Type1>_<Type2>` with `Iron_` prefix stripped from object types (confirmed by runtime probe)
- `.planning/phases/64-collision-2d-3d/64-02-SUMMARY.md` — large struct pass-by-value (Mesh 120 B, Matrix 64 B) validated first-try with zero `-Wlarge-by-value-copy` warnings

### raymath upstream
- `src/vendor/raylib/raymath.h` — entire header (143 RMAPI functions; 82 KB)
  - lines 122–181: `RMAPI`/mode defines, configuration macros
  - lines 182–210: `#define EPSILON`, `DEG2RAD`, `RAD2DEG`
  - lines 211–223: `float3` and `float16` helper struct typedefs (from `Vector3ToFloatV` and `MatrixToFloatV`)
  - lines 224–290: 6 scalar utility functions (MATH-01)
  - lines 291–620: 30 Vector2 functions (MATH-02)
  - lines 621–1140: 40 Vector3 functions (MATH-03)
  - lines 1141–1290: 22 Vector4 functions (MATH-04)
  - lines 1291–1730: 24 Matrix functions (MATH-05)
  - lines 1731–2170: 26 Quaternion functions (MATH-06)
- `src/vendor/raylib/raylib.h` — Vector2/3/4, Matrix, Quaternion typedef struct definitions (authoritative layout that raymath consumes)

### Iron stdlib precedents
- `src/stdlib/iron_raylib.h` line 687 — scaffolded `/* ── raymath (Phase 65) ───── */` section marker (Plan 60-01); Phase 65 fills this in
- `src/stdlib/iron_raylib.c` line 1293 — scaffolded section marker; Phase 65 fills this in (and adds `#define RAYMATH_STATIC_INLINE` + `#include "raymath.h"` earlier in the file, ~line 30)
- `src/stdlib/iron_raylib.c` `Iron_draw_get_spline_point_linear` — Vector2 memcpy-out pattern, reuse for every `Vector2Zero/One/Add/...` returning Vector2
- `src/stdlib/iron_raylib.c` `Iron_rectangle_intersection` — Rectangle-size (16 B) struct-by-value return, pattern scales to Vector4 (16 B) and Matrix (64 B)
- `src/stdlib/iron_raylib.c` `Iron_collision_lines` — tuple-return `(Bool, Vector2)` pattern via `Iron_Tuple_Bool_Vector2` auto-emitted typedef; Phase 65 adds `Iron_Tuple_Vector3_Vector3`, `Iron_Tuple_Vector3_Float32`, and `Iron_Tuple_Vector3_Quaternion_Vector3` (3-tuple) via the same auto-emit mechanism
- `src/stdlib/iron_raylib.c` `Iron_ray_hit_mesh` — 120 B Mesh pass-by-value as function ARG; Matrix-64 B pass-by-value validated here too. Phase 65 exercises Matrix-as-arg in ~half the Matrix functions (Matrix.multiply, Matrix.invert, etc.)
- `src/stdlib/iron_raylib_layout.c` — 392 existing `_Static_assert` entries through Phase 60-07. Phase 65 adds ~3 entries for Float3 and Float16 in Plan 65-03 (first type-grid growth since Phase 60).

### Project-level specs
- `.planning/REQUIREMENTS.md` lines 208–216 — MATH-01..08 detailed descriptions (enumerates all 143 function names)
- `.planning/ROADMAP.md` — Phase 65 section, 5 success-criteria truths, Phase 60 dependency
- `.planning/PROJECT.md` — Float32 ABI convention, "bind raymath fully (143 helpers)" decision entry in Key Decisions table

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`object Vector2`, `object Vector3`, `object Vector4`, `object Matrix`, `object Quaternion`** — all defined in `src/stdlib/raylib.iron` by Phase 60-02 with `_Static_assert` layout pinning (lines 48 offsetof entries). Phase 65 extends them with methods; zero layout changes.
- **raymath section markers** at `iron_raylib.h:687` and `iron_raylib.c:1293` — pre-scaffolded empty markers; Phase 65 fills them in.
- **Vector2/3 struct-by-value RETURN memcpy-out pattern** — 5 copies in `Iron_draw_get_spline_point_*` (Phase 63-04) + 1 Rectangle-return + 5 RayCollision-return (Phase 64-02). Phase 65 follows the identical template for every raymath function returning Vector2/3/4/Matrix/Quaternion (~100 of the 143).
- **`Iron_Tuple_<Type1>_<Type2>` auto-emission** — confirmed working for `Iron_Tuple_Bool_Vector2` in Phase 64-01. Phase 65 extends to 3 new 2-tuple shapes and 1 new 3-tuple shape (planner verifies 3-tuples at Plan 65-04 start).
- **Auto-generated prototypes** — `emit_c` commit e3e5eee means zero hand-maintenance of prototype sync. Saved ~143 lines of duplicate bookkeeping in Phase 65.
- **`_Static_assert` layout grid** — 392 entries from Phase 60-07. Pins Vector2/3/4/Matrix/Quaternion at exact raylib layout. No changes needed for MATH-01..06; Plan 65-03 adds ~3 entries for Float3/Float16 (first additions in 4 phases).
- **Memory discipline** — ironc ~10 GB. Canonical validation: `clang -c src/stdlib/iron_raylib.c` + `clang -c iron_raylib_layout.c`. `ironc build raymath_smoke.iron` budget: 1 invocation per plan (4 total across Phase 65).

### Established Patterns
- **Empty-body foreign-method stubs** with auto-emitted C prototypes — planner declares `func Vector2.normalize(v: Vector2) -> Vector2 {}` in raylib.iron; emit_c creates the `Iron_vector2_normalize(Iron_Vector2)` prototype + shim hookup.
- **Namespace-type declaration** — `object Math {}` / `object Collision {}` pattern validated in Phase 64. Phase 65 adds `object Math {}` for 6 scalar freestanding functions.
- **Instance method on data-carrying object** — validated in Phase 64 via `Rectangle.collides(other: Rectangle)`. Phase 65 extends to 5 distinct receiver types simultaneously — stresses the dispatch but the mechanism is already proven.
- **Tuple-return lowering** — `Iron_Tuple_A_B { v0: A; v1: B }` auto-emitted when function returns `(A, B)`. Phase 65 extends to 3-tuples (`MatrixDecompose`).

### Integration Points
- `src/stdlib/iron_raylib.c` raymath section (currently empty at line 1293) — fill with 143 shim wrappers
- `src/stdlib/iron_raylib.h` raymath section (currently empty at line 687) — fill with 143 C prototypes (auto-emitted for `emit_c` but still helpful to have hand-visible for review)
- `src/stdlib/raylib.iron` — extend existing `object Vector2/3/4`, `object Matrix`, `object Quaternion` with methods; add `object Math {}`; in Plan 65-03 add `object Float3 {}` and `object Float16 {}` (with `_Static_assert` mirror in iron_raylib_layout.c)
- `src/stdlib/iron_raylib_layout.c` — add 3 `_Static_assert` entries for Float3 and Float16 (Plan 65-03)
- `tests/manual/raymath_smoke.iron` — new file (Plan 65-01 creates, 65-02/03/04 append)

</code_context>

<specifics>
## Specific Ideas

- **RAYMATH_STATIC_INLINE is the first preprocessor define iron_raylib.c sets before including raylib.h.** Current iron_raylib.c has `#include "raylib.h"` at line 30 with no preceding defines. Plan 65-01 Task 1 inserts:
  ```c
  #define RAYMATH_STATIC_INLINE
  #include "raymath.h"
  ```
  IMMEDIATELY before the `#include "raylib.h"`. Order matters only if raylib.h itself includes raymath.h (it doesn't — the vendored rcore.c is where raylib.h + raymath.h meet).
- **Quaternion is NOT a typealias for Vector4 in Iron.** Phase 60-02 made them distinct `object` declarations with identical memory layout (pinned by `_Static_assert(sizeof(Iron_Quaternion) == sizeof(Vector4))`). The shim for `QuaternionNormalize(Quaternion q) -> Quaternion` memcpy's Iron_Quaternion (16 B) into raymath's `Quaternion` typedef (which IS `Vector4`), calls the raymath function, memcpy's result back. No ambiguity at the Iron level — user writes `Quaternion.normalize(q)` not `Vector4.normalize(q)`.
- **Matrix memory-order inversion hazard.** raymath's header comment (lines 11–15) notes: "Matrix structure is defined as row-major (memory layout) but parameters naming AND all math operations performed by the library consider the structure as it was column-major." Iron's Matrix layout matches raymath's C layout exactly — `_Static_assert(offsetof(Iron_Matrix, m0) == offsetof(Matrix, m0))` holds because both are row-major-in-memory. Users treat Iron Matrix the same way C users treat raymath Matrix — no semantic drift.
- **`QuaternionTransform(q, mat)` takes Matrix.** Single cross-type param in Quaternion surface. Shim does two memcpys (one Iron_Quaternion → raymath Quaternion; one Iron_Matrix → raymath Matrix), calls `QuaternionTransform(q, mat)`, memcpys result Vector4 back into Iron_Quaternion. Pattern: cross-type args handled by separate memcpys per arg.
- **`Vector3OrthoNormalize(Vector3 *v1, Vector3 *v2)` mutates both args.** Iron: `func Vector3.ortho_normalize(v1: Vector3, v2: Vector3) -> (Vector3, Vector3)`. Shim declares local `Vector3 c1, c2; memcpy(&c1, &v1, 12); memcpy(&c2, &v2, 12); Vector3OrthoNormalize(&c1, &c2);` then returns the tuple with both post-mutation values. First time out-param args BY POINTER from raymath are Iron-side transformed into return-tuple values.
- **`MatrixDecompose` is the 3-tuple case.** Phase 64 verified 2-tuples. Plan 65-04 Task 1 probe: declare `Matrix.decompose(m: Matrix) -> (Vector3, Quaternion, Vector3)` and inspect `emit_c` output. Expected typedef name: `Iron_Tuple_Vector3_Quaternion_Vector3`. If auto-emit fails for 3-tuples, fallback: `object MatrixDecomposition { val translation: Vector3; val rotation: Quaternion; val scale: Vector3 }` returned by value. Probe result is captured in Plan 65-04 SUMMARY.md as a compiler finding for future phases.
- **Vector2Rotate vs Vector3RotateByAxisAngle asymmetry.** raymath's Vector2Rotate takes `(Vector2 v, float angle)` (2D rotation needs only an angle), Vector3RotateByAxisAngle takes `(Vector3 v, Vector3 axis, float angle)` (3D rotation needs axis). Iron methods: `v.rotate(angle)` (Vector2), `v.rotate_by_axis_angle(axis, angle)` (Vector3). Same verb `rotate` overloaded across receivers; disambiguated by receiver type at dispatch.
- **Float3 and Float16 sizing.** `sizeof(Iron_Float3) == 12` (3 × Float32), `sizeof(Iron_Float16) == 64` (16 × Float32). Matches raymath's C-side exactly.
- **FloatEquals returns int (0 or 1) in raymath.** Shim: `return (bool)(FloatEquals(x, y) != 0);` — Iron `Bool` is 1 byte (Phase 60-05 proved the ABI), so the return-value marshalling is `(uint8_t)value`. Keeping the Iron API as `Bool` rather than `Int32` is user-friendly.
- **DEG2RAD / RAD2DEG / EPSILON module constants.** Add three `val` declarations at raylib.iron module level (Plan 65-01 Task 2):
  ```iron
  val DEG2RAD: Float32 = Float32(0.01745329251994329577)
  val RAD2DEG: Float32 = Float32(57.29577951308232087680)
  val EPSILON: Float32 = Float32(0.000001)
  ```
  Users write `Math.clamp(x, -EPSILON, EPSILON)` or `angle * DEG2RAD`. No shim needed — these are pure Iron-side constants.

</specifics>

<deferred>
## Deferred Ideas

- **SIMD-accelerated raymath.** raymath's implementation is plain C; SSE/AVX/NEON acceleration is not in scope for Phase 65. Could land as a Phase 73 optimization if benchmarks show demand.
- **raymath operator overloading** (`v1 + v2` sugar). Iron has no operator overloading yet. Users call `v1.add(v2)` instead. Phase 73 API polish could experiment if Iron gains the feature.
- **`MatrixFromArray(float *values, int count)` or similar constructor from Float16.** Not in raymath; users who have a `Float16` construct the Matrix manually. Out of scope.
- **Quaternion SLERP/NLERP tolerance tuning.** raymath ships with fixed tolerances; users who need custom tolerances wrap the Iron methods themselves. Out of scope.
- **Matrix decomposition caching.** Each `m.decompose()` call re-runs the full decomposition. A user-level cache is the right layer; stdlib shouldn't assume caching. Out of scope.
- **3D Ray/Line math** (e.g., `RayFromPoints`, `LineClosestPoint`). Not in raymath; users compose from existing primitives. Phase 73 could add idiomatic convenience wrappers if demand materializes.
- **Color math on `Color` type.** Color operations (tint, fade, blend, HSV conversion) are Phase 66 territory (TEX-12). raymath does NOT include color functions; those live in `rtextures.c`.
- **Transform propagation for scene graphs.** Applying a parent Matrix to a child Transform is user-level; stdlib doesn't impose a scene-graph model. Out of scope.
- **`float3.add(other)` / `float16.multiply(other)` methods.** These helper types are raymath-private ABI bridges; users who want math on them should convert to Vector3/Matrix first. No methods attached to Float3/Float16.

</deferred>

---

*Phase: 65-raymath*
*Context gathered: 2026-04-17 via smart discuss (autonomous)*
