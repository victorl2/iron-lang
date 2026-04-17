---
phase: 64-collision-2d-3d
verified: 2026-04-16T00:00:00Z
status: passed
score: 20/20 must-haves verified
gaps: []
---

# Phase 64: Collision (2D + 3D) Verification Report

**Phase Goal:** User can test every collision raylib exposes — 2D rectangle/circle/point/line/triangle/polygon combinations and 3D sphere/box/ray/mesh/triangle/quad intersections — invoked as methods on Rectangle/Vector2/Ray/BoundingBox where the receiver is idiomatic.
**Verified:** 2026-04-16
**Status:** PASSED
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | User can call `rect_a.collides(rect_b)` — Rectangle receiver, Bool return | VERIFIED | `func Rectangle.collides(rect: Rectangle, other: Rectangle) -> Bool {}` at raylib.iron:1597; `Iron_rectangle_collides` shim calls `CheckCollisionRecs(a,b)` |
| 2  | User can call `rect.intersection(other)` and receive a Rectangle by value | VERIFIED | `func Rectangle.intersection` at raylib.iron:1598; `Iron_rectangle_intersection` shim memcpy-in/out through `GetCollisionRec` |
| 3  | User can call `rect.contains_point(point)` and `rect.collides_circle(center, radius)` | VERIFIED | Both stubs at raylib.iron:1599-1600; shims call `CheckCollisionPointRec` and `CheckCollisionCircleRec` |
| 4  | User can call `point.inside_triangle`, `point.inside_polygon`, `point.on_line` — Vector2 receiver methods | VERIFIED | Three stubs at raylib.iron:1604-1606; shims use `CheckCollisionPointTriangle`, `CheckCollisionPointPoly`, `CheckCollisionPointLine` |
| 5  | `object Collision {}` namespace exists, routes `Collision.circles`, `Collision.circle_line`, `Collision.point_circle` | VERIFIED | `object Collision {}` at raylib.iron:962; three pair-based 2D stubs at lines 1609-1611 |
| 6  | `Collision.lines` returns `(Bool, Vector2)` tuple — tuple-return through foreign-method-stub | VERIFIED | `func Collision.lines(...) -> (Bool, Vector2) {}` at raylib.iron:1612; shim `Iron_collision_lines` returns `Iron_Tuple_Bool_Vector2` by value; guarded typedef in iron_raylib.h:644-650 |
| 7  | `box_a.collides(box_b)` — BoundingBox receiver, lowers to `Iron_boundingbox_collides` (concatenated lowercase) | VERIFIED | `func BoundingBox.collides(box: BoundingBox, other: BoundingBox) -> Bool {}` at raylib.iron:1623; `Iron_boundingbox_collides` shim calls `CheckCollisionBoxes`; no `Iron_bounding_box` form present |
| 8  | `box.collides_sphere(center, radius)` — mixed BoundingBox + Vector3 marshalling | VERIFIED | raylib.iron:1624; shim `Iron_boundingbox_collides_sphere` uses memcpy for both BoundingBox and Vector3 inputs |
| 9  | `ray.hit_sphere/box/triangle/quad` — Ray-receiver methods returning RayCollision struct by value (32 B) | VERIFIED | Four stubs at raylib.iron:1627-1628, 1630-1631; five RayCollision memcpy-out returns in iron_raylib.c (grep count: 5) |
| 10 | `ray.hit_mesh(mesh, transform)` — first Mesh (120 B) + Matrix (64 B) pass-by-value through the FFI | VERIFIED | raylib.iron:1629; `Iron_ray_hit_mesh` shim has `memcpy(&m, &mesh, sizeof(Mesh));` and `memcpy(&tx, &transform, sizeof(Matrix));`; clang -c exits 0 zero warnings |
| 11 | `Collision.spheres(c1, r1, c2, r2)` — 3D pair-based test in Collision namespace | VERIFIED | `func Collision.spheres` at raylib.iron:1634; shim calls `CheckCollisionSpheres(a, r1, b, r2)` |
| 12 | `clang -c iron_raylib.c -Wall -Wextra` exits 0 zero warnings | VERIFIED | Ran during verification: exit code 0, no output (zero warnings, zero errors) |
| 13 | `clang -c iron_raylib_layout.c -Wall -Wextra` exits 0 — Phase 60 _Static_assert grid at 392 asserts | VERIFIED | Exit code 0; `grep -c '_Static_assert' iron_raylib_layout.c` returns 392 |
| 14 | Total 19 Phase 64 Iron_* symbols defined in iron_raylib.c (11 + 8) | VERIFIED | `grep -cE '^(bool|struct Iron_Rectangle|struct Iron_RayCollision|Iron_Tuple_Bool_Vector2) Iron_(rectangle|vector2|collision|boundingbox|ray)_' iron_raylib.c` returns 19 |
| 15 | `tests/manual/collision_smoke.iron` exists and exercises all 11 2D bindings | VERIFIED | File present; all 11 call sites confirmed: `a.collides(b)`, `a.intersection(b)`, `a.contains_point`, `a.collides_circle`, `p.inside_triangle`, `p.inside_polygon`, `p.on_line`, `Collision.circles`, `Collision.circle_line`, `Collision.point_circle`, `Collision.lines` |
| 16 | Smoke test exercises 7 3D call sites (hit_mesh deferred to Phase 70) | VERIFIED | `box_a.collides(box_b)`, `box_a.collides_sphere`, `ray.hit_sphere`, `ray.hit_box`, `ray.hit_triangle`, `ray.hit_quad`, `Collision.spheres` — all present; Phase 64 Plan 02 marker confirmed at line with "Phase 64 Plan 02" |
| 17 | `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` guard block unchanged (not duplicated) | VERIFIED | Exactly 1 typedef for `Iron_List_Iron_Vector2` in iron_raylib.h; guard at lines 589-590 unchanged |
| 18 | Tuple typedef canonical name is `Iron_Tuple_Bool_Vector2` (not `Iron_Tuple_Bool_Iron_Vector2`) | VERIFIED | Observed in probe and used throughout: iron_raylib.h guarded typedef at line 650, prototype at line 669, shim body at line 1192+1201 |
| 19 | COLL-01 status: Complete in REQUIREMENTS.md | VERIFIED | REQUIREMENTS.md line 127: `- [x] **COLL-01**:...`; traceability table line 359: `| COLL-01 | Phase 64 | Complete |` |
| 20 | COLL-02 status: Complete in REQUIREMENTS.md | VERIFIED | REQUIREMENTS.md line 128: `- [x] **COLL-02**:...`; traceability table line 360: `| COLL-02 | Phase 64 | Complete |` |

