---
phase: 63-2d-drawing
plan: 01
subsystem: graphics
tags: [raylib, drawing, draw-mode, color, camera2d, rendertexture, shader, blendmode, scissor, ffi, struct-by-value]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Color / Iron_Camera / Iron_RenderTexture / Iron_Shader C mirrors with _Static_assert size + offset grid; BlendMode enum (21 total enums anchored); Draw namespace object declaration (Plan 60-06)"
  - phase: 62-input-keyboard-mouse-gamepad-touch-gestures
    provides: "shim-only foreign-method pattern (emit_c auto-generates extern C prototypes per commit e3e5eee); snake_case Iron_<namespace>_<name> mangling; FFI typed-enum -> int32_t lowering"
provides:
  - "Draw.begin / Draw.end frame bracket (DRAW2D-01)"
  - "Draw.clear(Color) struct-by-value clear (DRAW2D-02)"
  - "Draw.begin_mode_2d(Camera) / end_mode_2d Camera2D stack (DRAW2D-03)"
  - "Draw.begin_texture_mode(RenderTexture) / end_texture_mode framebuffer redirect (DRAW2D-04)"
  - "Draw.begin_shader_mode(Shader) / end_shader_mode GLSL stack (DRAW2D-05)"
  - "Draw.begin_blend_mode(BlendMode) / end_blend_mode typed-enum blend stack (DRAW2D-05)"
  - "Draw.begin_scissor_mode(x,y,w,h) / end_scissor_mode 4-scalar clip rect (DRAW2D-06)"
  - "4 validated struct-by-value INPUT ABI surfaces (Color, Camera2D, RenderTexture, Shader) — memcpy pattern proven clean"
  - "Draw.end() keyword-collision question answered: 'end' is not reserved, singular name legal"
affects: [63-02-shapes, 63-03-text-primitives, 63-04-pong-restore, 66-textures, 71-shaders]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Phase 61/62 shim-only foreign-method pattern extended to Draw namespace (13 functions)"
    - "Struct-by-value INPUT ABI via memcpy(&rl, &iron_in, sizeof(raylib_type)) — safe for Color, Camera2D, RenderTexture, Shader"
    - "Typed Iron enum -> int32_t FFI param -> (int) cast to raylib — same pattern as Phase 62 KeyboardKey/MouseButton"
    - "4-scalar Int32 wrapper: each scalar cast to (int) inside shim body"

key-files:
  created: []
  modified:
    - "src/stdlib/iron_raylib.h"
    - "src/stdlib/iron_raylib.c"
    - "src/stdlib/raylib.iron"

key-decisions:
  - "Draw.end() is legal — ironc accepts func _Probe.end() without parse/reserved-word errors; 'end' is not in the Iron keyword set (grep for \"end\" in src/parser/src/ast/ returned only a single LIR print token, not a reserved word). Singular name retained across all 13 stubs and shims; no fallback to Draw.finish() needed."
  - "Keyword probe verified via ironc build of a minimal _Probe.end() fixture. ironc parsed/lowered/emitted C cleanly; only failure was the expected linker error for the empty-body stub (no Iron__Probe_end symbol existed). Parse + HIR + analyzer + LIR + emit_c stages ALL passed — conclusive proof."
  - "4 struct-by-value INPUT ABIs validated on first clang compile — Phase 60 _Static_assert grid held for Color (4 bytes), Camera2D (24 bytes), RenderTexture (44 bytes), Shader (16 bytes). No layout drift."
  - "memcpy(&rl, &<iron_param>, sizeof(<raylib_type>)) body pattern adopted uniformly for all 4 struct-inputs to avoid strict-aliasing warnings (clang -Wall -Wextra clean)."
  - "BlendMode enum parameter follows Phase 62 typed-enum pattern: declared as BlendMode on the Iron side, lowered to int32_t at the FFI boundary, cast to (int) in the C shim before passing to BeginBlendMode."
  - "4-scalar BeginScissorMode wrapper takes 4 int32_t and casts each to (int) — same pattern as Iron_window_set_position (Phase 61)."

patterns-established:
  - "13-shim Draw-namespace block: prototypes in iron_raylib.h under '── 2D Drawing (Phase 63) ──' marker; implementations in iron_raylib.c under same marker; Iron-side stubs at EOF of raylib.iron under banner '-- 2D Drawing (Phase 63)'"
  - "Struct-by-value input via memcpy + raylib local: `Color rl; memcpy(&rl, &color, sizeof(Color)); ClearBackground(rl);` — becomes the canonical template for every Phase 63-02/03/04 draw primitive that takes a Color/Vector2/Rectangle by value"
  - "Phase 60..72 section markers preserved verbatim in both iron_raylib.h and iron_raylib.c — insert between the Phase 63 marker and the following Phase 64 marker; never reorder or delete downstream markers"

