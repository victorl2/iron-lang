---
phase: 70-models-meshes-materials-animations
plan: 01
subsystem: stdlib-ffi
tags: [raylib, model, ffi, struct-by-value, scan-b, iron-list]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Model (120 B) / Iron_BoundingBox (24 B) / Iron_Mesh (120 B) / Iron_Vector3 (12 B) / Iron_Color (4 B) layouts pinned via _Static_assert grid"
  - phase: 63-2d-drawing
    provides: "PLAN_63_04_EMIT_LIST_FOR Scan B auto-emit path in src/lir/emit_structs.c:252-340 — lands Iron_List_<T> typedefs for foreign-method stub signatures"
  - phase: 64-collision-2d-3d
    provides: "120 B Mesh pass-by-value ARG template + 32 B RayCollision struct-by-value RETURN template + Rectangle/BoundingBox instance-method dispatch"
  - phase: 65-raymath
    provides: "64 B Matrix struct-by-value RETURN template (Phase 65-03 first-try GREEN under -Wall -Wextra)"
  - phase: 66-textures-images
    provides: "Iron_String + struct-return memcpy-out template (Image.load)"
  - phase: 68-audio-system
    provides: "Sound.load Iron_String + struct-return template; raylib-internal Scan B extension precedent for ABI-FLOAT32/ABI-UINT8"
  - phase: 69-3d-drawing-camera3d
    provides: "Draw.begin_mode_3d / end_mode_3d host for model.draw*; Camera3D orbital pattern for model viewer"
provides:
  - "11 Iron_model_* C shim bodies + 11 prototypes under the pre-scaffolded /* ── Models (Phase 70) ── */ marker at iron_raylib.{c:5560,h:1739}"
  - "First 120 B struct-by-value RETURN direction in codebase (Iron_model_load / _from_mesh / _bounding_box returns Iron_Model / Iron_BoundingBox by value — Phase 64-02 validated ARG direction; zero -Wlarge-by-value-copy warnings confirms Risk 2 GREEN prediction)"
  - "First 24 B BoundingBox struct-by-value RETURN (Iron_model_bounding_box — generalizes Phase 61/63 Vector2 / Phase 64-02 RayCollision return template to a new type)"
  - "11 Iron-side Model.* foreign-method stubs in raylib.iron under the Phase 70: Models / Meshes / Materials / Animations banner"
  - "Scan B auto-emit verified GREEN for Iron_List_Iron_Matrix / Iron_List_Iron_Material / Iron_List_Iron_ModelAnimation typedefs via tests/manual/abi_phase70_probe.iron — unblocks Plan 70-02 ([Matrix] input) and Plan 70-04 ([Material] / [ModelAnimation] returns)"
affects: [70-02-mesh-operations, 70-03-mesh-generation, 70-04-material-animation-billboard, 73-api-polish]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "120 B Model struct-by-value RETURN via memcpy-out (first in codebase — generalizes Phase 65-03 64 B Matrix RETURN)"
    - "24 B BoundingBox struct-by-value RETURN via memcpy-out (new type crossing FFI)"
    - "Pre-flight Scan B auto-emit probe (Phase 68-01 precedent): throwaway foreign-method stubs force Iron_List_<T> typedef emission before real shims depend on them"

key-files:
  created:
    - "tests/manual/abi_phase70_probe.iron (probe file — 3 throwaway foreign-method stubs forcing Iron_List_Iron_Matrix / Material / ModelAnimation emission)"
  modified:
    - "src/stdlib/iron_raylib.c (+137 lines — Phase 70 banner + 11 shim bodies under the line-5560 marker)"
    - "src/stdlib/iron_raylib.h (+29 lines — 11 prototypes under the line-1739 marker)"
    - "src/stdlib/raylib.iron (+42 lines — Phase 70 banner + 11 Model.* foreign-method stubs)"

key-decisions:
  - "Scan B auto-emit GREEN — no ironc extension needed for Matrix / Material / ModelAnimation element types; the Phase 68-01 RED-path extension template is available if future phases surface new IRON_TYPE_OBJECT element types that fail to emit"
  - "Static Type.method(receiver: Type, ...) form for all 11 Iron stubs (Phase 68-05 / 69-04 precedent) — Iron reserves 'self' (E0101) so receiver param uses semantic name 'model'"
  - "120 B Model RETURN direction ships clean on first try — validates Risk 2 GREEN prediction (Phase 64-02 ARG direction + Phase 65-03 64 B Matrix RETURN combined prior held)"
  - "Phase 70 section markers in iron_raylib.{c,h} and raylib.iron preserved as single-line banners — downstream Plans 70-02/03/04 append under the same markers"

patterns-established:
  - "Pre-flight Scan B probe: before any real list-carrying shim lands in later plans, probe Iron_List_<T> emission via a throwaway foreign-method stub. GREEN result = proceed; RED = Phase 68-01-style extension in emit_structs.c before bulk shims"
  - "120 B struct-by-value RETURN template: Iron_<Type> out; memcpy(&out, &rl_ret, sizeof(struct Iron_<Type>)); return out; — direct generalization of 64 B Matrix + 40 B Image + 120 B Mesh ARG templates"

