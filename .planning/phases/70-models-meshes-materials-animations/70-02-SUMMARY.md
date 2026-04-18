---
phase: 70-models-meshes-materials-animations
plan: 02
subsystem: stdlib-ffi
tags: [raylib, mesh, ffi, struct-by-value, mutating-return, iron-list, matrix, uint8]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Mesh (120 B) / Iron_Material (40 B) / Iron_Matrix (64 B) / Iron_BoundingBox (24 B) / Iron_List_uint8_t layouts pinned via _Static_assert grid"
  - phase: 64-collision-2d-3d
    provides: "120 B Mesh struct-by-value ARG template (Iron_ray_hit_mesh); 120+64 B double struct-input precedent"
  - phase: 65-raymath
    provides: "64 B Matrix struct-by-value ARG/RETURN template"
  - phase: 66-textures-images
    provides: "Mutating-return-by-value shim template (Iron_image_crop)"
  - phase: 68-audio-system
    provides: "ABI-UINT8 closure (Iron_List_uint8_t); Iron_sound_update list-input template"
  - phase: 70-01
    provides: "11 Iron_model_* shims + Scan B auto-emit GREEN for Iron_List_Iron_Matrix / Iron_List_Iron_Material / Iron_List_Iron_ModelAnimation (abi_phase70_probe.iron)"
provides:
  - "9 Iron_mesh_* C shim bodies + 9 prototypes under the Phase 70 marker in iron_raylib.{c,h} (MODEL-04: 7 ops + MODEL-05: 2 draws)"
  - "9 Iron-side Mesh.* foreign-method stubs in raylib.iron"
  - "First simultaneous 120+40+64 = 224 B struct-by-value input validated clean (Iron_mesh_draw — Mesh + Material + Matrix by value)"
  - "First live [Matrix] list input consumer — Iron_mesh_draw_instanced forwards transforms.items as (const Matrix *)"
  - "Second Iron_List_uint8_t consumer in raylib bindings — Iron_mesh_update_buffer (Phase 68-01 ABI-UINT8 path)"
  - "IRON_LIST_IRON_MATRIX_STRUCT_DEFINED-guarded typedef manually declared in iron_raylib.h (matches Iron_List_Iron_Vector2 / Vector3 pattern — required for shim TU compilation before any user-program C file auto-emits via Scan B)"
affects: [70-03-mesh-generation, 70-04-material-animation-billboard, 73-api-polish]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Simultaneous 120+40+64 B struct-by-value triple-input validated clean under -Wall -Wextra (per-struct evaluation, not cumulative — confirms Phase 64-02 ceiling behavior)"
    - "Iron_List_Iron_Matrix live consumer path (Scan B auto-emit + guarded manual typedef in iron_raylib.h) — template for future [Matrix] / [Material] / [ModelAnimation] shim prototypes"
    - "Manual-declaration + #ifndef guard for Scan-B-auto-emitted Iron_List_Iron_<T> typedefs in iron_raylib.h — required because user-program generated C files are separate TUs from iron_raylib.c; same pattern as Iron_List_Iron_Vector2 (line 604-611) / Iron_List_Iron_Vector3 (line 1680-1687)"

key-files:
  created: []
  modified:
    - "src/stdlib/raylib.iron (+24 lines — 9 Mesh.* foreign-method stubs under the Phase 70 banner: MODEL-04 ops + MODEL-05 draws)"
    - "src/stdlib/iron_raylib.c (+88 lines — 9 Iron_mesh_* shim bodies under the /* MODEL-04 */ and /* MODEL-05 */ sub-sections after Plan 70-01 Iron_model_draw_points_ex)"
    - "src/stdlib/iron_raylib.h (+41 lines — guarded Iron_List_Iron_Matrix typedef + 9 Iron_mesh_* prototypes grouped by requirement)"

