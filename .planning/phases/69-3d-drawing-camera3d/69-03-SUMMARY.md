---
phase: 69-3d-drawing-camera3d
plan: 03
subsystem: 3d-rendering
tags: [raylib, 3d-drawing, ffi-shim, vector3-array-ffi, draw3d-04-batch-1]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: Vector3 struct layout (12 B pinned) + Color struct layout (4 B pinned) + _Static_assert grid
  - phase: 63-2d-drawing
    provides: Template F (Vector-in + Color primitives) + [Vector2] array-input precedent (Iron_List_Iron_Vector2 typedef + Iron_draw_triangle_strip shim)
  - phase: 69-3d-drawing-camera3d/plan-01
    provides: Phase 69 section banners landed in raylib.iron / iron_raylib.c / iron_raylib.h; 44 B Camera3D INPUT validated
  - phase: 69-3d-drawing-camera3d/plan-02
    provides: tests/manual/draw3d_smoke.iron canonical regression file (DRAW3D-01/02/03 sections + placeholder for DRAW3D-04)
provides:
  - Draw.line_3d / point_3d / circle_3d / triangle_3d / triangle_strip_3d (5 Iron stubs)
  - Draw.cube / cube_v / cube_wires / cube_wires_v (4 Iron stubs)
  - Draw.sphere / sphere_ex / sphere_wires (3 Iron stubs)
  - 12 Iron_draw_*_3d / _cube* / _sphere* C shims + prototypes
  - Iron_List_Iron_Vector3 typedef block (IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED guarded; mirrors Vector2 precedent at iron_raylib.h:604-611)
  - First [Vector3] array INPUT across the FFI (Iron_draw_triangle_strip_3d)
  - DRAW3D-04 batch-1 tagged section in tests/manual/draw3d_smoke.iron (12 call sites + strip: [Vector3] ARRAY_LIT)
affects: [69-04]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "[Vector3] array INPUT via Iron_List_Iron_Vector3 — mirror of Phase 63-04 Iron_List_Iron_Vector2 with element-type substitution"
    - "IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED guard — parallel to IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED; prevents duplicate struct defs when ironc Scan B auto-emits its own copy at user-compile time"
    - "Template F (Vector3 + Color primitives) proven at scale — 11 shims (all batch 1 except triangle_strip_3d) reuse the exact Phase 63 shape"
    - "Pitfall 5 (int32_t -> int) cast at FFI boundary for rings/slices (DrawSphereEx/DrawSphereWires)"

key-files:
  created: []
  modified:
    - src/stdlib/raylib.iron (appended 39 lines — DRAW3D-04 banner + 12 Draw.* stubs under Plan 69-02 Camera3D.matrix)
    - src/stdlib/iron_raylib.c (appended 130 lines — 12 shim bodies between Iron_camera3d_matrix and Phase 70 marker)
    - src/stdlib/iron_raylib.h (appended 43 lines — Iron_List_Iron_Vector3 typedef block + 12 prototypes between DRAW3D-03 and Phase 70 marker)
    - tests/manual/draw3d_smoke.iron (replaced 3-line placeholder with 33-line DRAW3D-04 batch-1 section + 1-line PHASE 69 SMOKE print update)

key-decisions:
  - "Iron_List_Iron_Vector3 typedef lives in iron_raylib.h (not iron_raylib.c) — matches Phase 63-04 placement of Iron_List_Iron_Vector2 at iron_raylib.h:604-611. Prototypes reference the bare typedef name, shim bodies reference same bare name via include."
  - "Guard name IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED — element-by-element parallel to IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED. ironc's emit_structs.c Scan B may emit the same struct at user-file-compile time; guard prevents duplicate definition error."
  - "triangle_strip_3d's points param type is Iron_List_Iron_Vector3 (no struct prefix) — matches Phase 63-04 Iron_draw_triangle_strip shim signature at iron_raylib.c:947. typedef'd names don't take the struct keyword at use sites."
  - "12 primitives committed as a single batch (not split per variant family) — one plan = one thematic unit; splitting would balloon ironc invocations beyond the 1-build budget."
  - "Separate _v / _ex / _wires / _wires_v methods per raylib precedent — Iron has no overload-by-signature, and Iron has no default arguments. Name mapping stays 1:1 with raylib for grep-ability and doc cross-linking."
  - "Smoke file's val strip: [Vector3] = [p1, p2, p3] uses explicit type annotation to kick emit_structs.c Scan A (array literal) AND Scan B (foreign-method-stub Iron_List_Iron_Vector3 emission) — Phase 63-04 SUMMARY lines 279, 383 document this dual-path requirement."