requirements-completed: [DRAW2D-01, DRAW2D-02, DRAW2D-03, DRAW2D-04, DRAW2D-05, DRAW2D-06]

# Metrics
duration: 3 min
completed: 2026-04-16
---

# Phase 63 Plan 01: 2D Drawing — Frame + Stack Modes Summary

**13 Draw.* frame/camera/texture/shader/blend/scissor stubs + shims landed; 4 new struct-by-value INPUT ABIs (Color, Camera2D, RenderTexture, Shader) validated clean via clang on first compile.**

## Performance

- **Duration:** ~3 min
- **Started:** 2026-04-16T22:46:58Z
- **Completed:** 2026-04-16T22:49:44Z
- **Tasks:** 3
- **Files modified:** 3

## Accomplishments
- `Draw.end()` keyword-collision question conclusively answered: `end` is not a reserved word in Iron; singular method name legal. Probe ran ironc on `func _Probe.end() {}` fixture; parse, HIR, analyzer, LIR, and emit_c stages all passed. (Only failure was the expected linker error on the empty-body stub — not a language-level rejection.)
- 13 `Iron_draw_*` C prototypes declared in `src/stdlib/iron_raylib.h` under the Phase 63 section marker (lines 513-528).
- 13 `Iron_draw_*` shim implementations added to `src/stdlib/iron_raylib.c` forwarding to raylib's frame/stack-mode functions (BeginDrawing/EndDrawing/ClearBackground/BeginMode2D/EndMode2D/BeginTextureMode/EndTextureMode/BeginShaderMode/EndShaderMode/BeginBlendMode/EndBlendMode/BeginScissorMode/EndScissorMode). clang `-Wall -Wextra` exits 0 with zero warnings.
- 13 `func Draw.*` empty-body Iron stubs appended to `src/stdlib/raylib.iron` under a new `-- 2D Drawing (Phase 63) --` banner at EOF. Breakdown: 7 void no-arg, 4 single-struct-input (Color/Camera/RenderTexture/Shader), 1 typed-enum (BlendMode), 1 four-scalar (Int32 x 4).
- 4 new struct-by-value INPUT ABI surfaces validated on first clang compile — Phase 60 `_Static_assert` grid held for Color (4 bytes), Camera2D (24 bytes), RenderTexture (44 bytes), Shader (16 bytes). No layout drift, no warnings.
- Phase 60 `_Static_assert` grid (`iron_raylib_layout.c`) still compiles clean — no regression to the 392-assertion ABI ground truth.
- No ironc invocation in Task 2 or Task 3 (memory discipline preserved per HANDOFF.md — ironc is ~10 GB/run). End-to-end pong.iron validation deferred to Plan 63-04.

## Task Commits

Each task was committed atomically:

1. **Task 1: Probe `Draw.end()` keyword collision + append 13 prototypes to iron_raylib.h** — `e2739f6` (feat)
2. **Task 2: Implement 13 frame + stack-mode shims in iron_raylib.c** — `ec632e5` (feat)
3. **Task 3: Add 13 Iron-side func Draw.* stubs to raylib.iron** — `7768040` (feat)

**Plan metadata:** pending — committed at end of plan with SUMMARY.md + STATE.md + ROADMAP.md + REQUIREMENTS.md

## Files Created/Modified
- `src/stdlib/iron_raylib.h` — +16 lines: 13 Iron_draw_* prototypes under the Phase 63 marker.
- `src/stdlib/iron_raylib.c` — +68 lines: 13 Iron_draw_* shim implementations with memcpy bodies for the 4 struct-by-value inputs.
- `src/stdlib/raylib.iron` — +81 lines: new `-- 2D Drawing (Phase 63) --` banner + 13 `func Draw.*` stubs at EOF.