key-decisions:
  - "Manual IRON_LIST_IRON_MATRIX_STRUCT_DEFINED-guarded typedef added to iron_raylib.h — plan rule #2 (Scan B auto-emission lands Iron_List_Iron_Matrix before the shim prototypes in iron_raylib.h) only holds inside user-program generated C files; iron_raylib.c is a separate static TU that cannot see Scan B output, so the typedef is declared manually using the same #ifndef guard pattern Iron_List_Iron_Vector2 / Vector3 use. Tracked as Rule 3 - Blocking deviation."
  - "Mutating-return-by-value applied to Iron_mesh_upload + Iron_mesh_gen_tangents (Phase 66-03 template verbatim) — Mesh local; memcpy(&local, &mesh, sizeof(Mesh)); raylib mutating call; struct Iron_Mesh out; memcpy(&out, &local, sizeof(struct Iron_Mesh)); return out."
  - "Simultaneous 120+40+64 B struct-by-value input (DrawMesh) ships clean first-try under -Wall -Wextra -Wno-unused-parameter — confirms Phase 64-02's empirical finding that clang's -Wlarge-by-value-copy evaluates per-struct (120 B Mesh each under ceiling), never cumulatively."
  - "Iron_mesh_update_buffer receives Iron_List_uint8_t (ABI-UINT8 closed Phase 68-01) + casts .items as (const void *) for raylib's void * data parameter."
  - "Iron_mesh_draw_instanced forwards transforms.items as (const Matrix *) — Iron_Matrix and raylib Matrix are layout-identical (Phase 60-04 _Static_assert, 64 B each), so the cast is ABI-safe."
  - "Task 3 checkpoint auto-approved under the orchestrator's autonomous-mode hint: probe + pong both built exit 0 with ~2.7 MB arm64 Mach-O binaries and Iron_List_Iron_Matrix still auto-emitted in .iron-build/abi_phase70_probe.c (3 matches)."

patterns-established:
  - "Simultaneous multi-struct-by-value input (120+40+64 B) template: three memcpy-ins + single raylib call — Iron_mesh_draw is the canonical example, reusable for future phases that combine large-struct arguments."
  - "Scan-B-typedef manual mirror in iron_raylib.h: when a shim prototype uses Iron_List_Iron_<T> where T is an IRON_TYPE_OBJECT type, declare the typedef under IRON_LIST_IRON_<T>_STRUCT_DEFINED guard in iron_raylib.h before the prototype (mirror the guard Scan B uses so duplicate definitions in separate TUs are harmless)."

requirements-completed: [MODEL-04, MODEL-05]

# Metrics
duration: ~4 min
completed: 2026-04-18
---

# Phase 70 Plan 02: Mesh operations + mesh draw Summary

**9 Iron_mesh_* shims bound under the Phase 70 marker (7 MODEL-04 ops including mutating-return upload/gen_tangents + 2 MODEL-05 draws); first simultaneous 120+40+64 B struct-by-value triple-input (DrawMesh) ships clean under -Wall -Wextra; first live [Matrix] list consumer wired via Iron_List_Iron_Matrix (Scan B + guarded manual typedef).**

## Performance

- **Duration:** ~4 min (2 atomic code commits + 1 docs commit: 2e1d42d Iron stubs → f06a84a C shims + prototypes + typedef → [docs])
- **Started:** 2026-04-18T02:33:25Z
- **Completed:** 2026-04-18T02:37:07Z
- **Tasks:** 3 (Task 1 Iron stubs; Task 2 C shims + prototypes + typedef; Task 3 end-to-end probe + pong regression — GREEN, auto-approved in autonomous mode)
- **Files modified:** 3 (raylib.iron / iron_raylib.c / iron_raylib.h — no files created)

## Accomplishments