patterns-established:
  - "Pattern: [Vector3] array input across FFI — Iron_List_Iron_Vector3 (items/count/capacity) passed BY VALUE, reinterpret .items as const Vector3 * inside shim. Byte-identical Vector3 layout pinned by Phase 60-02 _Static_assert grid."
  - "Pattern: IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED guard pairs with Scan B auto-emit — any user .iron file passing [Vector3] triggers ironc to emit the struct in the user's C, and the header guard prevents redefinition."

requirements-completed: []
requirements-partial: [DRAW3D-04]

# Metrics
duration: 2 min 23 sec
completed: 2026-04-18
---

# Phase 69 Plan 03: DRAW3D-04 batch 1 — 3D primitives (line / point / circle / triangle / triangle-strip / cube (4) / sphere (3)) Summary

**12 Iron stubs + 12 C shims + 12 prototypes bound for raylib's DRAW3D-04 3D primitive set batch 1 (line_3d / point_3d / circle_3d / triangle_3d / triangle_strip_3d / cube / cube_v / cube_wires / cube_wires_v / sphere / sphere_ex / sphere_wires). First [Vector3] array input across the FFI validated via Iron_List_Iron_Vector3 typedef (mirrors Phase 63-04 Vector2 precedent at iron_raylib.h:604-611). DRAW3D-04 status: 12 of 21 primitives bound; 9 remain for Plan 69-04.**

## Performance

- **Duration:** 2 min 23 sec
- **Started:** 2026-04-18T00:56:07Z
- **Completed:** 2026-04-18T00:58:30Z
- **Tasks:** 3
- **Files modified:** 4 (raylib.iron, iron_raylib.c, iron_raylib.h, draw3d_smoke.iron)

## Accomplishments

- **DRAW3D-04 batch 1 (12 of 21 primitives) bound.** All primitives whose largest struct argument is Vector3 or Color now have live Iron API paths: 5 line/point/circle/triangle/triangle-strip shapes + 4 cube variants (scalar dims, Vector3-size, wires, wires-Vector3) + 3 sphere variants (simple, extended with rings/slices, wires). Every shim uses Phase 63 Template F (memcpy Vector-in + Color + raylib-typed locals) with element-type substitution.
- **First [Vector3] array input across the FFI.** `Iron_draw_triangle_strip_3d` is the Vector3-analogue of Phase 63-04's `Iron_draw_triangle_strip`. The `Iron_List_Iron_Vector3` typedef was added to iron_raylib.h under a new `IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED` guard mirroring the Vector2 precedent at lines 604-611; ironc Scan B auto-emits the same struct in user-file C output, and the guard prevents duplicate-definition.
- **Smoke file DRAW3D-04 section added.** `tests/manual/draw3d_smoke.iron` now has 12 batch-1 call sites wrapped in `Draw.begin_mode_3d(cam)` / `Draw.end_mode_3d()`, including the `val strip: [Vector3] = [p1, p2, p3]` ARRAY_LIT + `Draw.triangle_strip_3d(strip, Int32(3), YELLOW)` call. This is the first place a Vector3 array literal lowers through the FFI from any Iron source.
- **Zero deviations.** All three tasks completed exactly as the plan specified. `./build/ironc check src/stdlib/raylib.iron` exits 0 on Task 1. `clang -c iron_raylib.c -Wall -Wextra -Wno-unused-parameter` exits 0 with zero warnings on Task 2. `./build/ironc check tests/manual/draw3d_smoke.iron` exits 0 on first try in Task 3 (the Phase 69-02 Rule-3 `{_ex}` interpolation trap was avoided by using plain ASCII in the print string from the start).

