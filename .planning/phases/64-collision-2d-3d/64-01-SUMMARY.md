---
phase: 64-collision-2d-3d
plan: 01
subsystem: stdlib-raylib
tags: [raylib, collision, 2d, tuple-return, foreign-method-stub, instance-methods, coll-01]

requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Rectangle / Iron_Vector2 struct mirrors + 392 _Static_assert layout grid (Plan 60-02, 60-05); object Collision namespace slot next to Window/Draw/Keyboard"
  - phase: 63-2d-drawing
    provides: "Iron_List_Iron_Vector2 typedef + guard at iron_raylib.h:589-596, foreign-method-stub [Vector2] array ABI (63-03 probe outcome A), Rectangle struct-by-value INPUT ABI, Vector2 RETURN memcpy-out template (63-04)"

provides:
  - "11 2D collision bindings (CheckCollisionRecs/Circles/CircleRec/CircleLine/PointRec/PointCircle/PointTriangle/PointLine/PointPoly/Lines + GetCollisionRec)"
  - "object Collision {} namespace type (36th top-level object in raylib.iron, holds pair-based tests without natural receivers)"
  - "First instance methods on data-carrying object types (Rectangle.*, Vector2.*) through raylib stdlib — unblocks Plan 64-02's Ray.* and BoundingBox.* methods"
  - "First Rectangle struct-by-value RETURN across the FFI (Rectangle.intersection -> GetCollisionRec)"
  - "First tuple-return (Bool, Vector2) through a raylib-stdlib foreign-method stub (Collision.lines -> CheckCollisionLines)"
  - "Canonical tuple typedef naming convention observed: Iron_Tuple_Bool_Vector2 (no Iron_ prefix on object element types)"
  - "tests/manual/collision_smoke.iron canonical regression test for all 11 2D collision bindings"

affects:
  - 64-02-collision-3d
  - 65-raymath
  - 66-textures
  - 67-text-fonts
  - 69-3d-drawing
  - 70-models
  - 73-api-polish

tech-stack:
  added: []
  patterns:
    - "Receiver parameter naming: rect (Rectangle), point (Vector2), etc. — `self` is a reserved word in Iron's parser"
    - "Guarded tuple typedef fallback in iron_raylib.h (IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED) mirrors Phase 63-03's IRON_LIST_IRON_VECTOR2 block for standalone clang -c compatibility"
    - "Tuple-return shim returns packed struct by value (out.v0 = scalar; memcpy(&out.v1, &struct_local, sizeof(...)); return out;) — NOT multiple out-pointers as CONTEXT.md initially proposed"

key-files:
  created:
    - "tests/manual/collision_smoke.iron"
  modified:
    - "src/stdlib/raylib.iron (+36 lines — object Collision + 11 func stubs + receiver-naming comments)"
    - "src/stdlib/iron_raylib.h (+28 lines — Iron_Tuple_Bool_Vector2 guarded typedef + 11 prototypes under Phase 64 marker)"
    - "src/stdlib/iron_raylib.c (+93 lines — 11 shim implementations under Phase 64 marker)"
    - ".gitignore (+1 line — /collision_smoke root-level build artifact)"

key-decisions:
  - "Tuple typedef canonical name is Iron_Tuple_Bool_Vector2 (observed, NOT Iron_Tuple_Bool_Iron_Vector2) — ironc strips the Iron_ prefix from object element types when composing tuple names; the field TYPE inside the struct is still Iron_Vector2 (via the typedef at probe_collision_lines.c:24, `typedef struct Iron_Vector2 Iron_Vector2`)"
  - "Receiver parameter renamed from `self` to `rect`/`point` (Rule 1 auto-fix): Iron's parser treats `self` as a reserved word and rejects it with E0101; iron_net.iron's convention (TcpSocket uses `s`, TcpListener uses `l`) was carried into raylib.iron"
  - "IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED guarded typedef added to iron_raylib.h so `clang -c iron_raylib.c` compiles standalone. ironc auto-emits the same typedef into the generated consumer C TU; the guard prevents double-definition in the combined build path"
  - "Iron's tuple-return mechanism works first-try through a stdlib foreign-method stub — matches iron_net.c:586 precedent (Iron_tcpsocket_read returns Iron_Tuple_Int_NetError by value). No manual emit_c.c registration fallback needed"
  - "Instance-method dispatch on data-carrying object types (Rectangle, Vector2) worked on first try through the full ironc pipeline — HIR->LIR mangler (hir_to_lir.c:1096-1297) dispatches uniformly on obj_type->object.decl->name whether or not the object has fields"
  - "Binary artifact /collision_smoke lands at repo root (ironc's default output location); added to .gitignore alongside existing root-level test binary patterns"