- **9 MODEL-04/05 shims landed.** 7 MODEL-04 mesh operations (upload / update_buffer / unload / export / export_as_code / bounding_box / gen_tangents) + 2 MODEL-05 mesh draws (draw / draw_instanced). Cumulative Phase 70 count: **20/46** shims (11 from Plan 70-01 + 9 from Plan 70-02).
- **First simultaneous 120+40+64 = 224 B struct-by-value triple-input validated clean.** `Iron_mesh_draw(Mesh, Material, Matrix)` compiles under `clang -Wall -Wextra -Wno-unused-parameter` with zero warnings. Confirms Phase 64-02's empirical finding: clang evaluates `-Wlarge-by-value-copy` per-struct (each arg independently under 120 B ceiling), never cumulatively.
- **First live `[Matrix]` list input consumer.** `Iron_mesh_draw_instanced(Mesh, Material, Iron_List_Iron_Matrix, int32_t)` forwards `transforms.items` as `(const Matrix *)` — layout-compatible cast is ABI-safe since `sizeof(Iron_Matrix) == sizeof(Matrix) == 64 B` per Phase 60-04 `_Static_assert`. Plan 70-01's Scan B probe proved the auto-emit; Plan 70-02 proves the live consumer path.
- **Mutating-return-by-value applied twice.** `Iron_mesh_upload(mesh, dynamic) -> Mesh` and `Iron_mesh_gen_tangents(mesh) -> Mesh` follow the Phase 66-03 `Iron_image_crop` template verbatim — memcpy-in 120 B Mesh → raylib in-place mutating call on &local → memcpy-out 120 B struct Iron_Mesh. Iron val-field immutability preserved; users rebind via `var mesh = Mesh.cube(...); mesh = mesh.upload(true)`.
- **Second `Iron_List_uint8_t` consumer in raylib bindings.** `Iron_mesh_update_buffer` forwards `data.items` as `(const void *)` — ABI-UINT8 path closed in Phase 68-01 continues to handle raw-byte FFI uniformly.
- **End-to-end probe + pong regression GREEN.** `./build/ironc build tests/manual/abi_phase70_probe.iron` exits 0 producing ~2.7 MB arm64 Mach-O; `Iron_List_Iron_Matrix` still auto-emitted in `.iron-build/abi_phase70_probe.c` (3 matches). `./build/ironc build examples/pong/pong.iron` exits 0 producing ~2.7 MB binary — Phase 60-69 surface unbroken by Plan 70-02 additions.
- **Plans 70-03 / 70-04 unblocked.** 120 B Mesh ARG + RETURN + triple-input paths validated; `[Matrix]` list consumer path live. Plan 70-03 (11 Mesh.* static constructors, each returning 120 B Mesh by value) inherits the same clean struct-by-value ABI.

## Task Commits

Each task was committed atomically and pushed to origin/feat/v2-raylib-milestone:

1. **Task 1: 9 Iron Mesh stubs** — `2e1d42d` (feat) — `src/stdlib/raylib.iron` +24 lines; 7 MODEL-04 ops + 2 MODEL-05 draws; `ironc check src/stdlib/raylib.iron` exits 0.
2. **Task 2: 9 C shims + 9 prototypes + Iron_List_Iron_Matrix typedef** — `f06a84a` (feat) — `src/stdlib/iron_raylib.c` +88 lines + `src/stdlib/iron_raylib.h` +41 lines; `clang -c -Wall -Wextra -Wno-unused-parameter` exits 0 with zero warnings.
3. **Task 3: end-to-end build verification** — no files modified; probe + pong both built exit 0, binaries present, Iron_List_Iron_Matrix still in generated probe C. **Auto-approved under autonomous-mode hint (no checkpoint pause needed).**

**Plan metadata:** pending (this SUMMARY commit).

## Files Created/Modified

- `src/stdlib/raylib.iron` (modified, +24 lines) — appended Phase 70 Plan 70-02 section: MODEL-04 banner + 7 Mesh.* ops stubs + MODEL-05 banner + 2 Mesh.* draw stubs. Static `Mesh.method(mesh: Mesh, ...)` form throughout; `data: [UInt8]`, `transforms: [Matrix]`, `Int32` for all integer-typed params.
- `src/stdlib/iron_raylib.c` (modified, +88 lines) — appended 9 shim bodies after `Iron_model_draw_points_ex`, organized under `/* MODEL-04: Mesh operations (7) */` and `/* MODEL-05: Mesh draw (2) */` sub-comments. Banner text documents the mutating-return-by-value rationale + 120+40+64 B per-struct evaluation note.
- `src/stdlib/iron_raylib.h` (modified, +41 lines) — inserted `IRON_LIST_IRON_MATRIX_STRUCT_DEFINED`-guarded typedef (lines following `Iron_model_draw_points_ex` prototype), then 9 Iron_mesh_* prototypes grouped by requirement (MODEL-04 / MODEL-05).

