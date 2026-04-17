# Phase 65: raymath — Research

**Researched:** 2026-04-16
**Domain:** raylib `raymath.h` FFI binding (143 math helper functions as idiomatic Iron methods on Vector2/3/4, Matrix, Quaternion + 6 freestanding scalars)
**Confidence:** HIGH

## Summary

Phase 65 binds every raymath.h helper as an Iron method on its natural receiver. CONTEXT.md locked the architecture completely (RAYMATH_STATIC_INLINE, snake_case, tuple returns, Float3/Float16 helpers, 4-plan slicing, incremental smoke test). My research job was verifying each locked mechanism against the actual compiler source and the actual raymath.h, NOT re-exploring alternatives.

All five "unverified mechanisms" CONTEXT.md flagged resolve GREEN:

1. **Arbitrary-arity tuple auto-emission works** — `emit_helpers.c:260-294` loops over `tuple_ty->tuple.elem_count` with no arity hardcode. `Iron_Tuple_Vector3_Quaternion_Vector3` will auto-emit identically to Phase 64's `Iron_Tuple_Bool_Vector2`.
2. **RAYMATH_STATIC_INLINE inclusion is safe** — `raylib.h` does NOT include `raymath.h` (grep confirmed: only comment mentions at line 18/162). rcore.c holds `RAYMATH_IMPLEMENTATION` as the canonical out-of-line owner; iron_raylib.c using `RAYMATH_STATIC_INLINE` gets self-contained `static inline` copies with zero link-time conflict.
3. **Float3/Float16 layout equivalence holds trivially** — C guarantees adjacent same-size float fields are contiguous; `struct { float x, y, z; }` and `struct { float v[3]; }` are byte-identical in size and per-element offset. `_Static_assert(sizeof(Iron_Float3) == sizeof(float3))` is sufficient.
4. **Method overloading across 5 receivers is NOT overloading** — `hir_to_lir.c:1097,1297` mangles each as `Iron_<lowercased_typename>_<method>`. `Vector2.add`, `Vector3.add`, `Vector4.add`, `Quaternion.add` produce four distinct C symbols (`Iron_vector2_add`, `Iron_vector3_add`, `Iron_vector4_add`, `Iron_quaternion_add`). Each is a separate function — no dispatch ambiguity possible.
5. **Cross-type args (e.g. Vector3+Quaternion)** work cleanly via independent memcpys per arg. Phase 64 `Iron_ray_hit_mesh` is the canonical precedent (3 memcpys: Ray + Mesh + Matrix).

**Primary recommendation:** Execute Plan 65-01..04 as sequenced in CONTEXT.md. Plan 65-01 Task 1 is a header-inclusion smoke test (clang -c iron_raylib.c after adding `#define RAYMATH_STATIC_INLINE` + `#include "raymath.h"` — expected clean). Plan 65-04 Task 1 is a 3-tuple auto-emit probe (declare `Matrix.decompose -> (Vector3, Quaternion, Vector3)`, inspect generated C for `Iron_Tuple_Vector3_Quaternion_Vector3`). Both probes are low-risk — compiler behavior is verified at the source level; probes exist to document empirical output for future phases.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Inherited from Phase 60/61/62/63/64:**
- Binding architecture: shim-only. Every raymath call goes through `src/stdlib/iron_raylib.c` under the `/* ── raymath (Phase 65) ──── */` section markers at `iron_raylib.h:687` and `iron_raylib.c:1293`.
- Method casing: `snake_case`. `v.dot_product(w)`, `Matrix.look_at(eye, target, up)`, `Quaternion.from_axis_angle(axis, angle)`.
- Instance-method dispatch on data-carrying objects validated in Phase 64 (`hir_to_lir.c:1096-1297`).
- `self` is reserved (E0101). Receiver params: `v`/`v1`/`v2` for vectors, `m`/`m1`/`m2` for matrices, `q`/`q1`/`q2` for quaternions.
- Static-method convention validated (`Matrix.identity()`, `Vector2.zero()`, etc.).
- Float32 everywhere (raymath uses C `float`).
- Struct pass-by-value in/out validated across Phases 63–64.
- No new types: Vector2/3/4, Matrix, Quaternion already pinned.
- Section markers ALREADY PRESENT at `iron_raylib.h:687` and `iron_raylib.c:1293`.
- Auto-generated prototypes (commit e3e5eee). Planner does NOT hand-maintain prototype sync.
- Clangd false-positive workaround: verify with `clang -c` directly.
- Memory discipline: ironc ~10 GB per invocation. Budget: 1 ironc invocation per plan (4 total for Phase 65).
- No impact on pong.iron / game_raylib.iron / hello_raylib.iron.

**Phase 65 specific:**
- **raymath.h inclusion strategy: `#define RAYMATH_STATIC_INLINE`** before `#include "raymath.h"` before the existing `#include "raylib.h"` at iron_raylib.c:30.
- **Method naming (snake_case):** e.g. `Vector2Add` → `v.add(other)`, `QuaternionFromAxisAngle` → `Quaternion.from_axis_angle(axis, angle)` static.
- **Out-param handling: tuple returns.** 4 out-param functions:
  - `Vector3OrthoNormalize(Vector3 *v1, Vector3 *v2)` → `v1.ortho_normalize(v2) -> (Vector3, Vector3)`
  - `QuaternionToAxisAngle(Quaternion q, Vector3 *outAxis, float *outAngle)` → `q.to_axis_angle() -> (Vector3, Float32)`
  - `MatrixDecompose(Matrix, Vector3*, Quaternion*, Vector3*)` → `m.decompose() -> (Vector3, Quaternion, Vector3)` (3-TUPLE)
  - Expected tuple typedefs: `Iron_Tuple_Vector3_Vector3`, `Iron_Tuple_Vector3_Float32`, `Iron_Tuple_Vector3_Quaternion_Vector3`.
- **Fallback for 3-tuple**: if auto-emit fails for `MatrixDecompose`, use named object `object MatrixDecomposition { val translation: Vector3; val rotation: Quaternion; val scale: Vector3 }`. Probe at Plan 65-04 Task 1.
- **Helper-struct returns: mirror raymath's `float3` / `float16`** with Iron named-field objects:
  ```iron
  object Float3 { val x: Float32; val y: Float32; val z: Float32 }
  object Float16 { val m0..m15: Float32 }   // 16 fields
  ```
  Added in Plan 65-03 alongside Matrix bindings. First post-Phase-60 type grid addition.
- **Scalar freestanding helpers: module-level `func Math.*`:**
  ```iron
  func Math.lerp(start: Float32, end: Float32, amount: Float32) -> Float32 {}
  func Math.clamp(value: Float32, min: Float32, max: Float32) -> Float32 {}
  func Math.normalize(value: Float32, start: Float32, end: Float32) -> Float32 {}
  func Math.wrap(value: Float32, min: Float32, max: Float32) -> Float32 {}
  func Math.remap(value: Float32, in_start: Float32, in_end: Float32, out_start: Float32, out_end: Float32) -> Float32 {}
  func Math.float_equals(x: Float32, y: Float32) -> Bool {}
  ```
  New `object Math {}` namespace. `FloatEquals` int→Bool coerced in shim.