requirements-completed: [MODEL-01, MODEL-02, MODEL-03]

# Metrics
duration: ~4 min
completed: 2026-04-17
---

# Phase 70 Plan 01: Model load/unload/bbox + draw variants + Scan B auto-emit probe Summary

**11 Iron_model_* shims landing MODEL-01/02/03 (Model load/from_mesh/is_valid/unload + bounding_box + 6 draw variants) + Scan B auto-emit probe GREEN — 120 B Model struct-by-value RETURN direction validated clean first-try under -Wall -Wextra.**

## Performance

- **Duration:** ~4 min (3 atomic commits: 3d05f18 probe → 5860925 Iron stubs → b86e199 C shims+prototypes)
- **Started:** 2026-04-17T23:14:05Z (probe commit)
- **Completed:** 2026-04-17T23:16:22Z (C shims commit)
- **Tasks:** 3 (Task 1 probe — checkpoint:human-verify GREEN; Task 2 Iron stubs; Task 3 C shims+prototypes)
- **Files modified:** 4 (tests/manual/abi_phase70_probe.iron created; iron_raylib.c / iron_raylib.h / raylib.iron extended)

## Accomplishments

- **Scan B auto-emit GREEN** — `tests/manual/abi_phase70_probe.iron` compiles clean via `./build/ironc build`; generated C at `.iron-build/abi_phase70_probe.c` contains all 3 expected typedefs (Iron_List_Iron_Matrix × 3 matches, Iron_List_Iron_Material × 3 matches, Iron_List_Iron_ModelAnimation × 3 matches — 9 total) plus IRON_LIST_DECL / IRON_LIST_IMPL triples. PLAN_63_04_EMIT_LIST_FOR macro at `src/lir/emit_structs.c:252-340` handles IRON_TYPE_OBJECT element types uniformly; no Phase 68-01 style extension needed.
- **11 Iron-side Model.* foreign-method stubs** landed in `src/stdlib/raylib.iron` under a new Phase 70 banner: 4 lifecycle (Model.load/from_mesh/is_valid/unload) + 1 bounding box + 6 draw variants (draw / draw_ex / draw_wires / draw_wires_ex / draw_points / draw_points_ex — every raylib suffix preserved).
- **11 C shim bodies + 11 prototypes** landed in `src/stdlib/iron_raylib.c` (+137 lines) and `src/stdlib/iron_raylib.h` (+29 lines) under the pre-scaffolded `/* ── Models (Phase 70) ── */` markers at c:5560 / h:1739.
- **120 B Model struct-by-value RETURN direction validated** (first in codebase) — `Iron_model_load` and `Iron_model_from_mesh` return `struct Iron_Model` by value via memcpy-out template; clang -c with -Wall -Wextra -Wno-unused-parameter exits 0 with zero warnings. Confirms Risk 2 GREEN prediction from RESEARCH.md.
- **24 B BoundingBox struct-by-value RETURN validated** via `Iron_model_bounding_box` — new type through the FFI crossing; zero warnings.
- **Plans 70-02/03/04 unblocked** — Scan B auto-emit GREEN means [Matrix] input (Plan 70-02 mesh.draw_instanced) and [Material] / [ModelAnimation] list returns (Plan 70-04) can proceed without ironc compiler changes.
- **11/46 Phase 70 shims bound** (MODEL-01: 4 + MODEL-02: 1 + MODEL-03: 6 = 11).

## Task Commits

Each task was committed atomically:

1. **Task 1: Scan B auto-emit probe** — `3d05f18` (test) — `tests/manual/abi_phase70_probe.iron` + `.gitignore` entries; ironc build GREEN; generated C contains all 3 expected typedefs.
2. **Task 2: 11 Iron stubs** — `5860925` (feat) — `src/stdlib/raylib.iron` +42 lines; Phase 70 section banner + 11 Model.* stubs; ironc check GREEN.
3. **Task 3: 11 C shims + 11 prototypes** — `b86e199` (feat) — `src/stdlib/iron_raylib.c` +137 lines + `src/stdlib/iron_raylib.h` +29 lines; clang -c -Wall -Wextra GREEN with zero warnings.

**Plan metadata:** pending (this SUMMARY commit)

## Files Created/Modified