## Decisions Made

- **Manual guarded typedef in iron_raylib.h for Iron_List_Iron_Matrix.** The plan's CRITICAL formatting rule #2 ("Iron_List_Iron_Matrix is auto-emitted by Scan B during ironc codegen; in iron_raylib.h it will appear BEFORE these prototypes via the existing Scan B ordering") holds only for user-program generated C files, which auto-emit the typedef inline. `iron_raylib.c` is a separate static translation unit that includes `iron_raylib.h` and cannot see Scan B output. The solution is the established pattern used for Iron_List_Iron_Vector2 (iron_raylib.h:604-611) and Iron_List_Iron_Vector3 (iron_raylib.h:1680-1687): declare the typedef manually with an `IRON_LIST_IRON_<T>_STRUCT_DEFINED` guard. Tracked as Rule 3 - Blocking deviation (required for shim TU compilation). Captured as a new pattern for the summary.
- **Checkpoint Task 3 auto-approved under autonomous mode.** The orchestrator prompt stated: "if the probe builds cleanly with the expected outputs listed in the plan, mark it GREEN and proceed without pausing." Both builds exited 0, both binaries produced at expected sizes (~2.7 MB arm64 Mach-O), Iron_List_Iron_Matrix still emitted in probe generated C. No real problem surfaced; proceeded without pausing.
- **No per-struct suppressions needed.** The 120+40+64 B simultaneous input path compiled clean under `-Wall -Wextra -Wno-unused-parameter`. No `-Wno-large-by-value-copy` scope disabling. Phase 64-02's per-struct evaluation finding holds at Phase 70 scale.
- **Every raylib suffix preserved.** `export` / `export_as_code` both kept (raylib has `ExportMesh` + `ExportMeshAsCode`); `draw` / `draw_instanced` both kept (not merged into a single polymorphic call). Matches Phase 63/69 variant-suffix precedent.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Added manual IRON_LIST_IRON_MATRIX_STRUCT_DEFINED-guarded typedef to iron_raylib.h**
- **Found during:** Task 2 (C shims + prototypes).
- **Issue:** Plan rule #2 says Scan B auto-emits `Iron_List_Iron_Matrix` before the Mesh prototypes in iron_raylib.h via the existing Scan B ordering. That only holds for user-program generated C files (`.iron-build/*.c`). `src/stdlib/iron_raylib.c` is a separate static TU that `#include`s `src/stdlib/iron_raylib.h` — Scan B does not modify either of these files. Without a typedef in iron_raylib.h, `Iron_mesh_draw_instanced`'s prototype using `Iron_List_Iron_Matrix` as a parameter type would fail to parse when compiling `iron_raylib.c`.
- **Fix:** Added guarded typedef block to `iron_raylib.h` between `Iron_model_draw_points_ex` prototype and the Mesh prototype group, following the same pattern as the existing `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` (line 604-611) and `IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED` (line 1680-1687) blocks. The `#ifndef` guard matches the macro name Scan B uses in auto-emitted user-program C, so duplicate definitions across TUs are harmless.
- **Files modified:** `src/stdlib/iron_raylib.h` (+13 lines including comment + guard block + typedef).
- **Verification:** `clang -c src/stdlib/iron_raylib.c -I src/stdlib -I src/vendor/raylib -I src -Wall -Wextra -Wno-unused-parameter -o /dev/null` exits 0. `./build/ironc build tests/manual/abi_phase70_probe.iron` exits 0 — the probe's generated C re-declares the typedef via Scan B, guard prevents redefinition.
- **Committed in:** `f06a84a` (Task 2 commit).