**Score:** 20/20 truths verified

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/stdlib/raylib.iron` | `object Collision {}` + 11 2D func stubs + 8 3D func stubs (19 total) | VERIFIED | `object Collision {}` at line 962; 4 Rectangle.* + 3 Vector2.* + 5 Collision.* + 2 BoundingBox.* + 5 Ray.* = 19 func stubs |
| `src/stdlib/iron_raylib.h` | 11 + 8 = 19 Iron_* C prototypes; guarded tuple typedef; no redeclared Iron_List_Iron_Vector2 | VERIFIED | 10 bool prototypes + 1 struct Iron_Rectangle + 1 Iron_Tuple_Bool_Vector2 + 5 struct Iron_RayCollision + 1 bool Iron_collision_spheres = 19; typedef guard at line 640-651 |
| `src/stdlib/iron_raylib.c` | 19 shim implementations with real forwarding bodies (memcpy pattern) | VERIFIED | 19 definitions confirmed; spot-checked Iron_rectangle_collides and Iron_ray_hit_sphere — both have full memcpy-in/out bodies forwarding to raylib |
| `tests/manual/collision_smoke.iron` | Exercises all 11 2D + 7 3D call sites (hit_mesh deferred); tuple destructure present | VERIFIED | File exists; all call sites confirmed; `(Bool, Vector2)` appears at line 44 in destructure position |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `raylib.iron` Rectangle/Vector2/Collision 2D stubs | `iron_raylib.c` 11 Iron_rectangle_*/Iron_vector2_*/Iron_collision_* shims | HIR->LIR type-name-to-lowercase mangling + foreign-method-stub dispatch | WIRED | All 11 C function names use expected lowercase form; `Iron_rectangle_collides`, `Iron_vector2_inside_polygon`, `Iron_collision_lines` etc. present and call matching raylib functions |
| `raylib.iron` BoundingBox/Ray 3D stubs | `iron_raylib.c` Iron_boundingbox_*/Iron_ray_* shims | Same HIR->LIR mangler; `BoundingBox` -> `boundingbox` (concatenated, no underscore) | WIRED | `Iron_boundingbox_collides` and `Iron_boundingbox_collides_sphere` present; zero `Iron_bounding_box` occurrences |
| `Iron_collision_lines` shim | `Iron_Tuple_Bool_Vector2` typedef | ironc auto-emits typedef into consumer C TU; guarded fallback typedef in iron_raylib.h for standalone clang -c | WIRED | `Iron_Tuple_Bool_Vector2` defined at iron_raylib.h:644-650 behind `IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED` guard; prototype and shim body both use the observed name |
| `Iron_ray_hit_mesh` shim | Phase 60 `_Static_assert(sizeof(Iron_Mesh)==sizeof(Mesh))` + `_Static_assert(sizeof(Iron_Matrix)==sizeof(Matrix))` | memcpy trusts layout pin from Phase 60-04 | WIRED | `_Static_assert` grid at 392 entries (unchanged); `memcpy(&m, &mesh, sizeof(Mesh))` and `memcpy(&tx, &transform, sizeof(Matrix))` present; clang -c exits 0 zero warnings |
| `tests/manual/collision_smoke.iron` | All 19 Phase 64 Iron_* shims via ironc end-to-end build | `./build/ironc build` pipeline; binary produced | WIRED | SUMMARY confirms binary produced (Mach-O 64-bit arm64, 2,659,632 bytes, runs to exit 0); commits `0863e53` and `d6b55d3` present in git history |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| COLL-01 | 64-01-PLAN.md | User can test 2D rectangle/circle/point collisions as methods on Rectangle/Vector2 | SATISFIED | 11 2D functions bound; clang -c clean; smoke test builds and runs; REQUIREMENTS.md line 127 marked `[x]`; traceability table line 359 `Complete` |
| COLL-02 | 64-02-PLAN.md | User can test 3D collisions as methods on Ray/BoundingBox | SATISFIED | 8 3D functions bound; RayCollision 32B struct-by-value return across 5 shims; Mesh 120B + Matrix 64B pass-by-value; clang -c clean; smoke test extended and builds; REQUIREMENTS.md line 128 marked `[x]`; traceability table line 360 `Complete` |

No orphaned requirements. ROADMAP.md phase 64 entry at line 48: `- [x] **Phase 64: Collision (2D + 3D)** — All 2D shape and 3D ray/box/sphere collision tests (completed 2026-04-17)`.

---

### Anti-Patterns Found

None. No TODOs, FIXMEs, HAX, or placeholders found in `src/stdlib/raylib.iron`, `src/stdlib/iron_raylib.h`, `src/stdlib/iron_raylib.c`, or `tests/manual/collision_smoke.iron`. All 19 shim bodies are substantive (memcpy-in + raylib call + memcpy-out pattern, not empty `return {}` stubs).

---

### Human Verification Required

None for goal achievement. The ironc build validation was performed by the executing agent (SUMMARY.md records binary at 2,659,632 bytes, exit 0). The only runtime behavior not exercised in the smoke test is `ray.hit_mesh` — intentionally deferred to Phase 70 (Models) because Iron currently lacks null-pointer-literal syntax for the 12 opaque pointer fields required to construct a valid Mesh struct without `LoadModel`. This deferral is documented, rationale is sound, and the shim's ABI correctness is verified by `clang -c`.

---

### Notes on Deviations from Plan (Informational)

Two deviations were auto-fixed by the executing agent and do not affect goal achievement:

1. **`self` is a reserved word in Iron's parser.** Plan stubs used `self: Rectangle` / `self: Ray` etc. as receiver parameter names. The agent renamed them to `rect`/`point` (Plan 64-01) and `box`/`ray` (Plan 64-02). This matches the `iron_net.iron` convention. C-side shim bodies retain `self` as a parameter name (C does not reserve it). All 19 functions behave exactly as designed.

2. **Tuple typedef canonical name is `Iron_Tuple_Bool_Vector2` not `Iron_Tuple_Bool_Iron_Vector2`.** ironc strips the `Iron_` prefix from object element types when composing tuple type names. The probe in Plan 64-01 Task 1 surfaced this. All three affected sites (guarded typedef, prototype, shim body) use the observed canonical name.

---

_Verified: 2026-04-16_
_Verifier: Claude (gsd-verifier)_