- `tests/manual/abi_phase70_probe.iron` (created) — 27 lines; 3 throwaway `_Phase70Probe.*` foreign-method stubs with `[Matrix]` / `[Material]` / `[ModelAnimation]` signatures forcing Scan B emission.
- `src/stdlib/iron_raylib.c` (modified) — +137 lines under the `/* ── Models (Phase 70) ── */` marker at line 5560. Contents: Phase 70 banner comment (templates used + memory-discipline statement) + 11 shim bodies (Iron_model_load, _from_mesh, _is_valid, _unload, _bounding_box, _draw, _draw_ex, _draw_wires, _draw_wires_ex, _draw_points, _draw_points_ex).
- `src/stdlib/iron_raylib.h` (modified) — +29 lines under the `/* ── Models (Phase 70) ── */` marker at line 1739. Contents: 11 Iron_model_* prototypes grouped by requirement (MODEL-01 / MODEL-02 / MODEL-03).
- `src/stdlib/raylib.iron` (modified) — +42 lines at end-of-file. Contents: Phase 70 section banner (Models / Meshes / Materials / Animations with ABI statement + Material size correction) + 11 Model.* foreign-method stubs grouped by requirement.

## Decisions Made

- **Scan B GREEN — no extension needed.** Pre-flight probe confirmed the `IRON_TYPE_OBJECT` dispatch path in `emit_structs.c:252-340` handles Matrix / Material / ModelAnimation element types uniformly without source changes. The Phase 68-01 RED-path extension template remains available as precedent if future phases surface novel IRON_TYPE_OBJECT element types that fail to emit.
- **Static `Type.method(receiver: Type, ...)` form** for all 11 Iron stubs. Matches Phase 68-05 / 69-04 pattern. Iron reserves `self` (E0101) so receiver param uses semantic name `model`.
- **Parameter names snake_case verbatim from raylib C** (`file_name`, `rotation_axis`, `rotation_angle`). Float32 everywhere for raylib `float` (Pitfall 3).
- **120 B Model RETURN direction validated clean first try** — no fallback to out-param + memcpy-out needed. The Phase 64-02 ARG-direction validation at 120 B + Phase 65-03 RETURN-direction validation at 64 B Matrix combined to a strong GREEN prior; the empirical result confirms Risk 2 prediction.
- **Phase 70 markers preserved as single-line banners** in both iron_raylib.c and iron_raylib.h. Downstream Plans 70-02/03/04 append under the same markers.

## Deviations from Plan

None - plan executed exactly as written.

---

**Total deviations:** 0
**Impact on plan:** Plan executed mechanically first try across all 3 tasks. No Rule 1-3 auto-fixes triggered, no Rule 4 architectural decisions needed. Scan B GREEN on the first probe means Plans 70-02 / 70-03 / 70-04 inherit a fully proven auto-emit path with no ironc-extension prerequisite.

## Issues Encountered

None.

## Authentication Gates

None — binding work is offline and requires no external service authentication.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- **Plan 70-02 unblocked** — `[Matrix]` input for `mesh.draw_instanced` proven to auto-emit via `Iron_List_Iron_Matrix`; Plan 70-02 can proceed with 9 mesh operation + mesh draw shims (MODEL-04 + MODEL-05) including mutating-return-by-value `mesh.upload` / `gen_tangents` and the first 120+40+64 B simultaneous struct-input (DrawMesh takes Mesh + Material + Matrix).
- **Plan 70-03 unblocked** — Mesh generation can proceed with 11 `Mesh.<shape>` static constructors (MODEL-06); the 120 B Model RETURN clean result is a strong prior for the 120 B Mesh RETURN direction that Plan 70-03 will probe.
- **Plan 70-04 unblocked** — `[Material]` and `[ModelAnimation]` list returns confirmed emissible. Material + Animation + Billboard + BoundingBox draw (MODEL-07..10) + smoke + showcase can proceed.
- **Phase 70 at 11/46 shims (24%)** — three more plans close the remaining 35 shims.

---
*Phase: 70-models-meshes-materials-animations*
*Completed: 2026-04-17*

## Self-Check: PASSED

Verified before finalization:

- [x] `tests/manual/abi_phase70_probe.iron` exists (27 lines, 3 foreign-method stubs)
- [x] `src/stdlib/iron_raylib.c` contains 11 Iron_model_* symbols (`grep -c '^struct Iron_Model Iron_model_\|^bool Iron_model_\|^void Iron_model_\|^struct Iron_BoundingBox Iron_model_'` returns 11)
- [x] `src/stdlib/iron_raylib.h` contains 11 Iron_model_* references (`grep -c 'Iron_model_'` returns 11)
- [x] `src/stdlib/raylib.iron` contains 11 Model.* stubs (`grep -cE '^func Model\.(load|from_mesh|is_valid|unload|bounding_box|draw|draw_ex|draw_wires|draw_wires_ex|draw_points|draw_points_ex)\('` returns 11)
- [x] Phase 70 markers preserved in both iron_raylib.c (line 5560) and iron_raylib.h (line 1739)
- [x] 3 code commits present on disk: `3d05f18` test(70-01), `5860925` feat(70-01) Iron stubs, `b86e199` feat(70-01) C shims + prototypes
- [x] Generated probe C at `.iron-build/abi_phase70_probe.c` contains all 3 expected Iron_List_Iron_<type> typedefs (9 total matches)
- [x] Probe binary `./abi_phase70_probe` exists (2.7 MB arm64 Mach-O)