patterns-established:
  - "Instance methods on data-carrying Iron objects dispatch cleanly through the stdlib foreign-method-stub mechanism — planner can wire receiver methods on ANY object (Ray, BoundingBox, Mesh, Model, etc.) with confidence"
  - "Packed tuple-return struct-by-value shim pattern: Iron `-> (A, B)` lowers to auto-emitted `typedef struct { A v0; B v1; } Iron_Tuple_A_B;` returned by value. Fallback guarded typedef in iron_raylib.h matches the auto-emit shape byte-for-byte"
  - "Reserved word avoidance: `self` is NOT a valid parameter name in Iron — follow iron_net.iron's short-name convention (`s`/`l`) or a semantic name (`rect`/`point`/`ray`/`box`)"
  - "Standalone clang-c round-trip remains the authoritative ABI verification method — iron_raylib.c is outside the CMake iron_stdlib target, so the tuple typedef must either be auto-emitted into the generated C TU OR guarded-defined in iron_raylib.h"

requirements-completed: [COLL-01]

duration: 5min
completed: 2026-04-17
---

# Phase 64 Plan 01: 2D Collision Bindings Summary

**11 2D raylib collision functions bound as idiomatic Iron methods — first instance methods on data-carrying object types (Rectangle, Vector2), first Rectangle struct-by-value RETURN across the FFI, first tuple-return `(Bool, Vector2)` through a raylib-stdlib foreign-method stub.**

## Performance

- **Duration:** ~5 min
- **Started:** 2026-04-17T01:34:15Z
- **Completed:** 2026-04-17T01:39:00Z
- **Tasks:** 2
- **Files modified:** 4 (3 modified + 1 created + 1 .gitignore entry)

## Accomplishments

- **Closes COLL-01.** Every 2D collision test from raylib's `rshapes.c` is now a single Iron call: `rect_a.collides(rect_b)`, `rect_a.intersection(rect_b)` (Rectangle return), `point.inside_triangle(a, b, c)`, `point.inside_polygon(pts)`, `Collision.circles(c1, r1, c2, r2)`, `Collision.lines(a, b, c, d)` (tuple return).
- **Instance-method dispatch on data-carrying object types PROVEN.** Plan 64-01 was the first phase to attach methods to `object` types that carry fields (Rectangle/Vector2) rather than to empty namespace types (Keyboard/Draw/etc.). The HIR->LIR mangler at `src/hir/hir_to_lir.c:1096-1297` treats both uniformly. Unblocks Plan 64-02's `Ray.*` and `BoundingBox.*` methods.
- **Tuple-return through stdlib foreign-method stub PROVEN.** Plan 64-01 Task 1 probe confirmed ironc auto-emits `typedef struct { bool v0; Iron_Vector2 v1; } Iron_Tuple_Bool_Vector2;` into the generated consumer C TU when it encounters `Collision.lines(...) -> (Bool, Vector2) {}`. Shim returns the tuple by value (iron_net.c:586 precedent pattern). No manual `emit_c.c` registration fallback was needed.
- **Canonical tuple typedef naming observed.** Iron_Tuple_Bool_Vector2 — the compiler strips the Iron_ prefix from object element names when composing tuple type names. This is input for Phase 64-02 (which may use `(Bool, Vector3)` or `(RayCollision, ...)` returns), Phase 65 (raymath tuple returns), and Phase 70 (model loading tuple returns).
- **First Rectangle struct-by-value RETURN across the FFI** validated via `Iron_rectangle_intersection` using the memcpy-out template from Phase 61's Vector2 returns. Phase 60-02's `_Static_assert(sizeof(Iron_Rectangle) == sizeof(Rectangle))` held.
- **Canonical smoke test landed.** `tests/manual/collision_smoke.iron` exercises every Phase 64 2D binding in one main() body; `./build/ironc build` produces a 2,659,152-byte Mach-O arm64 executable that runs to exit 0.

## Task Commits

1. **Task 1: Probe tuple-return + add object Collision + 11 func stubs + 11 shims** — `84ca4c0` (feat)
2. **Task 2: End-to-end smoke test + self->rect/point rename** — `d6b55d3` (test)