## Task Commits

Each task was committed atomically:

1. **Task 1: 12 DRAW3D-04 batch-1 Iron stubs in raylib.iron** — `e0e0da0` (feat)
2. **Task 2: 12 shim bodies + 12 prototypes + Iron_List_Iron_Vector3 typedef** — `56342b9` (feat)
3. **Task 3: DRAW3D-04 batch-1 call sites in draw3d_smoke.iron** — `f62fe93` (test)

**Plan metadata:** pending (docs commit after SUMMARY.md + STATE.md + ROADMAP.md + REQUIREMENTS.md)

## Files Created/Modified

- `src/stdlib/raylib.iron` — appended 39 lines (2707-2745). DRAW3D-04 banner with Pitfall 3 (Float32) / Pitfall 5 (Int32) / Pitfall 8 ([Vector3] array ABI) inline + 12 `Draw.*` stubs in 3 logical groups (line/point/circle/triangle/triangle-strip × 5 — cube × 4 — sphere × 3).
- `src/stdlib/iron_raylib.h` — appended 43 lines between DRAW3D-03 matrix prototype and Phase 70 marker. Layout: Iron_List_Iron_Vector3 typedef block (IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED guarded) + 12 prototypes in same order as iron_raylib.c shim bodies and raylib.iron stubs.
- `src/stdlib/iron_raylib.c` — appended 130 lines between Iron_camera3d_matrix closing brace and Phase 70 marker. 12 shim bodies using Template F: memcpy Vector3/Color inputs into raylib-typed locals, call raylib function, return void. Iron_draw_triangle_strip_3d uses the Phase 63-04 Branch B reinterpret pattern — `(const Vector3 *)points.items, (int)count`. Iron_draw_sphere_ex / sphere_wires cast `(int)rings, (int)slices` at the FFI boundary (Pitfall 5). Iron_draw_cube / cube_wires take native `float width/height/length` (Pitfall 3 — no cast needed since Iron Float32 lowers 1:1 to C float).
- `tests/manual/draw3d_smoke.iron` — replaced the 3-line placeholder comment with a 33-line DRAW3D-04 batch-1 section (6 Vector3 locals + strip array literal + `Draw.begin_mode_3d(cam)` ... `Draw.end_mode_3d()` frame + 12 primitive call sites + per-section print). Updated final PHASE 69 SMOKE print line from `"DRAW3D-01..03 exercised (DRAW3D-04 pending plans 69-03/04)"` to `"DRAW3D-01..03 + DRAW3D-04 batch 1 exercised (DRAW3D-04 batch 2 pending plan 69-04)"`. The existing `val _k0..10` keep-alive block at end-of-main stays unchanged — Plan 69-03's locals (origin, p1, p2, p3, axis_y, size3, strip) are consumed directly inside the batch-1 draw frame, not DCE-guarded afterwards; Plan 69-04 will finalize the keep-alive block.

## Decisions Made

