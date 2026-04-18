---
phase: 71-shaders
plan: 01
subsystem: stdlib-raylib
tags: [raylib, shaders, glsl, ffi, struct-by-value, iron-list-uint8]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: Iron_Shader 16 B layout pin + ShaderLocationIndex/UniformDataType/AttributeDataType enums
  - phase: 63-2d-drawing
    provides: Template B/F/G shim precedent + Draw.begin_shader_mode binding
  - phase: 65-raymath
    provides: Matrix 64 B struct-by-value ARG precedent
  - phase: 66-textures-images
    provides: string-in NULL-fallback + Texture 20 B ARG + RenderTexture load
  - phase: 68-audio-system
    provides: ABI-UINT8 pattern (Iron_List_uint8_t.items → const void *)
  - phase: 70-models-meshes-materials-animations
    provides: static-form method dispatch precedent
provides:
  - 11 Iron_shader_* C shims (SHADER-01/02/03) under `/* ── Shaders (Phase 71) ── */` marker
  - 11 Shader.* Iron stubs (static-form dispatch) under `-- ── Phase 71: Shaders` banner
  - Empty-string → NULL default-stage fallback for Shader.load / load_from_memory
  - NULL-guarded opaque-pointer `_locs` writer (Iron-side set_location helper, no raylib call)
  - 4th live Iron_List_uint8_t → const void * consumer (after Wave/Mesh/Music load_from_memory)
affects: [71-02-shader-postfx-smoke, 73-polish, plan-phase-scanning]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Empty-string → NULL fallback via `(c && *c) ? c : NULL` — reuse of Phase 66-02 string-in pattern across two arguments simultaneously"
    - "Opaque-pointer field write through `(int *)shader._locs` with NULL guard — first Phase 71 usage of the pattern for user-invoked helpers"
    - "Iron_List_uint8_t as const void * — 4th live consumer of Phase 68-01 ABI-UINT8 path"

key-files:
  created: []
  modified:
    - src/stdlib/raylib.iron
    - src/stdlib/iron_raylib.c
    - src/stdlib/iron_raylib.h

key-decisions:
  - "Empty-string for default shader stage (Iron String `\"\"` → NULL in C) — avoids Iron optional-String FFI round-trip which is unsupported."
  - "Iron-side `Shader.set_location` helper is the only invented API in Phase 71 — raylib exposes no C-side setter; users would otherwise need `shader.locs[i] = loc` which Iron cannot express without a ptr-cast primitive."
  - "Dedicated `set_value_matrix` / `set_value_texture` (not unified through a byte-buffer enum switch) — raylib C signatures diverge, 1:1 mapping stays clearer."
  - "Static-form dispatch throughout (Phase 68-05 / 70-04 precedent) — ironc at build/ironc predates receiver-method support; `func shader.X` would emit E0200."

patterns-established:
  - "Pattern: simultaneous 16 B Shader + 64 B Matrix struct-by-value INPUT (Template F) — zero -Wlarge-by-value-copy under clang -Wall -Wextra"
  - "Pattern: simultaneous 16 B Shader + 20 B Texture struct-by-value INPUT (Template F)"
  - "Pattern: dual iron_string_cstr + NULL-fallback per argument — two simultaneous string-in-NULL arg sites per load entry"

requirements-completed: [SHADER-01, SHADER-02, SHADER-03]

# Metrics
duration: 2m 32s
completed: 2026-04-18
---

# Phase 71 Plan 01: Shaders — SHADER-01/02/03 Summary

**11 Iron Shader.* methods bound via 11 Iron_shader_* C shims under the pre-scaffolded Phase 71 marker — load / valid / unload / get_location / get_location_attrib / set_location / set_value / set_value_v / set_value_matrix / set_value_texture — zero novel ABI, every struct size and direction a proven path (Shader 16 B, Matrix 64 B, Texture 20 B, Iron_List_uint8_t void*, string-in NULL fallback).**

## Performance

- **Duration:** 2m 32s (152 s total)
- **Started:** 2026-04-18T03:50:02Z
- **Completed:** 2026-04-18T03:52:34Z
- **Tasks:** 3 (2 implementation + 1 checkpoint:human-verify auto-approved)
- **Files modified:** 3 (src/stdlib/raylib.iron, src/stdlib/iron_raylib.c, src/stdlib/iron_raylib.h)

## Accomplishments

- **11 Iron_shader_* C shims bound** under `/* ── Shaders (Phase 71) ── */` marker in iron_raylib.{c,h}:
  - **SHADER-01 (4):** Iron_shader_load / _load_from_memory / _is_valid / _unload
  - **SHADER-02 (3):** Iron_shader_get_location / _get_location_attrib / _set_location
  - **SHADER-03 (4):** Iron_shader_set_value / _set_value_v / _set_value_matrix / _set_value_texture
- **11 Iron Shader.* foreign-method stubs** added to raylib.iron under `-- ── Phase 71: Shaders` banner, all static-form dispatch.
- **Empty-string → NULL fallback verified** for both `Shader.load` and `Shader.load_from_memory` — 4 `(vs_c && *vs_c)` / `(fs_c && *fs_c)` match sites in iron_raylib.c.
- **NULL-guarded `_locs` opaque-pointer write** in Iron_shader_set_location — protects against zeroed Shader struct from failed load.
- **4th live Iron_List_uint8_t → const void * consumer** (after Wave.load_from_memory, Mesh.update_buffer, Music.load_from_memory) — ABI-UINT8 pattern remains proven.
- **clang -c iron_raylib.c -Wall -Wextra -Wno-unused-parameter** exits 0 with zero warnings across the full translation unit.
- **Pong regression GREEN** — `./build/ironc build examples/pong/pong.iron` produces 2,741,800 B arm64 Mach-O (+736 B vs. Phase 70-04 baseline 2,741,064 B, well within ±100 KB tolerance).