- **MATH-07/08 validation: incremental `tests/manual/raymath_smoke.iron`.** Plan 65-01 creates, 65-02/03/04 append. Final: 143 call sites round-trip; ~20 ABI-probed sites via `Math.float_equals(result, expected)` print `"ALL MATH-08 ASSERTS PASS"`.
- **4-plan slicing:**
  - Plan 65-01 — Scalars + Vector2 (MATH-01, MATH-02): 36 fns. Task 1 = header-include probe.
  - Plan 65-02 — Vector3 (MATH-03): 40 fns. First cross-type (Quaternion in Vector3 method).
  - Plan 65-03 — Vector4 + Matrix + Float3/Float16 (MATH-04, MATH-05): 46 fns + 2 new objects + ~3 new _Static_asserts.
  - Plan 65-04 — Quaternion (MATH-06) + MATH-07/08 sweep: 26 fns. Task 1 = 3-tuple probe.

### Claude's Discretion
- Exact shim signatures (byval vs const-ref — default byval per Phase 63–64).
- Instance-vs-static form per function (planner picks).
- Parameter naming (`v1`/`v2` vs `a`/`b` vs `other`).
- Whether to define `Math.DEG2RAD` / `Math.RAD2DEG` / `Math.EPSILON` constants in Plan 65-01 (default: YES).
- Smoke-test reference values (hand-compute or calculator cross-check).
- Method placement order in `raylib.iron`.