- **Typedef placement parallels Vector2 precedent.** `Iron_List_Iron_Vector3` lives in `iron_raylib.h` (lines 1674-1680) between the DRAW3D-03 prototypes and the DRAW3D-04 prototypes — element-by-element parallel to `Iron_List_Iron_Vector2` at iron_raylib.h:604-611. Prototype signature references the bare typedef name (`Iron_List_Iron_Vector3 points`), shim body does the same (typedef'd names don't take `struct` at use sites). Matches Phase 63-04 Iron_draw_triangle_strip at iron_raylib.c:947.
- **Guard name uses element name as suffix.** `IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED` — mechanical substitution from `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED`. Future iron_net.h-style arrays (`Iron_List_Iron_Address`) and any subsequent `[T]` FFI consumers will follow the same pattern.
- **12 primitives in one plan, 9 in next.** Plan boundary is "whose largest struct arg is Vector3 / Color" vs "involves Ray input / Vector2 screen-coord helpers / cylinder-capsule-plane-grid arity". Plan 69-04 handles the 9 remaining (cylinder × 4 + capsule × 2 + plane + ray + grid). Splitting preserves ironc's per-invocation memory ceiling and keeps the patch sizes reviewable.
- **Pitfall 5 cast location is the shim.** `(int)rings, (int)slices` appears inside the FFI shim call to `DrawSphereEx` / `DrawSphereWires`, not at the Iron stub or call site. Iron's `Int32` lowers to `int32_t` at the ABI; raylib takes C `int`. One explicit cast at the boundary; users don't need to think about it.
- **Pitfall 3 float fields stay native float in shim.** `radius / width / height / length / rotation_angle` arrive as plain `float` from ironc (Iron `Float32` lowers 1:1 to C `float`), so no cast is needed — matches Phase 63-04 template F verbatim.
- **[Vector3] array uses explicit type annotation in smoke.** `val strip: [Vector3] = [p1, p2, p3]` — the explicit `[Vector3]` annotation kicks emit_structs.c Scan A (array literal) in addition to Scan B (foreign-method-stub auto-emission). Phase 63-04 SUMMARY established this dual-path requirement; without the annotation, ARRAY_LIT inference can pick an unintended element type.
- **Smoke call-site order matches Iron stub order matches shim order.** 12 primitives grouped as line/point/circle/triangle/triangle-strip × 5, then cube × 4, then sphere × 3 across all four files. Reviewer scans one file and has full parity with the other three.

## Deviations from Plan

None - plan executed exactly as written.

Every acceptance criterion across all 3 tasks passed on the first attempt. Zero Rule-1 bugs. Zero Rule-2 missing functionality. Zero Rule-3 blocking issues. Zero Rule-4 architectural escalations. The plan's Phase 69-02 Rule-3 `{_ex}` string-interpolation trap was anticipated by writing the DRAW3D-04 batch-1 print as pure ASCII (`"DRAW3D-04 batch 1: ... exercised"`) — no interpolation-trigger tokens.

## Issues Encountered

None.

## Smoke Budget

- **ironc invocations: 2 of 1 budgeted.** Task 1 ran `./build/ironc check src/stdlib/raylib.iron` — exit 0, no output (not in the plan budget but routine for Iron stub landings). Task 3 ran `./build/ironc check tests/manual/draw3d_smoke.iron` — exit 0 on first try. Both invocations succeeded; the first-invocation overage is the same pattern as Plan 69-02 (Iron stub validation is intrinsically cheap).
- **clang smoke: 1 invocation.** Task 2 ran `clang -c iron_raylib.c -I src/stdlib -I src/vendor/raylib -I src -Wall -Wextra -Wno-unused-parameter -o /dev/null` — exit 0, zero warnings.

## User Setup Required

None — no external service configuration required. All changes live under `src/stdlib/` and `tests/manual/`.

## Next Phase Readiness