## Task Commits

Each task was committed atomically and pushed to origin/feat/v2-raylib-milestone immediately.

1. **Task 1: 11 Iron Shader.* stubs** — `33a51a5` (feat)
2. **Task 2: 11 Iron_shader_* C shims + prototypes** — `a9de8ec` (feat)
3. **Task 3: Pong regression + ironc check + shim count** — verification-only (no commit); auto-approved under orchestrator autonomous-mode hint (all three checks GREEN: pong build exit 0 / 2,741,800 B binary, shim count == 11, ironc check exit 0).

**Plan metadata commit:** added in final step.

## Files Created/Modified

- `src/stdlib/raylib.iron` — +30 lines. 11 new `func Shader.*` stubs + Phase 71 banner. Now 2,949 lines (was 2,919).
- `src/stdlib/iron_raylib.c` — +111 lines (Phase 71 shim bodies). Now 6,238 lines (was 6,127).
- `src/stdlib/iron_raylib.h` — +23 lines (11 Phase 71 prototypes under pre-scaffolded marker). Now 1,910 lines (was 1,887).

### Iron_shader_* → raylib entry-point mapping

| Iron method                      | raylib call                 | Template               |
| -------------------------------- | --------------------------- | ---------------------- |
| `Shader.load`                    | `LoadShader`                | G + string-in×2 + NULL |
| `Shader.load_from_memory`        | `LoadShaderFromMemory`      | G + string-in×2 + NULL |
| `Shader.is_valid`                | `IsShaderValid`             | B + bool coerce        |
| `Shader.unload`                  | `UnloadShader`              | B                      |
| `Shader.get_location`            | `GetShaderLocation`         | B + string-in          |
| `Shader.get_location_attrib`     | `GetShaderLocationAttrib`   | B + string-in          |
| `Shader.set_location`            | *(no raylib call)*          | B + opaque-ptr write   |
| `Shader.set_value`               | `SetShaderValue`            | B + ABI-UINT8          |
| `Shader.set_value_v`             | `SetShaderValueV`           | B + ABI-UINT8          |
| `Shader.set_value_matrix`        | `SetShaderValueMatrix`      | F (16 B + 64 B)        |
| `Shader.set_value_texture`       | `SetShaderValueTexture`     | F (16 B + 20 B)        |

## Decisions Made

None beyond those locked in CONTEXT.md / RESEARCH.md. Plan executed exactly as written.

## Deviations from Plan

None - plan executed exactly as written.

No Rule 1-3 auto-fixes needed. Phase 71 Plan 01 is pure ABI binding with zero novel size/direction crossings — every pattern used was established by Phase 60-70.

**Total deviations:** 0
**Impact on plan:** No scope creep. Plan template was structurally complete and executed verbatim.

## Issues Encountered

None.

## User Setup Required

None.

## Next Phase Readiness

- **Plan 71-02 unblocked.** Shim surface (11 Iron_shader_*) is ready for SHADER-04 composition tests.
- Asset paths needed for Plan 71-02:
  - `tests/assets/shaders/default.vs`, `grayscale.fs`, `invert.fs` (new — hand-write recommended per RESEARCH Q2)
  - `tests/assets/models/cube.obj` already vendored (Phase 70-04) — post-FX showcase can reuse
- **`checkpoint:human-verify` auto-approved** under orchestrator autonomous-mode hint because all three verification commands matched expected outputs exactly (pong build exit 0 with binary size within ±100 KB baseline, shim count exactly 11, ironc check exit 0).
- No known blockers for Plan 71-02.

## Handoff Notes to Plan 71-02

- `Shader.load("", "grayscale.fs")` is the documented smoke path for post-FX pipelines (vs_path empty-string → raylib default vertex shader + user fragment shader).
- `set_value` with `.float` requires 4 little-endian bytes (IEEE-754). For smoke coverage of SHADER-03 variants, prefer `set_value_matrix(Matrix.identity())` + `set_value_texture(tex)` (no packing required) first; one `.float` packing path exercises ABI-UINT8.
- `set_location` NULL-guards via `if (locs == NULL) return;` — smoke can safely call on a zeroed / never-loaded Shader without segfault.
- `Camera3D(...)` struct literal with `CameraProjection.PERSPECTIVE.ordinal` is blocked by a pre-existing ironc codegen bug (Phase 69 Rule-3 fix); use `Int32(0)` literal for the fifth argument when building showcases.

---
*Phase: 71-shaders*
*Completed: 2026-04-18*

## Self-Check: PASSED

- Files verified on disk: `.planning/phases/71-shaders/71-01-SUMMARY.md`, `src/stdlib/raylib.iron`, `src/stdlib/iron_raylib.c`, `src/stdlib/iron_raylib.h`.
- Commits verified on origin/feat/v2-raylib-milestone: `33a51a5` (Task 1) / `a9de8ec` (Task 2).
- Grep-verifiable claims re-checked post-summary: `func Shader.` == 11 / `Iron_shader_` proto count == 11 / Phase 71 section marker present in all 3 files.
