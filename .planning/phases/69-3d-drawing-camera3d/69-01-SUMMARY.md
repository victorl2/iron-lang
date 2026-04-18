---
phase: 69-3d-drawing-camera3d
plan: 01
subsystem: 3d-rendering
tags: [raylib, camera3d, 3d-drawing, ffi-shim, mutating-return-by-value]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: Camera3D struct layout (44 B pinned) + CameraMode enum ordinals
  - phase: 63-2d-drawing
    provides: Draw.begin_mode_2d / Draw.end_mode_2d template + memcpy struct-in shim pattern
  - phase: 66-textures-images
    provides: Mutating-return-by-value template (Iron_image_crop at iron_raylib.c:3121-3130)
  - phase: 68-audio-system
    provides: Static-form receiver convention (Namespace.method(receiver: T, ...))
provides:
  - Draw.begin_mode_3d(camera) / Draw.end_mode_3d() — raylib 3D draw-mode stack bound
  - Camera3D.update(camera, mode) — raylib UpdateCamera bound via mutating-return-by-value
  - Camera3D.update_pro(camera, movement, rotation, zoom) — raylib UpdateCameraPro bound
  - Iron_draw_begin_mode_3d / Iron_draw_end_mode_3d / Iron_camera3d_update / Iron_camera3d_update_pro C shims
  - First Camera3D struct-by-value INPUT (44 B) validated across the FFI
  - First mutating-return-by-value on Camera3D (generalizes Phase 66-03 from Image to Camera3D)
affects: [69-02, 69-03, 69-04]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Camera3D struct-by-value INPUT (44 B) via memcpy into raylib-typed local"
    - "Mutating-return-by-value on 44 B Camera3D (Phase 66-03 Image template generalized)"
    - "Typed CameraMode enum at Iron call site; (int)mode cast at FFI boundary"
    - "Static Namespace.method(receiver: T, ...) form for Camera3D (matches Image.*/Wave.*/Music.*)"

key-files:
  created: []
  modified:
    - src/stdlib/raylib.iron (Phase 69 banner + 4 stubs under 3D Drawing section)
    - src/stdlib/iron_raylib.c (4 shim bodies between Phase 69 and Phase 70 markers at line 5211)
    - src/stdlib/iron_raylib.h (4 prototypes between Phase 69 and Phase 70 markers at line 1649)

key-decisions:
  - "Parameter typed Camera3D (never Camera) — Iron inverts raylib's typedef Camera3D Camera; Iron Camera is 2D (16 B), Iron Camera3D is 3D (44 B). Pitfall 1 locked."
  - "Camera3D.update uses mutating-return-by-value (Phase 66-03 Iron_image_crop template) because Iron val fields are immutable — user rebinds val cam = cam.update(.free)."
  - "CAMERA_CUSTOM (mode = 0) is a no-op in raylib's UpdateCamera; binding preserves the shape, inline comment documents the no-op so users in .custom mode mutate fields directly via val re-binding."
  - "Static form Camera3D.update(camera: Camera3D, ...) — lowercase receivers (func camera3d.update) trigger E0200 (Plan 68-05 Rule-3 Blocking precedent)."
  - "sizeof(Camera3D) used explicitly in shim bodies for Pitfall 1 readability — avoids ambiguity with raylib's typedef alias in a mixed-namespace file."

patterns-established:
  - "Pattern: Camera3D pass-by-value INPUT via memcpy — shim copies into raylib-typed local, calls raylib, returns void."
  - "Pattern: Camera3D mutating-return-by-value — memcpy input into local, raylib mutates via out-pointer, memcpy local into Iron-typed out, return out."
  - "Pattern: Typed CameraMode enum lowered to int32_t, shim casts (int)mode at FFI boundary."

requirements-completed: [DRAW3D-01, DRAW3D-02]

# Metrics
duration: 2 min
completed: 2026-04-18
---

# Phase 69 Plan 01: Draw.begin/end_mode_3d + Camera3D.update / update_pro Summary

**4 Iron shims bound for raylib's 3D draw-mode stack (BeginMode3D / EndMode3D) and camera update functions (UpdateCamera / UpdateCameraPro) using the Phase 66-03 mutating-return-by-value template — first Camera3D struct-by-value INPUT (44 B) across the FFI.**