_Plan metadata commit to follow after SUMMARY + STATE + ROADMAP update._

## Files Created/Modified

- `tests/manual/collision_smoke.iron` (created) — canonical 2D collision regression test, exercises all 11 bindings + tuple destructure.
- `src/stdlib/raylib.iron` (modified) — added `object Collision {}` + 11 func stubs. Receiver params named `rect`/`point` (not `self` — reserved word).
- `src/stdlib/iron_raylib.h` (modified) — added guarded `Iron_Tuple_Bool_Vector2` typedef + 11 C prototypes under the pre-scaffolded Phase 64 section marker.
- `src/stdlib/iron_raylib.c` (modified) — added 11 shim implementations (4 Rectangle + 3 Vector2 + 4 Collision) using memcpy-in / memcpy-out / Iron_List_Iron_Vector2 by-value patterns from Phases 60-63.
- `.gitignore` (modified) — added `/collision_smoke` for the root-level build artifact.

## Decisions Made

1. **Tuple typedef canonical name is `Iron_Tuple_Bool_Vector2`** (observed via probe, NOT `Iron_Tuple_Bool_Iron_Vector2` as the plan anticipated). ironc strips the `Iron_` prefix from object element types in tuple type names; the field TYPE inside the struct is still `Iron_Vector2` (via `typedef struct Iron_Vector2 Iron_Vector2`).
2. **Receiver param named `rect`/`point`**, not `self` (Rule 1 auto-fix — see Deviations). Follows iron_net.iron's existing convention (TcpSocket `s`, TcpListener `l`).
3. **Guarded `IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED` typedef added to iron_raylib.h.** Required so `clang -c iron_raylib.c` standalone (outside CMake iron_stdlib target) compiles — iron_raylib.c references `Iron_Tuple_Bool_Vector2` but ironc auto-emits the typedef only into the generated consumer C TU. Mirrors the `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` pattern at iron_raylib.h:589.
4. **Probe produced no file-change commit.** Task 1 Step 1 added a temporary `func Collision.lines` stub, ran `ironc build --debug-build` on a tiny caller, inspected the generated C at `.iron-build/probe_collision_lines.c:800`, then removed both the probe stub and the generated C file. Only the main 11 stubs (added in Step 3) and `object Collision {}` (Step 2) survive.
5. **Two ironc invocations total for Plan 64-01.** Task 1 probe: 1 invocation. Task 2 smoke build: 1 invocation. Within HANDOFF.md memory budget (~10 GB per invocation).
6. **`ironc check` invoked twice** (once before probe, once after receiver-rename). `check` has a much smaller memory footprint than `build`.
7. **Binary artifact `/collision_smoke` ignored.** Added to .gitignore under the existing "Compiled binaries (root-level test outputs from iron build)" section.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] `self` is a reserved word in Iron's parser**
- **Found during:** Task 2 (canonical ironc build)
- **Issue:** The plan's verbatim stub text used `self: Rectangle` / `self: Vector2` as the receiver parameter name. `./build/ironc check src/stdlib/raylib.iron` exited 0 silently in the probe run (the probe stub used `start_a, end_a, ...`), but `./build/ironc check src/stdlib/raylib.iron` after Task 1 Step 3 produced seven `E0101: expected parameter name` errors on every stub that used `self`. Cross-referenced with iron_net.iron which uses `s` (TcpSocket), `l` (TcpListener) — never `self`. `self` is a reserved word in Iron's grammar.
- **Fix:** Renamed receiver param `self` -> `rect` on all four Rectangle stubs, `self` -> `point` on all three Vector2 stubs. Added an Iron-side comment noting the convention.
- **Files modified:** src/stdlib/raylib.iron
- **Verification:** `./build/ironc check src/stdlib/raylib.iron` exits 0. `./build/ironc build tests/manual/collision_smoke.iron` exits 0.
- **Committed in:** d6b55d3 (Task 2 commit)