## Decisions Made
- **Draw.end() singular name retained.** Probe result: ironc parses `func _Probe.end() {}` cleanly — `end` is not in Iron's reserved word set. No fallback to `Draw.finish()` needed. All 13 stubs use the singular naming convention.
- **memcpy-to-local-raylib-struct pattern** adopted uniformly for all 4 struct-by-value inputs (Color, Camera2D, RenderTexture, Shader) — copied from Iron_window_set_icon (Phase 61) and Iron_set_shapes_texture (Phase 62). Avoids strict-aliasing warnings under `-Wall -Wextra`; compiler elides the copy for trivially copyable structs.
- **Single Shader 16-byte layout retained.** Iron_Shader's `_locs` is stored as Iron `Int` (int64_t, 8 bytes) to match raylib's `int *locs` pointer size on 64-bit targets. Phase 60-04 `_Static_assert` locked this mapping; Plan 63-01's memcpy uses `sizeof(Shader)` (the raylib typedef) so any future host-pointer-size change would fail the layout assert before reaching the shim.
- **BlendMode typed-enum parameter.** Plan carried forward Phase 62's typed-enum FFI convention: Iron declares `mode: BlendMode`, ironc lowers to int32_t at the C boundary, shim casts to `(int)` before calling BeginBlendMode. User code cannot pass an out-of-range int (enum type constraint), satisfying threat T-63-03.

## Deviations from Plan

None — plan executed exactly as written. Every acceptance criterion on every task passed on the first attempt. No auto-fixes (Rules 1-3) triggered, no architectural decisions (Rule 4) required.

The only "interesting" moment: the overall verification criterion `grep -c 'memcpy(&rl, &' src/stdlib/iron_raylib.c` expected "at least 5" citing "existing Phase 61 memcpys." The actual count is 4 (the 4 new Draw shims) because Phase 61's pre-existing memcpys use different destination names (`&rl_image`, `&out`) and don't match the strict `&rl,` pattern. This is a plan-side spec imprecision — the 4 new struct-by-value memcpys are all present and correct. Not treated as a deviation; flagged here for plan template cleanup in future phases.

## Issues Encountered

- **Branch state at plan start.** The repo's `main` branch does not yet contain Phase 60/61/62 work — those live on `remote/pr-28` (STATE.md documents this: "39 commits ahead of main through Phase 62 + Phase 63 planning"). Executor switched to `remote/pr-28` before executing; all 3 task commits land on that branch. No code-level action needed — the branch situation is a pre-existing project state tracked in STATE.md's Accumulated Context.

## User Setup Required

None — no external service configuration required. Plan 63-01 is pure stdlib ABI work; no env vars, no dashboards, no credentials.

## Next Phase Readiness

- **Plan 63-02 is fully unblocked.** The Color struct-by-value INPUT pattern is now a known-good template: every draw primitive in 63-02 (DrawRectangle, DrawCircle, DrawLine, DrawTriangle, etc.) uses the same `memcpy(&rl, &color, sizeof(Color))` body + `(int)` casts for scalar coords. No ABI surprises expected.
- **Plan 63-03 is unblocked.** DrawText / measure_text / DrawTextEx / font-loading shims can reuse the Font struct-by-value pattern; Plan 60-03's `_Static_assert` grid already covers Font's 48-byte layout, and Plan 63-01's Shader memcpy proves the same pattern holds for non-trivial embedded-object types (Font embeds Texture which embeds Image ID).
- **Plan 63-04 is unblocked for the canonical end-to-end pong restore.** All 13 Draw namespace methods needed for the pong render loop (begin/end/clear, begin_mode_2d/end_mode_2d where applicable, begin_scissor_mode/end_scissor_mode) are bound. Plan 63-02's primitive binding + Plan 63-03's text binding must still land before 63-04 closes the `-- PHASE 63:` markers in pong.iron / game_raylib.iron / hello_raylib.iron.
- **No blockers for Phase 63 closure.** The risky plan (63-01) landed clean on first compile. Phases 63-02/03/04 become mechanical extensions.

## Self-Check: PASSED

**Commits verified:**
- `e2739f6` — Task 1 (feat(63-01): declare 13 Iron_draw_* C prototypes in Phase 63 section)
- `ec632e5` — Task 2 (feat(63-01): implement 13 Iron_draw_* frame + stack-mode shims)
- `7768040` — Task 3 (feat(63-01): add 13 Phase 63 Draw.* stubs to raylib.iron)

**Files verified modified:**
- src/stdlib/iron_raylib.h (+16 lines, 13 prototypes)
- src/stdlib/iron_raylib.c (+68 lines, 13 shim impls)
- src/stdlib/raylib.iron (+81 lines, 13 Iron stubs + banner)

**Clang baseline verified:**
- `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` → exit 0, zero warnings
- `clang -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` → exit 0 (Phase 60 ABI asserts still pass)

**Acceptance criteria:** All 13 prototype greps (Task 1), all 17 shim-body greps + 2 clang compiles (Task 2), all 13 Iron-stub greps + 4 structural greps (Task 3) returned expected values. No criterion failed.

---
*Phase: 63-2d-drawing*
*Completed: 2026-04-16*