## Performance

- **Duration:** 2 min 22 sec
- **Started:** 2026-04-18T00:40:36Z
- **Completed:** 2026-04-18T00:42:58Z
- **Tasks:** 3
- **Files modified:** 3

## Accomplishments

- **DRAW3D-01 fully bound.** `Draw.begin_mode_3d(camera)` + `Draw.end_mode_3d()` parallel Phase 63's `Draw.begin_mode_2d` / `Draw.end_mode_2d` verbatim; `Iron_draw_begin_mode_3d` is the first shim to cross Camera3D (44 B) pass-by-value INPUT, validating the Phase 60-04 `_Static_assert` grid pin.
- **DRAW3D-02 fully bound.** `Camera3D.update(camera, mode)` + `Camera3D.update_pro(camera, movement, rotation, zoom)` use the Phase 66-03 `Iron_image_crop` mutating-return-by-value template generalized to Camera3D: memcpy input into raylib-typed local, raylib mutates via out-pointer, memcpy local into Iron-typed out, return by value. `CameraMode` typed enum at the Iron call site lowers to `int32_t` at FFI with `(int)mode` cast per Phase 62/63 template.
- **Phase 69 section banner landed** in all three files (`raylib.iron` line 2625, `iron_raylib.c` line 5211, `iron_raylib.h` line 1649) — Plans 69-02/03/04 can extend directly under the markers.
- **Smoke verification passed on first try.** `clang -c iron_raylib.c -I src/stdlib -I src/vendor/raylib -I src -Wall -Wextra -Wno-unused-parameter` exits 0 with zero new warnings; `./build/ironc check src/stdlib/raylib.iron` exits 0 (no parse or type errors).

## Task Commits

Each task was committed atomically:

1. **Task 1: Phase 69 banner + DRAW3D-01 + DRAW3D-02 Iron stubs in raylib.iron** — `e3a9297` (feat)
2. **Task 2: 4 Phase 69 C shim bodies in iron_raylib.c** — `9f64a42` (feat)
3. **Task 3: 4 Phase 69 prototypes in iron_raylib.h + clang smoke + ironc check** — `e7d77a5` (feat)

**Plan metadata:** pending (docs commit after SUMMARY.md + STATE.md + ROADMAP.md + REQUIREMENTS.md)

## Files Created/Modified

- `src/stdlib/raylib.iron` — appended 60-line Phase 69 section: banner with 3D-binding overview + Pitfalls 0/1/2/3 inline, `Draw.begin_mode_3d(camera: Camera3D) {}`, `Draw.end_mode_3d() {}`, `Camera3D.update(camera: Camera3D, mode: CameraMode) -> Camera3D {}`, `Camera3D.update_pro(camera: Camera3D, movement: Vector3, rotation: Vector3, zoom: Float32) -> Camera3D {}` (all static form per Pitfall 0).
- `src/stdlib/iron_raylib.c` — inserted 49-line block between Phase 69 and Phase 70 markers (line 5211): `Iron_draw_begin_mode_3d` (Template B memcpy-in), `Iron_draw_end_mode_3d` (Template A no-arg void), `Iron_camera3d_update` (Phase 66-03 mutating-return-by-value template — 8 lines), `Iron_camera3d_update_pro` (same template + two Vector3 locals + native `float zoom`).
- `src/stdlib/iron_raylib.h` — inserted 12-line block between Phase 69 and Phase 70 markers (line 1649): 4 prototypes using `struct Iron_Camera3D` / `struct Iron_Vector3` prefix convention matching Phase 63 header style.

## Decisions Made