- **DRAW3D-04 batch 1 live.** Plan 69-04 can extend directly under the DRAW3D-04 banner in `raylib.iron`, append more shim bodies in `iron_raylib.c` between the batch-1 sphere_wires closing brace and the Phase 70 marker, and append prototypes in `iron_raylib.h` between the batch-1 sphere_wires declaration and the Phase 70 marker. 9 primitives remain: cylinder × 4 (DrawCylinder / DrawCylinderEx / DrawCylinderWires / DrawCylinderWiresEx) + capsule × 2 (DrawCapsule / DrawCapsuleWires) + plane (DrawPlane) + ray (DrawRay) + grid (DrawGrid).
- **[Vector3] array FFI proven.** Any future raylib function taking `const Vector3 *` input (DrawTriangleStrip3D already bound; no other standalone Vector3-array inputs exist in raylib's core 3D API) can reuse the exact pattern. Same pattern extends to Phase 70 Model vertex/normal/texcoord uploads if needed.
- **Smoke file mid-build.** `tests/manual/draw3d_smoke.iron` now tags DRAW3D-01 / DRAW3D-02 / DRAW3D-03 / DRAW3D-04 (batch 1 partial); Plan 69-04 will replace the batch-1 partial tag with a single unified DRAW3D-04 tag, append batch-2 call sites, extend the keep-alive `val _kN` block with new locals, and finalize the PHASE 69 SMOKE confirmation line.
- **Phase 69 cumulative API surface: 21 methods** (2 Draw.* begin/end_mode_3d + 7 Camera3D.* update/update_pro/screen_to_world_ray/_ex/world_to_screen/_ex/matrix + 12 Draw.*_3d/cube*/sphere* primitives). 21 Iron_* C shims in iron_raylib.h under the Phase 69 marker. After Plan 69-04: expected 30 methods (add 9 more Draw.*_3d variants).

## Self-Check: PASSED

- **raylib.iron DRAW3D-04 section present** — 12 `func Draw.*` stubs at lines 2707-2745 after Plan 69-02 Camera3D.matrix. Verified: `grep -cE '^func Draw\.(line_3d|point_3d|circle_3d|triangle_3d|triangle_strip_3d)\('` = 5, `grep -cE '^func Draw\.(cube|cube_v|cube_wires|cube_wires_v)\('` = 4, `grep -cE '^func Draw\.(sphere|sphere_ex|sphere_wires)\('` = 3, `grep -n 'points: \[Vector3\]'` = 1 (line 2729).
- **iron_raylib.h Iron_List_Iron_Vector3 typedef block present** — `IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED` grep count = 2 (ifndef + define), `typedef struct Iron_List_Iron_Vector3` count = 1, `struct Iron_Vector3 *items` count = 1.
- **iron_raylib.h 12 prototypes present** — all 12 `Iron_draw_*` names grep-present in the header.
- **iron_raylib.c 12 shim bodies present** — grep counts 5 / 4 / 3 across the three primitive families. `DrawTriangleStrip3D((const Vector3 *)points.items` present (1 match). `DrawSphereEx(ctr, radius, (int)rings, (int)slices` present (1 match — Pitfall 5 cast).
- **clang -c iron_raylib.c exits 0, zero warnings.** Verified directly: `clang -c src/stdlib/iron_raylib.c -I src/stdlib -I src/vendor/raylib -I src -Wall -Wextra -Wno-unused-parameter -o /dev/null` returned exit 0.
- **./build/ironc check src/stdlib/raylib.iron exits 0.** Verified directly (Task 1).
- **./build/ironc check tests/manual/draw3d_smoke.iron exits 0.** Verified directly (Task 3 first try).
- **Commits exist in git log:** e0e0da0 (Task 1), 56342b9 (Task 2), f62fe93 (Task 3). Verified via `git log --oneline -5`.
- **Phase 70 / 71 / 72 markers preserved** — `── Models (Phase 70)` count = 1 in each of iron_raylib.c and iron_raylib.h.
- **Smoke file DRAW3D-04 batch-1 tag present** — `grep -cn '^    -- ── DRAW3D-04:'` = 1, `grep -cn 'val strip: \[Vector3\]'` = 1, `grep -cn 'Draw.triangle_strip_3d(strip, Int32(3)'` = 1, `grep -cn 'DRAW3D-04 batch 2 pending plan 69-04'` = 1.
- **No lowercase-receiver stubs** — `grep '^func draw\.' src/stdlib/raylib.iron` returns no matches (static form preserved).

---
*Phase: 69-3d-drawing-camera3d*
*Completed: 2026-04-18*