---

**Total deviations:** 1 auto-fixed (1 blocking).
**Impact on plan:** Necessary for shim TU compilation; no scope creep. The fix is a direct mirror of the existing Iron_List_Iron_Vector2 / Vector3 manual-declaration pattern. Establishes a new explicit pattern for all future phases using Scan B-emitted list types as shim prototype parameters (patterns-established[1]).

## Issues Encountered

None.

## Authentication Gates

None — binding work is offline and requires no external service authentication.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- **Plan 70-03 unblocked.** 11 Mesh.* static constructors (MODEL-06 — poly / plane / cube / sphere / hemi_sphere / cylinder / cone / torus / knot / heightmap / cubicmap) all return 120 B Mesh by value. Plan 70-01's 120 B Model RETURN and Plan 70-02's 120 B Mesh ARG + triple-input both validated clean — strong GREEN prior for Plan 70-03's single Mesh.cube probe.
- **Plan 70-04 unblocked.** `Iron_List_Iron_Material` and `Iron_List_Iron_ModelAnimation` probe-emitted in Plan 70-01; the manual-guarded-typedef pattern established in Plan 70-02 makes declaring their iron_raylib.h mirrors a mechanical copy-paste of the Iron_List_Iron_Matrix block.
- **Phase 70 at 20/46 shims (43%).** Two more plans close the remaining 26 shims (11 in 70-03 mesh gen + 15 in 70-04 Material/Animation/Billboard/BBox + smoke + showcase).
- **Memory budget:** 2 ironc builds consumed (probe + pong); no ironc invocations beyond budget.

---
*Phase: 70-models-meshes-materials-animations*
*Completed: 2026-04-18*

## Self-Check: PASSED

Verified before finalization:

- [x] `src/stdlib/raylib.iron` contains 9 `func Mesh.*` stubs (`grep -cE '^func Mesh\.(upload|update_buffer|unload|export|export_as_code|bounding_box|gen_tangents|draw|draw_instanced)\('` returns 9)
- [x] `src/stdlib/iron_raylib.c` contains 9 Iron_mesh_* shim-body symbols (2 `^struct Iron_Mesh Iron_mesh_` + 2 `^bool Iron_mesh_export` + 4 `^void Iron_mesh_(draw|unload|update_buffer|draw_instanced)` + 1 `^struct Iron_BoundingBox Iron_mesh_bounding_box`)
- [x] `src/stdlib/iron_raylib.h` contains 9 Iron_mesh_* prototypes + the `IRON_LIST_IRON_MATRIX_STRUCT_DEFINED` guarded typedef block
- [x] `UploadMesh(&local, dynamic)` present exactly once in iron_raylib.c
- [x] `GenMeshTangents(&local)` present exactly once in iron_raylib.c
- [x] `DrawMesh(rm, rmat, rt)` present exactly once in iron_raylib.c
- [x] `DrawMeshInstanced(rm, rmat, (const Matrix *)transforms.items, (int)instances)` present exactly once in iron_raylib.c
- [x] `clang -c src/stdlib/iron_raylib.c -I src/stdlib -I src/vendor/raylib -I src -Wall -Wextra -Wno-unused-parameter -o /dev/null` exits 0 with zero warnings
- [x] `./build/ironc check src/stdlib/raylib.iron` exits 0
- [x] `./build/ironc build tests/manual/abi_phase70_probe.iron` exits 0; `./abi_phase70_probe` produced
- [x] `./build/ironc build examples/pong/pong.iron` exits 0; `./pong` produced (regression guard)
- [x] `.iron-build/abi_phase70_probe.c` still contains `Iron_List_Iron_Matrix` (3 matches) — Scan B auto-emit unaffected by the new manual typedef in iron_raylib.h
- [x] 2 code commits on disk + pushed to origin/feat/v2-raylib-milestone: `2e1d42d` feat(70-02) Iron stubs, `f06a84a` feat(70-02) C shims + prototypes + typedef