**2. [Rule 3 - Blocking] Binary artifact at repo root not in .gitignore**
- **Found during:** Task 2 (after ironc build succeeded)
- **Issue:** ironc dumps the built binary alongside/near the source file; for `tests/manual/collision_smoke.iron` the binary landed at `/collision_smoke` (repo root). Untracked — would either be accidentally committed on `git add -A` or noise `git status`.
- **Fix:** Added `/collision_smoke` to .gitignore under the existing "Compiled binaries (root-level test outputs from iron build)" section.
- **Files modified:** .gitignore
- **Verification:** `git check-ignore collision_smoke` reports the path is ignored.
- **Committed in:** d6b55d3 (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (1 bug, 1 blocking)
**Impact on plan:** Both auto-fixes necessary. The `self` rename is a minor naming difference; no functional impact — all 11 methods behave exactly as planned. Binary gitignore is hygiene. No scope creep.

## Issues Encountered

- **Probe step revealed a different canonical tuple name than anticipated.** The plan expected `Iron_Tuple_Bool_Iron_Vector2`; actual observed was `Iron_Tuple_Bool_Vector2`. Handled automatically — probe's purpose is precisely to surface this. All three affected sites (iron_raylib.h guarded typedef, iron_raylib.h prototype, iron_raylib.c shim) were authored with the observed name, not the anticipated name.
- **No manual emit_c.c registration fallback needed.** The plan's contingency path (add `Iron_Tuple_Bool_Vector2` to `emit_ensure_tuple` in `src/lir/emit_c.c`) was not exercised because the auto-emit path already handles `(Bool, Vector2)` tuples correctly — ironc's tuple-emission logic generalizes beyond the net-tuple pairs that were its original use case.

## Key Validation Outputs

- **Probe typedef name (Task 1):** `typedef struct { bool v0; Iron_Vector2 v1; } Iron_Tuple_Bool_Vector2;` observed at `.iron-build/probe_collision_lines.c:800` (probe file deleted after inspection).
- **Instance-method dispatch worked on first try:** `ironc check src/stdlib/raylib.iron` exits 0 after the receiver-rename fix; `ironc build collision_smoke.iron` exits 0. No fallback to namespace-only form (`Collision.rects(a, b)`) was needed.
- **`clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra`** exits 0 with 0 warnings.
- **`clang -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra`** exits 0 with 0 warnings — Phase 60 _Static_assert grid unchanged at **392** asserts.
- **`./build/ironc build tests/manual/collision_smoke.iron`** exits 0, produces Mach-O 64-bit arm64 executable (2,659,152 bytes). 0 errors + 0 warnings in build log. `./collision_smoke` runs to exit 0.
- **ironc invocation count:** 2 (Task 1 probe + Task 2 smoke build) — within HANDOFF.md memory discipline.

## User Setup Required

None — no external service configuration. raylib is vendored at `src/vendor/raylib/`.

## Next Phase Readiness

- **Plan 64-02 (3D collision, COLL-02) fully unblocked.** All three "unverified mechanisms" flagged in Phase 64 CONTEXT.md are now GREEN:
  - Instance-method dispatch on data-carrying objects WORKS (proven via Rectangle.collides / Vector2.inside_triangle).
  - Tuple-return through foreign-method stub WORKS (proven via Collision.lines with Iron_Tuple_Bool_Vector2).
  - Struct-by-value RETURN of composite type WORKS (proven via Rectangle.intersection; Plan 64-02 extends to RayCollision — already validated in Phase 60-05 layout for the bool+padding).
- **Plan 64-02 should reuse the receiver-param convention:** `ray: Ray`, `box: BoundingBox` (NOT `self`).
- **Plan 64-02's `Mesh` pass-by-value** and `Matrix` pass-by-value are NOT stressed by Plan 64-01; first-ever exercise lands in 64-02.
- **pong.iron / game_raylib.iron / hello_raylib.iron** were NOT modified (no PHASE 64 markers exist in those files). Phase 64 is the first phase since 60 that completes without touching the consumer files — confirmed.
- **Spot-check:** pong.iron build was NOT re-run in this plan's execution (memory discipline — ironc 2x budget consumed by probe + smoke). Plan 64-01 touched only the Collision section of raylib.iron/iron_raylib.{h,c}; no reason pong.iron would regress.

## Self-Check: PASSED

- tests/manual/collision_smoke.iron: FOUND
- src/stdlib/raylib.iron (object Collision + 11 stubs): FOUND
- src/stdlib/iron_raylib.h (11 prototypes + guarded tuple typedef): FOUND
- src/stdlib/iron_raylib.c (11 shim implementations): FOUND
- .gitignore (/collision_smoke entry): FOUND
- Commit 84ca4c0 (Task 1): FOUND
- Commit d6b55d3 (Task 2): FOUND

---
*Phase: 64-collision-2d-3d*
*Completed: 2026-04-17*