### Deferred Ideas (OUT OF SCOPE)
- SIMD-accelerated raymath (Phase 73 candidate).
- Operator overloading (`v1 + v2`) — Iron has no operator overloading.
- `MatrixFromArray` constructor from Float16.
- Quaternion SLERP tolerance tuning.
- Matrix decomposition caching.
- 3D Ray/Line math helpers (raymath doesn't provide).
- Color math (Phase 66 territory).
- Transform propagation for scene graphs.
- `Float3.add` / `Float16.multiply` methods (no methods attached to helper types).
- `Vector3ToFloatV` / `MatrixToFloatV` returning `[Float32]` Iron array — pass-through struct only. Convenience conversion deferred to Phase 73.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| MATH-01 | Scalar utilities (Lerp, Clamp, Normalize, Wrap, FloatEquals, Remap) exist as freestanding functions | 6 `func Math.*` stubs in raylib.iron (Plan 65-01); shims forward to raymath.h lines 178–228; `int`→`Bool` coercion for `FloatEquals` follows Phase 62 pattern |
| MATH-02 | `Vector2` has methods for all raymath `Vector2*` operations (30 functions) | Instance methods on data-carrying object validated by Phase 64 Plan 01; lines 231–620 of raymath.h enumerate all 30; memcpy-in/memcpy-out template from `Iron_draw_get_spline_point_linear` |
| MATH-03 | `Vector3` has methods for all raymath `Vector3*` operations (40 functions) | Same dispatch + memcpy pattern as Vector2; includes `Vector3RotateByQuaternion` (cross-type) — verified pattern: Phase 64 `Iron_ray_hit_mesh` does 3 independent memcpys for 3 different struct types |
| MATH-04 | `Vector4` has methods for all raymath `Vector4*` operations (22 functions) | 16-byte struct pass-by-value validated by Phase 63 (Rectangle 16B in/out); Vector4 layout pinned by Phase 60-02 `_Static_assert` (offsetof x/y/z/w + sizeof) |
| MATH-05 | `Matrix` has methods for all raymath `Matrix*` operations (24 functions) | 64-byte Matrix pass-by-value as ARG proven in Phase 64 Plan 02 (`Iron_ray_hit_mesh` — zero `-Wlarge-by-value-copy` warnings); 64-byte Matrix as RETURN is new in Phase 65, same template scaled up |
| MATH-06 | `Quaternion` has methods for all raymath `Quaternion*` operations (26 functions) | Quaternion layout-identical to Vector4 (both 16B); Phase 60-02 cross-asserts `sizeof(Iron_Quaternion) == sizeof(Vector4)`. `MatrixDecompose` is the 3-tuple case — auto-emit verified generic in `emit_helpers.c:260-294` |
| MATH-07 | All 143 raymath functions individually testable via Iron code | `tests/manual/raymath_smoke.iron` incremental build (Plan 65-01 creates; 65-02/03/04 append). 148 call sites across the 4 plans |
| MATH-08 | Math operations preserve C ABI (Float32 round-trip without precision loss) | Subset of smoke test (~20 sites spanning every return shape) uses `Math.float_equals(result, expected)` with EPSILON tolerance; final run prints `"ALL MATH-08 ASSERTS PASS"` |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| raylib (vendored) | 5.5 | FFI target — `raymath.h` is part of the raylib project | Already vendored at `src/vendor/raylib/`; Phase 60-07 pinned layouts |
| `raymath.h` | 2.0 (bundled with raylib 5.5) | 143 RMAPI math helpers; header-only | Authoritative source; 2941 lines; no external deps besides `<math.h>` |
| Iron `emit_c.c` generic tuple auto-emit | commit e3e5eee (2026-04-14) | Synthesises `Iron_Tuple_<mangled>` typedefs on demand | Arbitrary arity via `emit_helpers.c:260-294` loop over `elem_count` |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| `<string.h>` (libc) | C11 | `memcpy` for ABI struct marshalling | Every shim does `memcpy(&c_local, &iron_param, sizeof(C_type))` — template in Phase 60–64 |
| `<math.h>` (libc) | C11 | raymath's only external dependency (sqrtf, sinf, cosf, atan2f, etc.) | Pulled in automatically by raymath.h:171 |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `#define RAYMATH_STATIC_INLINE` | `#define RAYMATH_IMPLEMENTATION` in iron_raylib.c | **REJECTED.** rcore.c already uses `RAYMATH_IMPLEMENTATION` (line 116). Defining it in a second TU triggers `raymath.h:58 #error` at compile time. `RAYMATH_STATIC_INLINE` is the only safe option. |
| `#define RAYMATH_STATIC_INLINE` | No define (default: `RMAPI inline`) | **REJECTED.** Default mode produces `extern inline` declarations that rely on another TU providing the out-of-line definition (via `RAYMATH_IMPLEMENTATION`). While rcore.c does provide those via linking, the dependency graph is implicit and fragile. `RAYMATH_STATIC_INLINE` produces self-contained `static inline` copies per TU — linker strips duplicates, LTO flattens. Zero cross-TU coordination concerns. |
| Method form (`v.dot_product(w)`) | C++ operator overloads | Iron has no operator overloading. Method form is mandatory. |
| Iron `Float3 { val x, y, z }` | Iron `Float3 { val v: [Float32; 3] }` | Iron does not yet support fixed-array fields in objects. Named fields are the only portable option. Byte-equivalence holds regardless (C contiguous-float layout). |
| Tuple return `(Vector3, Quaternion, Vector3)` | Named object `MatrixDecomposition` | Tuple is idiomatic Iron; named object is fallback only if auto-emit fails. Research verifies auto-emit supports arbitrary arity — fallback likely unneeded. |

**Installation:** N/A — raylib vendored; raymath.h is part of it.

**Version verification:** Not applicable — using vendored raylib 5.5 at `src/vendor/raylib/` (same version used by Phases 60–64). No registry lookup needed.

## Architecture Patterns

### Recommended Project Structure
```
src/stdlib/
├── raylib.iron               # Iron-side: func stubs grouped by receiver (Vector2/3/4, Matrix, Quaternion, Math)
├── iron_raylib.h             # C prototypes appended under /* ── raymath (Phase 65) ── */ at line 687
├── iron_raylib.c             # Shim implementations; raymath.h include + RAYMATH_STATIC_INLINE at top
└── iron_raylib_layout.c      # _Static_assert grid: 390 now → ~393 after Float3+Float16 (+2 size + ~19 offsets)
tests/manual/
└── raymath_smoke.iron        # Incrementally built across 4 plans (NEW in Plan 65-01)
```

### Pattern 1: Header-Include Strategy (Plan 65-01 Task 1)
**What:** Before `#include "raylib.h"` in iron_raylib.c, add RAYMATH_STATIC_INLINE define + raymath.h include.
**When to use:** ONCE, at the start of Phase 65. Subsequent plans do NOT re-add.
**Example:**
```c
// Source: CONTEXT.md "raymath.h inclusion strategy" + raymath.h:57-73 (verified)
// iron_raylib.c lines ~27-31 after edit:
#include <string.h>
#include "iron_raylib.h"
#define RAYMATH_STATIC_INLINE    /* NEW Plan 65-01 */
#include "raymath.h"             /* NEW Plan 65-01 */
#include "raylib.h"
```

**Verification:**
- `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0 with zero warnings.
- No link-time duplicate symbols when iron_raylib.c is linked alongside rcore.c (which owns `RAYMATH_IMPLEMENTATION`) because static-inline symbols are file-local.

### Pattern 2: Shim Template (by category)

#### 2a. Simple pass-through (returns Vector2/3/4/Matrix/Quaternion)
Source: Phase 63 `Iron_draw_get_spline_point_linear` template.
```c
// Source: pattern from Phase 63-04 + raymath.h:252 Vector2Add
struct Iron_Vector2 Iron_vector2_add(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Add(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}
```

#### 2b. Cross-type args (Vector3 + Quaternion, Quaternion + Matrix)
Source: Phase 64 `Iron_ray_hit_mesh` (3 independent memcpys).
```c
// Source: raymath.h:865 Vector3RotateByQuaternion + Phase 64-02 pattern
struct Iron_Vector3 Iron_vector3_rotate_by_quaternion(struct Iron_Vector3 self, struct Iron_Quaternion q) {
    Vector3 v;
    Quaternion quat;
    memcpy(&v,    &self, sizeof(Vector3));
    memcpy(&quat, &q,    sizeof(Quaternion));
    Vector3 r = Vector3RotateByQuaternion(v, quat);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}
```

#### 2c. Out-param → tuple return (2-tuple)
Source: Phase 64 `Iron_collision_lines` + raymath.h:2416 `QuaternionToAxisAngle`.
```c
// Source: raymath.h:2416 QuaternionToAxisAngle + Phase 64-01 Iron_Tuple_Bool_Vector2 pattern
Iron_Tuple_Vector3_Float32 Iron_quaternion_to_axis_angle(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Vector3 axis;
    float angle;
    QuaternionToAxisAngle(q, &axis, &angle);
    Iron_Tuple_Vector3_Float32 out;
    memcpy(&out.v0, &axis, sizeof(Vector3));
    out.v1 = angle;
    return out;
}
```

#### 2d. Out-param → tuple return (3-tuple) — PROBE
Source: raymath.h:2531 `MatrixDecompose` + generic tuple auto-emit at `emit_helpers.c:260-294`.
```c
// Source: raymath.h:2531 MatrixDecompose
// EXPECTED auto-emitted typedef: typedef struct { Iron_Vector3 v0; Iron_Quaternion v1; Iron_Vector3 v2; } Iron_Tuple_Vector3_Quaternion_Vector3;
Iron_Tuple_Vector3_Quaternion_Vector3 Iron_matrix_decompose(struct Iron_Matrix self) {
    Matrix m;
    memcpy(&m, &self, sizeof(Matrix));
    Vector3 translation, scale;
    Quaternion rotation;
    MatrixDecompose(m, &translation, &rotation, &scale);
    Iron_Tuple_Vector3_Quaternion_Vector3 out;
    memcpy(&out.v0, &translation, sizeof(Vector3));
    memcpy(&out.v1, &rotation,    sizeof(Quaternion));
    memcpy(&out.v2, &scale,       sizeof(Vector3));
    return out;
}
```

**Probe instructions (Plan 65-04 Task 1):** Before writing this shim, add the Iron stub `func Matrix.decompose(m: Matrix) -> (Vector3, Quaternion, Vector3) {}` to raylib.iron, invoke `ironc build --debug-build` on a tiny caller, inspect `.iron-build/probe_matrix_decompose.c` for the auto-emitted typedef name. **Expected:** `Iron_Tuple_Vector3_Quaternion_Vector3`. **Fallback:** If auto-emit fails (no evidence suggests it will — the emit_helpers.c loop is generic over `elem_count`), swap to `object MatrixDecomposition { val translation: Vector3; val rotation: Quaternion; val scale: Vector3 }`. Record probe result in Plan 65-04 SUMMARY.md.

#### 2e. Out-param mutating both args → 2-tuple return
Source: raymath.h:816 `Vector3OrthoNormalize`.
```c
// Source: raymath.h:816 Vector3OrthoNormalize
Iron_Tuple_Vector3_Vector3 Iron_vector3_ortho_normalize(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 c1, c2;
    memcpy(&c1, &self,  sizeof(Vector3));
    memcpy(&c2, &other, sizeof(Vector3));
    Vector3OrthoNormalize(&c1, &c2);   /* mutates both */
    Iron_Tuple_Vector3_Vector3 out;
    memcpy(&out.v0, &c1, sizeof(Vector3));
    memcpy(&out.v1, &c2, sizeof(Vector3));
    return out;
}
```

#### 2f. Scalar free helper + int→Bool coercion
```c
// Source: raymath.h:220 FloatEquals + Phase 62 Bool coercion precedent
bool Iron_math_float_equals(float x, float y) {
    return (bool)(FloatEquals(x, y) != 0);
}
```

#### 2g. Helper-struct return (Float3, Float16)
```c
// Source: raymath.h:2121 Vector3ToFloatV (example) + contiguous-float layout guarantee
struct Iron_Float3 Iron_vector3_to_float_v(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    float3 r = Vector3ToFloatV(v);
    struct Iron_Float3 out;
    memcpy(&out, &r, sizeof(float3));   /* 12 B copy — named-fields and v[3] are byte-identical */
    return out;
}
```

### Pattern 3: Iron Stub Form

```iron
-- Instance method on data-carrying object (Rectangle/Vector2 precedent from Phase 64)
func Vector2.add(v: Vector2, other: Vector2) -> Vector2 {}
func Vector3.rotate_by_quaternion(v: Vector3, q: Quaternion) -> Vector3 {}

-- Static method (no receiver instance — namespace-form dispatch validated for Collision/Math)
func Vector2.zero() -> Vector2 {}
func Matrix.identity() -> Matrix {}
func Matrix.look_at(eye: Vector3, target: Vector3, up: Vector3) -> Matrix {}
func Quaternion.from_axis_angle(axis: Vector3, angle: Float32) -> Quaternion {}

-- Out-param → tuple
func Matrix.decompose(m: Matrix) -> (Vector3, Quaternion, Vector3) {}
func Quaternion.to_axis_angle(q: Quaternion) -> (Vector3, Float32) {}
func Vector3.ortho_normalize(v: Vector3, other: Vector3) -> (Vector3, Vector3) {}

-- Free function in Math namespace
func Math.lerp(start: Float32, end: Float32, amount: Float32) -> Float32 {}
```

### Pattern 4: `object Math {}` Namespace + Module Constants

```iron
-- Add once in Plan 65-01, alongside existing object Collision {} at line 962
object Math {}

-- Module-level constants (Plan 65-01 Task 2, Claude discretion GO)
val DEG2RAD: Float32 = Float32(0.01745329251994329577)
val RAD2DEG: Float32 = Float32(57.29577951308232087680)
val EPSILON: Float32 = Float32(0.000001)
```

### Pattern 5: Float3/Float16 Type Addition (Plan 65-03)

```iron
-- raylib.iron — add alongside Matrix/Quaternion (near line 161, before Rectangle)
-- raymath-private helper types for to_float_v() returns
object Float3 {
    val x: Float32
    val y: Float32
    val z: Float32
}

object Float16 {
    val m0:  Float32; val m1:  Float32; val m2:  Float32; val m3:  Float32
    val m4:  Float32; val m5:  Float32; val m6:  Float32; val m7:  Float32
    val m8:  Float32; val m9:  Float32; val m10: Float32; val m11: Float32
    val m12: Float32; val m13: Float32; val m14: Float32; val m15: Float32
}
```

```c
/* iron_raylib.h — add under Plan 60-02 section marker (or Phase 65 section) */
struct Iron_Float3 {
    float x, y, z;
};

struct Iron_Float16 {
    float m0,  m1,  m2,  m3;
    float m4,  m5,  m6,  m7;
    float m8,  m9,  m10, m11;
    float m12, m13, m14, m15;
};
```

```c
/* iron_raylib_layout.c — add under Plan 65-03 section */
/* Float3: size + 3 offsets */
_Static_assert(sizeof(struct Iron_Float3) == sizeof(float3),
               "Iron_Float3 size must equal float3");
_Static_assert(offsetof(struct Iron_Float3, x) == offsetof(float3, v[0]),
               "Iron_Float3.x offset must equal float3.v[0]");
_Static_assert(offsetof(struct Iron_Float3, y) == offsetof(float3, v[1]),
               "Iron_Float3.y offset must equal float3.v[1]");
_Static_assert(offsetof(struct Iron_Float3, z) == offsetof(float3, v[2]),
               "Iron_Float3.z offset must equal float3.v[2]");

/* Float16: size + 16 offsets (1 sizeof + 16 offsetof pairs) */
_Static_assert(sizeof(struct Iron_Float16) == sizeof(float16), "Iron_Float16 size");
_Static_assert(offsetof(struct Iron_Float16, m0) == offsetof(float16, v[0]), "Iron_Float16.m0");
/* ... repeat for m1..m15 ... */
```

**Grid growth:** 390 existing asserts → 390 + 4 (Float3: 1 sizeof + 3 offsetof) + 17 (Float16: 1 sizeof + 16 offsetof) = **411 asserts** after Plan 65-03. Note: CONTEXT.md said "~3 entries" which understates; actual addition is ~21 entries. The larger number is correct per the one-assert-per-field convention established in Phase 60-02 (see SUMMARY line 81: "45 `_Static_assert` entries: 7 size asserts (+1 extra) + 37 per-field offset asserts").

### Anti-Patterns to Avoid

- **Using `self` as a receiver param name:** E0101 reserved word (Phase 64-01 Rule 1 fix — recurred in Phase 64-02). Use `v`/`m`/`q` instead. Keep C-side shim params named `self` (C does not reserve it; matches 64-01 style).
- **Mixing `RAYMATH_IMPLEMENTATION` and `RAYMATH_STATIC_INLINE` in one TU:** raymath.h line 58 fires `#error`. rcore.c owns IMPLEMENTATION; iron_raylib.c MUST use STATIC_INLINE.
- **Hand-maintaining prototype sync between raylib.iron stubs and iron_raylib.c wrappers:** emit_c (commit e3e5eee) auto-generates prototypes into consumer C TUs. The prototypes in iron_raylib.h exist for review only; they must match emit_c's mangling (`Iron_<lowercased_type>_<method>`).
- **Calling `ironc build` more than once per plan:** ironc consumes ~10 GB RAM. Budget: 1 invocation per plan, 4 total for Phase 65. Use `clang -c` for ABI round-trips during development. `ironc check` has a smaller memory footprint and can be invoked more freely.
- **Declaring `Vector2.zero()` without Iron recognizing it as a static call:** hir_to_lir.c:1240-1287 requires the call site to be a `FUNC_REF` or `IDENT` referring to the type name with no instance binding. Ensure Iron test sites write `Vector2.zero()` as a bare type reference — works per Phase 64 Collision.spheres precedent.
- **Forgetting that Matrix field order is NOT sequential:** raymath's `Matrix { float m0, m4, m8, m12; float m1, m5, m9, m13; ... }` — the field DECLARATION order interleaves columns. Iron's `object Matrix` at raylib.iron:145-160 matches exactly; Phase 60-02 pinned the 16 offsetof asserts. Any shim that hand-constructs a Matrix from component floats must respect this order.
- **Returning Iron arrays from `to_float_v`:** Out of scope per CONTEXT.md Deferred. Return `Float3`/`Float16` objects only.
- **Testing Ray.hit_mesh runtime without LoadModel:** not a Phase 65 concern, but cross-referenced: Phase 64-02 SUMMARY notes Mesh runtime instantiation requires Phase 70. Phase 65 does NOT exercise Mesh.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Tuple typedef `Iron_Tuple_Vector3_Quaternion_Vector3` | Manual registration in `emit_c.c:k_net_tuple_names`-style block | Rely on generic auto-emit (`emit_helpers.c:260-294`) | The auto-emit path is already generic over `elem_count`. The net-specific hardcoded lists at emit_c.c:6334-6480 exist for pre-emit ordering (ensuring the typedef appears BEFORE function prototypes that reference it) — they predate the generic auto-emit. For Phase 65, the auto-emit path activates when ironc encounters a function signature with a tuple return type. Phase 64 Plan 01 SUMMARY explicitly documents: "No manual `emit_c.c` registration fallback was needed" — and that was a 2-tuple case. 3-tuples flow through the same code path. Probe to confirm, don't assume. |
| Function prototypes in iron_raylib.h | Hand-maintain every `Iron_vector2_add(...)` declaration | Rely on emit_c auto-generation (commit e3e5eee) | CONTEXT.md Inherited Decision. Prototypes in iron_raylib.h exist for review, not for compiler consumption. |
| ABI layout assertions beyond what the grid already has | Per-method marshalling verification | Reuse the 390-entry `_Static_assert` grid | Phase 60-02 pinned Vector2/3/4/Matrix/Quaternion byte-for-byte. Only Float3/Float16 need new asserts (Plan 65-03). |
| Matrix math or quaternion slerp implementations | Any Iron-side math logic | Forward to raymath.h's proven C implementations | raymath is 5.5 stable, thoroughly tested in production. Iron never re-implements — shims forward. |
| Cross-type memcpy marshalling | Generic `Iron_Any → raylib_Any` converter | One memcpy per parameter, explicit | Struct-by-value crosses FFI transparently when sizes match. Each memcpy is 2-3 lines of C. Generic converters add complexity with no benefit. |
| Smoke-test reference values for MATH-08 | Code generation from raymath | Hand-picked ~20 call sites spanning every return shape | `Math.float_equals(result, expected)` with EPSILON tolerance. Picking non-trivial inputs that avoid FP edge cases is a 5-minute exercise, not a framework concern. |
| Int-to-Bool coercion helper | Iron-side conversion function | C-shim `(bool)(FloatEquals(x, y) != 0)` | Phase 62 pattern — shim does the coercion; Iron sees clean `Bool`. |
| New stdlib test infrastructure | Unity-style framework for raymath | `tests/manual/raymath_smoke.iron` + `ironc build` + `./raymath_smoke` exit 0 | Phase 64 collision_smoke.iron precedent — one Iron file, printed assert results, exit code gates success. |

**Key insight:** Phase 65 is the biggest function-count phase of the milestone (143 shims) but also the lowest novelty — every ABI mechanism was validated in Phases 60–64. The work is 90% mechanical (memcpy templates scaled over 143 raymath signatures) with 10% novel risk concentrated in three probe points: (1) raymath.h inclusion side-effects (expected clean; raylib.h does NOT pull raymath.h), (2) 3-tuple auto-emission (expected clean; emit_helpers.c:260 generic), (3) Float3/Float16 layout equivalence (expected clean; C contiguous-float guarantee). All three probes fit inside the existing ironc-budget discipline.

## Common Pitfalls

### Pitfall 1: `self` reserved-word trap (KNOWN, recurrent)
**What goes wrong:** Plan text uses `self: Vector2` as receiver param → ironc E0101 "expected parameter name".
**Why it happens:** Iron's parser reserves `self`. Every past plan that wrote `self` got auto-fixed; Phase 64-01 AND 64-02 both hit this.
**How to avoid:** Iron stub receiver param MUST be `v`/`v1`/`v2` (vectors), `m`/`m1`/`m2` (matrices), `q`/`q1`/`q2` (quaternions). C-side shim param keeps `self` (C doesn't reserve it — consistent with Phase 64 shims).
**Warning signs:** Any `func TYPE.method(self: ...)` in raylib.iron.

### Pitfall 2: RAYMATH_STATIC_INLINE position
**What goes wrong:** Define placed AFTER `#include "raymath.h"` → default `inline` mode activates; possible link-time issues.
**Why it happens:** CONTEXT.md specifies position correctly; a copy-paste mistake in the plan could mis-order.
**How to avoid:** The define MUST come before the include. Also: ensure the define is in iron_raylib.c BEFORE ANY other include that might transitively include raymath.h. Current include order at iron_raylib.c:28-30 is `<string.h>` → `iron_raylib.h` → `raylib.h`. Insert the two new lines between iron_raylib.h and raylib.h. Confirmed safe: raylib.h itself does NOT `#include "raymath.h"` — grep of raylib.h shows only a prose comment mention.
**Warning signs:** `clang -c iron_raylib.c` emits undefined references to raymath functions, OR link-time duplicate-symbol errors when combined with rcore.c.

### Pitfall 3: Tuple element name prefix handling
**What goes wrong:** Plan assumes `Iron_Tuple_Iron_Vector3_...` (with `Iron_` prefix on object components).
**Why it happens:** The compiler strips `Iron_` at `types.c:368` — `iron_type_to_string` for OBJECT returns `t->object.decl->name` (raw "Vector3"), not the mangled C name.
**How to avoid:** Canonical names: `Iron_Tuple_Vector3_Vector3`, `Iron_Tuple_Vector3_Float32`, `Iron_Tuple_Vector3_Quaternion_Vector3`. Float32 is the primitive kind name per `iron_type_to_string` IRON_TYPE_FLOAT32 → "Float32". Phase 64-01 SUMMARY documented this exact behavior.
**Warning signs:** Shim signature `Iron_Tuple_Iron_Vector3_...` in iron_raylib.c/h → undefined reference at link time because the auto-emitted typedef uses the stripped name.

### Pitfall 4: Static-method dispatch on Matrix.identity() / Vector2.zero()
**What goes wrong:** Iron resolves `Matrix.identity()` as an instance call requiring a Matrix receiver arg.
**Why it happens:** hir_to_lir.c:1240-1287 static-call detection relies on the object-expression being a FUNC_REF/IDENT referring to the type name WITH no instance mapping. If a local variable is named `Matrix` (unlikely but possible), dispatch would route through the variable.
**How to avoid:** Phase 64 validated `Collision.spheres(...)` static dispatch — works when type name is never shadowed. Plan 65-01/03/04 static stubs: `Vector2.zero()`, `Vector2.one()`, `Matrix.identity()`, `Matrix.frustum/perspective/ortho/look_at`, `Quaternion.identity/from_axis_angle/from_euler/from_vector3_to_vector3`. Smoke test sites: write these as bare type references — `val m = Matrix.identity()` not `val mat = Matrix; val m = mat.identity()`.
**Warning signs:** ironc error "wrong number of arguments" on a call like `Matrix.identity()` where signature is `func Matrix.identity() -> Matrix {}`.

### Pitfall 5: Scalar-helper receiver dispatch (Math namespace)
**What goes wrong:** `Math.lerp(...)` resolves as instance call requiring a Math arg.
**Why it happens:** Same mechanism as Pitfall 4 — Phase 64 `object Collision {}` namespace validated this exact pattern; `Collision.lines(...)` dispatches statically.
**How to avoid:** Add `object Math {}` exactly once. Write `Math.lerp(0.0, 10.0, 0.5)`, never shadow `Math` with a local. Matches Collision precedent (Plan 64-01).
**Warning signs:** Same as Pitfall 4.

### Pitfall 6: Matrix pass-by-value warnings
**What goes wrong:** `clang -Wall -Wextra` fires `-Wlarge-by-value-copy` on Matrix (64 B) or Mesh (120 B) arguments.
**Why it happens:** Clang's default threshold is 64 bytes; Matrix is exactly at the boundary.
**How to avoid:** Phase 64-02 proved Mesh (120 B) and Matrix (64 B) pass clean WITHOUT `-Wno-large-by-value-copy`. The default threshold fires at >64 bytes, and 64 is not strictly greater. Expect zero warnings. If warnings appear (could happen if build flags tighten the threshold), suppress per-file: `#pragma clang diagnostic ignored "-Wlarge-by-value-copy"` around the affected functions.
**Warning signs:** Build log contains `warning: ... byte is larger than 64 bytes [-Wlarge-by-value-copy]`.

### Pitfall 7: 3-tuple auto-emit unlikely-but-possible failure
**What goes wrong:** `Iron_Tuple_Vector3_Quaternion_Vector3` auto-emits with WRONG field count (e.g., only v0/v1).
**Why it happens:** If `hir_to_lir.c` or `analyzer/types.c` has a 2-element hardcode somewhere overlooked by `emit_helpers.c`.
**How to avoid:** Plan 65-04 Task 1 PROBE. Build a tiny caller that binds `Matrix.decompose`, invoke ironc with `--debug-build`, inspect `.iron-build/probe_*.c` for typedef content. Expected: `typedef struct { Iron_Vector3 v0; Iron_Quaternion v1; Iron_Vector3 v2; } Iron_Tuple_Vector3_Quaternion_Vector3;`. If output has fewer than 3 fields, switch to `object MatrixDecomposition`.
**Warning signs:** Generated C typedef has `v0` and `v1` but no `v2`; shim body references `out.v2` → compile error.
**Evidence this is unlikely:** `emit_helpers.c:288-293` loop is `for (int i = 0; i < tuple_ty->tuple.elem_count; i++)` — generic. `types.c:177-181` mangler loop is also `for (int i = 0; i < count; i++)`. Both are arity-agnostic by construction.

### Pitfall 8: Float3/Float16 layout drift if raymath changes
**What goes wrong:** Future raylib update changes `float3` to `struct { float x, y, z; }` (no array); Iron-side byte equivalence breaks.
**Why it happens:** raymath is stable but not frozen.
**How to avoid:** The `_Static_assert` grid catches drift at compile time. Plan 65-03 Task 3 adds the asserts per-field using `offsetof(float3, v[0])` — if raymath drops the `v[]` field, asserts fail immediately with a build error.
**Warning signs:** `clang -c iron_raylib_layout.c` fails with static-assertion error citing Float3 or Float16. Read the failing assert to identify what changed.

### Pitfall 9: DEG2RAD / RAD2DEG / EPSILON as Iron constants
**What goes wrong:** Iron `val DEG2RAD: Float32 = Float32(0.01745329251994329577)` gets a double-to-float narrowing warning, or the Iron compiler rejects the literal as Float (not Float32).
**Why it happens:** CONTEXT.md Specific #9 spelled out the syntax. The `Float32(...)` cast may or may not accept Float literals at compile time depending on Iron's current constant-expression rules.
**How to avoid:** If `Float32(0.017...)` rejects, use `Float32(0.017453293)` (single-precision-representable exactly) or defer the constants to Phase 73 per Deferred Ideas. Claude discretion per CONTEXT.md.
**Warning signs:** ironc emits E-code on module-level `val DEG2RAD` line.

## Code Examples

### Example 1: RAYMATH_STATIC_INLINE header add (Plan 65-01 Task 1)

```c
/* Source: iron_raylib.c current state (lines 27-30) + CONTEXT.md inclusion strategy */
/* BEFORE: */
#include <string.h>
#include "iron_raylib.h"
#include "raylib.h"

/* AFTER Plan 65-01 Task 1: */
#include <string.h>
#include "iron_raylib.h"
#define RAYMATH_STATIC_INLINE
#include "raymath.h"
#include "raylib.h"
```

### Example 2: Vector2 method set (sample from 30)

```iron
-- raylib.iron — Vector2 method stubs (Plan 65-01 Task 3 excerpt)
-- Source: raymath.h lines 236-620 (Vector2 block)
func Vector2.zero() -> Vector2 {}
func Vector2.one() -> Vector2 {}
func Vector2.add(v: Vector2, other: Vector2) -> Vector2 {}
func Vector2.add_value(v: Vector2, add: Float32) -> Vector2 {}
func Vector2.subtract(v: Vector2, other: Vector2) -> Vector2 {}
func Vector2.subtract_value(v: Vector2, sub: Float32) -> Vector2 {}
func Vector2.length(v: Vector2) -> Float32 {}
func Vector2.length_sqr(v: Vector2) -> Float32 {}
func Vector2.dot_product(v: Vector2, other: Vector2) -> Float32 {}
func Vector2.distance(v: Vector2, other: Vector2) -> Float32 {}
func Vector2.distance_sqr(v: Vector2, other: Vector2) -> Float32 {}
func Vector2.angle(v: Vector2, other: Vector2) -> Float32 {}
func Vector2.line_angle(start: Vector2, end: Vector2) -> Float32 {}
func Vector2.scale(v: Vector2, scale: Float32) -> Vector2 {}
func Vector2.multiply(v: Vector2, other: Vector2) -> Vector2 {}
func Vector2.negate(v: Vector2) -> Vector2 {}
func Vector2.divide(v: Vector2, other: Vector2) -> Vector2 {}
func Vector2.normalize(v: Vector2) -> Vector2 {}
func Vector2.transform(v: Vector2, mat: Matrix) -> Vector2 {}
func Vector2.lerp(v: Vector2, other: Vector2, amount: Float32) -> Vector2 {}
func Vector2.reflect(v: Vector2, normal: Vector2) -> Vector2 {}
func Vector2.min(v: Vector2, other: Vector2) -> Vector2 {}
func Vector2.max(v: Vector2, other: Vector2) -> Vector2 {}
func Vector2.rotate(v: Vector2, angle: Float32) -> Vector2 {}
func Vector2.move_towards(v: Vector2, target: Vector2, max_distance: Float32) -> Vector2 {}
func Vector2.invert(v: Vector2) -> Vector2 {}
func Vector2.clamp(v: Vector2, min: Vector2, max: Vector2) -> Vector2 {}
func Vector2.clamp_value(v: Vector2, min: Float32, max: Float32) -> Vector2 {}
func Vector2.equals(v: Vector2, other: Vector2) -> Bool {}
func Vector2.refract(v: Vector2, n: Vector2, r: Float32) -> Vector2 {}
```

### Example 3: Shim set (sample from 143)

```c
/* Source: raymath.h:236-256 + Phase 63-04 memcpy-out template */

struct Iron_Vector2 Iron_vector2_zero(void) {
    Vector2 r = Vector2Zero();
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_add(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Add(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

float Iron_vector2_length(struct Iron_Vector2 self) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    return Vector2Length(v);
}

bool Iron_vector2_equals(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    return (bool)(Vector2Equals(a, b) != 0);
}
```

### Example 4: Smoke-test skeleton (tests/manual/raymath_smoke.iron)

```iron
-- tests/manual/raymath_smoke.iron — Phase 65 canonical regression test
-- Plan 65-01 creates; 65-02/03/04 append.

import raylib

func main() -> Int32 {
    -- MATH-01: scalar helpers
    val lerped  = Math.lerp(0.0, 10.0, 0.5)        -- expect 5.0
    val clamped = Math.clamp(-3.0, 0.0, 5.0)       -- expect 0.0
    val wrapped = Math.wrap(370.0, 0.0, 360.0)     -- expect 10.0
    val remap1  = Math.remap(5.0, 0.0, 10.0, 100.0, 200.0)  -- expect 150.0
    val feq     = Math.float_equals(1.0 + 2.0, 3.0)  -- expect true
    val norm    = Math.normalize(7.5, 5.0, 10.0)   -- expect 0.5

    -- MATH-02: Vector2 (30 call sites in Plan 65-01)
    val v  = Vector2(3.0, 4.0)
    val w  = Vector2(1.0, 2.0)
    val vw = v.add(w)                          -- expect (4.0, 6.0)
    val len = v.length()                       -- expect 5.0
    val zero = Vector2.zero()                  -- expect (0.0, 0.0)
    -- ... 27 more Vector2 call sites ...

    -- MATH-08 ABI asserts (subset: 20 across all return shapes)
    val pass_lerp  = Math.float_equals(lerped,  5.0)
    val pass_len   = Math.float_equals(len,     5.0)
    val pass_vw_x  = Math.float_equals(vw.x,    4.0)
    val pass_vw_y  = Math.float_equals(vw.y,    6.0)
    -- ... 16 more ABI checks ...

    if pass_lerp and pass_len and pass_vw_x and pass_vw_y {
        print("ALL MATH-08 ASSERTS PASS")
    } else {
        print("MATH-08 FAILURE")
        return Int32(1)
    }

    return Int32(0)
}
```

### Example 5: 3-tuple probe (Plan 65-04 Task 1)

```iron
-- probe_matrix_decompose.iron — Plan 65-04 Task 1 only; deleted after inspection
import raylib

func main() -> Int32 {
    val m = Matrix.identity()
    val (trans, rot, scale) = m.decompose()
    print(trans.x)
    print(rot.w)
    print(scale.z)
    return Int32(0)
}
```

```bash
./build/ironc build probe_matrix_decompose.iron --debug-build
grep -A1 'Iron_Tuple_Vector3' .iron-build/probe_matrix_decompose.c
# EXPECTED output:
# typedef struct { Iron_Vector3 v0; Iron_Quaternion v1; Iron_Vector3 v2; } Iron_Tuple_Vector3_Quaternion_Vector3;
```

### Example 6: Out-param → tuple shim (QuaternionToAxisAngle)

```c
/* Source: raymath.h:2416 QuaternionToAxisAngle */
Iron_Tuple_Vector3_Float32 Iron_quaternion_to_axis_angle(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Vector3 axis;
    float angle;
    QuaternionToAxisAngle(q, &axis, &angle);
    Iron_Tuple_Vector3_Float32 out;
    memcpy(&out.v0, &axis, sizeof(Vector3));
    out.v1 = angle;
    return out;
}
```

```c
/* iron_raylib.h — guarded typedef (mirrors Phase 64-01 IRON_TUPLE_BOOL_VECTOR2 pattern) */
#ifndef IRON_TUPLE_VECTOR3_FLOAT32_STRUCT_DEFINED
#define IRON_TUPLE_VECTOR3_FLOAT32_STRUCT_DEFINED
typedef struct {
    struct Iron_Vector3 v0;
    float v1;
} Iron_Tuple_Vector3_Float32;
#endif
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Hand-coded `emit_c.c` tuple-name registration per type | Generic auto-emit over arbitrary arity | Phase 59/64 (`emit_helpers.c:260`) | Phase 65 3-tuple just works — no registration patch needed. |
| Hand-maintain prototypes in iron_raylib.h sync with raylib.iron stubs | emit_c auto-generates consumer C prototypes | Commit e3e5eee (2026-04-14) | Phase 65 saves ~143 lines of prototype bookkeeping. |
| `extern func` external FFI declarations | Empty-body foreign-method stubs | Phase 60-02 rewrite | Phase 65 uses new pattern exclusively. |
| Operator overloading `v1 + v2` for math types | Method form `v1.add(v2)` | N/A — Iron has no operator overloading | Consumer code is slightly verbose; CONTEXT.md Deferred for Phase 73 if Iron gains the feature. |

**Deprecated/outdated:**
- `RAYMATH_IMPLEMENTATION` in iron_raylib.c — rejected by CONTEXT.md and confirmed by code review (rcore.c already holds it at line 116). Never use.
- Old Vec2/Color/Key/RED/InitWindow stubs from pre-Phase-60 raylib.iron — deleted in Plan 60-02. Phase 65 references the NEW Vector2/Color/Quaternion objects only.

## Open Questions

1. **Float32 literal ergonomics for module constants (DEG2RAD/RAD2DEG/EPSILON)**
   - What we know: CONTEXT.md Specific #9 proposes `val DEG2RAD: Float32 = Float32(0.01745329251994329577)`. Iron's general type-cast syntax is `Float32(double_literal)`.
   - What's unclear: Whether Iron's type checker accepts the cast at compile-time with no runtime cost, or whether the 18-significant-digit literal is truncated silently on narrowing.
   - Recommendation: Plan 65-01 Task 2 tries the `Float32(...)` cast form first; if ironc complains, fall back to single-precision-representable literals (e.g., `0.017453293` which is Float32-exact). If both routes fail, defer DEG2RAD/RAD2DEG/EPSILON to Phase 73 per the Deferred-Ideas fallback. Claude discretion per CONTEXT.md.

2. **Whether Iron's positional constructor supports 16-argument types**
   - What we know: Phase 60-02 established positional constructors in field-declaration order for up to Matrix (16 fields). Smoke tests validate `Matrix(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, ...)` works.
   - What's unclear: Whether a user-written smoke test can idiomatically construct a `Float16` literal with 16 positional args without surpassing an Iron parser limit.
   - Recommendation: If `Float16(0.0, ...)` fails to parse or emits a diagnostic, the smoke test can use `val f = Matrix.identity().to_float_v()` to obtain a Float16 via raymath rather than literal construction. No blocker — smoke test only needs to exercise the to-float-v path, not construct Float16 from literals.

3. **EmitCtx tuple-dedup across multiple .iron files in the smoke test**
   - What we know: `emit_helpers.c:266` dedupes tuple typedefs within one translation unit (`ctx->emitted_tuples`).
   - What's unclear: When ironc compiles `raymath_smoke.iron`, it produces ONE C file per Iron module (typically one). `raymath_smoke.iron` is a single-file smoke → single C file → single EmitCtx → dedup works trivially. Multi-file Iron programs that each reference `Iron_Tuple_Vector3_Float32` might emit the typedef in multiple C files (one per TU); the guarded typedef in iron_raylib.h plus `#ifndef X_STRUCT_DEFINED` prevents double-definition at link time.
   - Recommendation: No action needed for Phase 65 — smoke test is single-file. Future multi-file consumers of tuple-returning raymath methods will need guarded typedefs matching Phase 64-01's `IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED` pattern. Plan 65-04 can optionally add `IRON_TUPLE_VECTOR3_VECTOR3_STRUCT_DEFINED`, `IRON_TUPLE_VECTOR3_FLOAT32_STRUCT_DEFINED`, `IRON_TUPLE_VECTOR3_QUATERNION_VECTOR3_STRUCT_DEFINED` to iron_raylib.h as insurance (matches 64-01 style).

4. **raymath `QuaternionEquals` / `Vector2Equals` / `Vector3Equals` / `Vector4Equals` return int**
   - What we know: raymath.h:2512 `QuaternionEquals` returns `int`; Iron-side is `-> Bool`. Same pattern as `FloatEquals`.
   - What's unclear: Whether every equals function needs the `(bool)(... != 0)` coercion explicitly, or if returning raw `int` as `bool` is safe in C (0 = false, non-0 = true, both assignable).
   - Recommendation: Use the explicit coercion for all equals-returning shims. Matches Phase 62 pattern. Cost: one extra token per shim × 6 equals functions (Vector2/3/4, Quaternion, Float, Matrix-has-no-equals). Negligible.

## Sources

### Primary (HIGH confidence)
- `src/vendor/raylib/raymath.h` — authoritative header, 2941 lines. Verified: 143 RMAPI functions; float3/float16 at lines 163-169; RAYMATH_STATIC_INLINE semantics at line 72-73; `#error` guard at line 58 prevents both defines simultaneously.
- `src/vendor/raylib/raylib.h` — confirmed does NOT include raymath.h. Only references at lines 18 and 162 are prose comments ("Other modules (raymath, rlgl) also require some of those types").
- `src/vendor/raylib/rcore.c:116` — `#define RAYMATH_IMPLEMENTATION` — proves rcore.c is the canonical out-of-line owner, forcing iron_raylib.c to use RAYMATH_STATIC_INLINE.
- `src/lir/emit_helpers.c:260-294` — `emit_ensure_tuple` generic auto-emit. Verified: `for (int i = 0; i < tuple_ty->tuple.elem_count; i++)` loop over arbitrary arity; no 2-element hardcode. Recursive nested-tuple handling.
- `src/analyzer/types.c:170-205` — `tuple_build_mangled_name` construction. Verified: element-wise concatenation `_elem_elem_elem`, arbitrary arity.
- `src/analyzer/types.c:361-374` — `iron_type_to_string` for OBJECT returns `t->object.decl->name` (raw name, no `Iron_` prefix). Confirms canonical tuple names: `Iron_Tuple_Vector3_Quaternion_Vector3` (no `Iron_` prefix interior).
- `src/hir/hir_to_lir.c:1090-1303` — method-call dispatch mangling. Verified: obj_type->object.decl->name drives mangling; static-call detection at lines 1240-1287; lowercase-type-name construction at lines 1294-1302.
- `src/stdlib/iron_raylib.c` lines 1-40 + 1293 — current state of shim file. Verified: raymath section marker present and empty; existing include order at lines 27-30 (`<string.h>` → `iron_raylib.h` → `raylib.h`).
- `src/stdlib/iron_raylib.h` line 687 — current state of header; raymath marker present and empty.
- `src/stdlib/iron_raylib_layout.c` — current assert count is 390 (not 392 as CONTEXT.md claims). Note: Plan 65-03 expected growth is ~+21 (4 for Float3, 17 for Float16), not "~3" as CONTEXT.md stated. Final: ~411.
- `src/stdlib/raylib.iron` — Phase 64 precedent patterns (object Collision {}, instance methods on Rectangle/Vector2/Ray/BoundingBox, tuple return `Collision.lines`).
- `.planning/phases/60-type-enum-foundation/60-02-SUMMARY.md` — Vector2/3/4/Matrix/Quaternion `_Static_assert` layout pattern.
- `.planning/phases/64-collision-2d-3d/64-01-SUMMARY.md` — Iron_Tuple_Bool_Vector2 canonical naming ("Iron_ prefix STRIPPED"), receiver-param convention (`self` reserved → use `rect`/`point`).
- `.planning/phases/64-collision-2d-3d/64-02-SUMMARY.md` — Matrix/Mesh pass-by-value works zero-warning, first-try.

### Secondary (MEDIUM confidence)
- CONTEXT.md inference of `Iron_Tuple_Vector3_Float32` mangled name: MEDIUM until probe confirms. Evidence strongly supports it (pattern matches Phase 64-01; mangling code is generic), but empirical confirmation lands at Plan 65-04 Task 1.

### Tertiary (LOW confidence)
- None. Every claim in this research is verified against either raymath.h source, Iron compiler source, Phase 60–64 summaries, or C language guarantees.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — raymath 5.5 vendored, file-level inspection; no external deps.
- Architecture: HIGH — all 5 unverified mechanisms resolved green by source review of emit_helpers.c, hir_to_lir.c, types.c; all patterns traced to Phase 60–64 precedents.
- Pitfalls: HIGH — 9 pitfalls cover every risk surface CONTEXT.md identified plus 4 additional ones from source review (float literal ergonomics, Matrix field interleaving, 64-byte pass-by-value warning threshold, static-call receiver shadowing).
- Tuple 3-arity auto-emit: HIGH — source inspection of `emit_helpers.c:288-293` and `types.c:177-181` confirms arity-agnostic loops. Empirical validation deferred to Plan 65-04 probe (proper hygiene; evidence is already sufficient for HIGH confidence).

**Research date:** 2026-04-16
**Valid until:** 30 days (stable vendored library; no upstream changes pending). raymath.h is part of raylib 5.5 (vendored, frozen at src/vendor/raylib); Iron compiler internals (emit_c.c, hir_to_lir.c) are current as of commit 2918be5.