- **Pitfall 1 locked.** Parameter type is `Camera3D` everywhere (Iron-side `Camera3D`, C shim `struct Iron_Camera3D`, raylib-typed local `Camera3D`). Iron inverts raylib's `typedef Camera3D Camera` — writing `Camera` would have picked up Iron's 2D camera (16 B) and silently broken the 44-byte layout. `sizeof(Camera3D)` used explicitly in shim bodies for readability in a mixed-namespace file.
- **Mutating-return-by-value over in-place mutation.** Iron's `val`-declared Camera3D fields are immutable; raylib's `UpdateCamera` mutates via out-pointer. The shim does memcpy-in → raylib mutates local → memcpy-out → return by value. User rebinds: `val cam = cam.update(.free)`. Direct generalization of Phase 66-03 `Iron_image_crop` to the 44-byte Camera3D.
- **Static form preemptively (Pitfall 0).** Plan 68-05 discovered that lowercase-receiver stubs (`func camera3d.update(...)`) trigger E0200. Every Phase 69 method uses `Namespace.method(receiver: T, ...)` static form from the first stub — matches Image.* / Wave.* / Sound.* / Music.* / AudioStream.* precedent. ironc `check` confirms typecheck on first try.
- **`(int)mode` cast, not `(int32_t)mode`.** Matches Phase 62/63 template. raylib's `UpdateCamera` takes C `int`; Iron-side `CameraMode` lowers to `int32_t`, so one explicit cast at the boundary.
- **CAMERA_CUSTOM no-op documented inline.** raylib's `UpdateCamera` does nothing when `mode == 0`; the returned Camera3D is bit-identical to the input. Binding preserves the shape (users in `.custom` mode mutate via val re-binding directly and skip this call); inline comment above both the Iron stub and the C shim captures the behavior.

## Deviations from Plan

None - plan executed exactly as written.

Every acceptance criterion across all 3 tasks passed on the first attempt. Zero auto-fix invocations. Zero Rule-4 architectural escalations.

## Issues Encountered

None.

## Smoke Budget

- **ironc invocations: 1 of 2 budgeted.** Task 3 ran `./build/ironc check src/stdlib/raylib.iron` — exit 0, no output. 1 invocation remaining for downstream plans in this phase if needed, though 69-02..04 will each have their own budget.
- **clang smoke: 1 invocation.** Task 3 ran `clang -c iron_raylib.c -I src/stdlib -I src/vendor/raylib -I src -Wall -Wextra -Wno-unused-parameter -o /dev/null` — exit 0, zero warnings.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- **DRAW3D-01 + DRAW3D-02 live.** Plans 69-02 (DRAW3D-03: screen↔world + camera matrix) and 69-03/04 (DRAW3D-04: 21 3D primitive draws) can extend directly under the Phase 69 markers in all three files.
- **Camera3D struct-by-value INPUT validated.** 44 B under Phase 64's 120 B `-Wlarge-by-value-copy` ceiling. Downstream 69-02 Ray-by-value RETURN (24 B) + `camera.matrix()` Matrix-64 B RETURN reuse the same memcpy template.
- **Mutating-return-by-value template generalized.** Works for Wave (Plan 68-02), Image (Plan 66-03), and now Camera3D (Plan 69-01). Any future raylib "mutate via out-pointer" function slots into the same shape with a type rename.
- **Phase 69 section banner is the anchor.** Every Phase 69-02/03/04 insertion point is the `/* ── 3D Drawing (Phase 69) ── */` marker in iron_raylib.c (line 5211) / iron_raylib.h (line 1649) and the `-- ══ 3D Drawing (Phase 69) ══` banner in raylib.iron (line 2625).

## Self-Check: PASSED

- **raylib.iron Phase 69 section present** — banner at line 2625, 4 stubs at lines 2665-2681. ✓
- **iron_raylib.c shim bodies present** — 4 functions at lines 5217-5260 between Phase 69 and Phase 70 markers. ✓
- **iron_raylib.h prototypes present** — 4 declarations at lines 1652-1660 between Phase 69 and Phase 70 markers. ✓
- **clang -c iron_raylib.c exit 0, zero warnings.** ✓
- **./build/ironc check raylib.iron exit 0.** ✓
- **Commits exist in git log:** e3a9297 (Task 1), 9f64a42 (Task 2), e7d77a5 (Task 3). ✓
- **Phase 70/71/72 markers preserved** on their own lines in both iron_raylib.c and iron_raylib.h. ✓
- **Zero lowercase-receiver stubs** — grep `^func (camera3d|draw)\.` returns no matches. ✓

---
*Phase: 69-3d-drawing-camera3d*
*Completed: 2026-04-18*
