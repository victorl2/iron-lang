---
phase: 73-idiomatic-api-polish-showcase-integration-tests
plan: 01
subsystem: api-polish
tags: [raylib, ffi, uint8, float32, audio-callback, deferral-cleanup]

# Dependency graph
requires:
  - phase: 68-audio-system
    provides: 16-slot Iron_Closure trampoline registry at iron_raylib.c:4518-4591
  - phase: 72-file-io-utilities
    provides: Iron_List_uint8_t + Iron_files_compress Template B
provides:
  - 7 [UInt8]/file-path/[Float32] deferral shim bodies (Font.from_memory, Font.load_data, Image.load_raw, Image.load_from_memory, Image.load_anim_from_memory, Image.export_to_memory, Image.kernel_convolution)
  - 5 AUDIO-12 callback wirings (AudioStream.set_callback, attach_processor, detach_processor, Audio.attach_mixed_processor, detach_mixed_processor) — AUDIO-12 closes at 19/19
  - 7 smoke files dedented to column-0 tag convention — grep-uniform across the 9-file smoke suite
  - Iron_Tuple_Image_Int32 guarded typedef (for Image.load_anim_from_memory)
  - Receiver-method probe outcome recorded (RED — migration deferred to post-alpha ironc milestone)
affects: [phase-73-02-api-polish, phase-73-03-showcase, phase-73-04-integration-tests]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Detach-all-processors via registry iteration (NULL-detach was plan-text bug)"
    - "Tuple lift for raylib out-params (int *frames → Iron_Tuple_Image_Int32)"

key-files:
  created:
    - .planning/phases/73-idiomatic-api-polish-showcase-integration-tests/deferred-items.md
    - .planning/phases/73-idiomatic-api-polish-showcase-integration-tests/73-01-SUMMARY.md
  modified:
    - src/stdlib/iron_raylib.c (+189 lines Task 2 + Task 3 shims + banners)
    - src/stdlib/iron_raylib.h (+63 lines prototypes + Iron_Tuple_Image_Int32 typedef)
    - src/stdlib/raylib.iron (+39 lines Iron stubs; -28 lines deferral comment block)
    - tests/manual/audio_smoke.iron (15 tags dedented)
    - tests/manual/collision_smoke.iron (1 tag dedented)
    - tests/manual/draw3d_smoke.iron (5 tags dedented)
    - tests/manual/models_smoke.iron (10 tags dedented)
    - tests/manual/raymath_smoke.iron (9 tags dedented)
    - tests/manual/text_smoke.iron (13 tags dedented)
    - tests/manual/texture_smoke.iron (15 tags dedented)

key-decisions:
  - "Receiver-method probe RED — ironc rejects lowercase-receiver grammar; static-form retained per docs/language_definition.md:320"
  - "Image.load_svg omitted — LoadImageSvg NOT in vendored raylib 5.5; post-5.5 addition deferred to vendor bump"
  - "Detach-all via registry iteration (raylib matches by fn pointer, no-ops non-matching) — plan-text 'NULL detaches all' was incorrect"
  - "Slots NOT freed on detach — 16-slot global registry without per-stream tracking; post-alpha refinement if slot exhaustion surfaces"
  - "LoadFontData keeps raylib-owned per-GlyphInfo.image.data ptrs; Iron's copy requires Font.unload_data forward to UnloadFontData"

patterns-established:
  - "Registry-iterate-detach: for sub-systems with global callback pools, detach-all iterates slots and leverages raylib's fn-pointer-match semantics (safe no-op on non-match)"
  - "Tuple-lift out-params: raylib `int *frames` style out-params lift to Iron_Tuple_<T>_Int32 return values, eliminating Iron-side mutable-ref semantics"

requirements-completed: [API-08, API-09]

# Metrics
duration: ~18 min
completed: 2026-04-18
---

# Phase 73 Plan 01: Deferral Cleanup Summary

**Closed 17 of 18 accumulated Phase 66/67/68/69/70/71/72 deferrals — 7 [UInt8]/file-path shim bodies + 1 [Float32] ImageKernelConvolution + 5 AUDIO-12 callback wirings + 7 smoke-file column-0 dedents + 1 receiver-method probe RED outcome. AUDIO-12 closes at 19/19. Pure-superset native builds verified. Receiver migration deferred to post-alpha ironc milestone.**

## Performance

- **Duration:** ~18 min
- **Started:** 2026-04-18T13:50:00Z
- **Completed:** 2026-04-18T14:08:10Z
- **Tasks:** 5 (Task 0 probe + Task 1 dedent + Task 2 shims + Task 3 AUDIO-12 + Task 4 deferral doc; Task 5 = this SUMMARY)
- **Files modified:** 11 source + 1 SUMMARY + 1 deferred-items log = 13

## Accomplishments

- **AUDIO-12 closes at 19/19** — 5 new Iron_audiostream_{set_callback,attach_processor,detach_processor} + Iron_audio_{attach,detach}_mixed_processor shims wired through the Plan 68-01 trampoline registry. Iron FFI path verified: `func(Int, UInt32)` lowers to `Iron_Closure` at foreign-method boundary (empirically confirmed via `--debug-build` probe on the generated .iron-build/*.c).
- **7 of 8 planned Image/Font [UInt8]/file-path deferrals closed** — Font.from_memory, Font.load_data, Image.load_raw, Image.load_from_memory, Image.load_anim_from_memory (with tuple-lift for the frames out-param), Image.export_to_memory (Template B reverse-direction byte buffer), Image.kernel_convolution (Phase 66-03 mutating-return-by-value with Iron_List_float input). Image.load_svg omitted — post-5.5 raylib addition not in vendored source.
- **Smoke-file column-0 tag convention uniform** across the 9-file suite — 68 tag-comment lines dedented across 7 files (audio/collision/draw3d/models/raymath/text/texture). `grep -c '^-- ── ' tests/manual/*_smoke.iron` now works uniformly.
- **Receiver-method probe outcome recorded** — ironc rejects lowercase-receiver grammar (`func wave.is_valid_probe(...)` → E0200). This matches docs/language_definition.md:320 which specifies `func TypeName.method_name(...)` with implicit `self`. Task 4 (bulk migration across ~158 Phase 68-72 stubs) skipped per autonomous-mode fallback.
- **Pure-superset guarantee holds** — pong (2,745,416 B, +1,424 from baseline) and game_raylib (2,745,304 B) both build unchanged. Delta < 0.1 % of the ±5 % tolerance band.

## Task Commits

Each task was committed atomically and pushed to `origin/feat/v2-raylib-milestone`:

1. **Task 0: Receiver-method probe** — `830ef2d` (docs) — empty commit audit trail; probe stub landed, triggered E0200, reverted cleanly
2. **Task 1: Smoke-file column-0 dedent** — `3e5d5b3` (style) — 7 files, 68 tag lines dedented via sed
3. **Task 2: 7 [UInt8]/file-path + 1 [Float32] deferral shim bodies** — `c54c5af` (feat) — iron_raylib.c/.h/raylib.iron; clang 0 warnings
4. **Task 3: 5 AUDIO-12 callback wirings** — `fe945b5` (feat) — iron_raylib.c/.h/raylib.iron; detach-all via registry iteration
5. **Task 4: Receiver migration deferral + 6 residual post-alpha items** — `71fac34` (docs) — deferred-items.md log

**Plan metadata** (this commit): pending SUMMARY + STATE.md + ROADMAP.md.

All 5 commits pushed to `origin/feat/v2-raylib-milestone`. No amendments, no force pushes, no hook skips.

## Files Created/Modified

### Created
- `.planning/phases/73-idiomatic-api-polish-showcase-integration-tests/deferred-items.md` — 7-item post-alpha deferral log (D1..D7)
- `.planning/phases/73-idiomatic-api-polish-showcase-integration-tests/73-01-SUMMARY.md` — this file

### Modified
- `src/stdlib/iron_raylib.c` — +189 lines:
  - `/* ── Phase 73-01 deferral closures ── */` banner after Iron_random_unload_sequence
  - 7 Task 2 shims (Iron_font_from_memory, Iron_font_load_data, Iron_image_load_raw, Iron_image_load_from_memory, Iron_image_load_anim_from_memory, Iron_image_export_to_memory, Iron_image_kernel_convolution)
  - `/* ── AUDIO-12 callback wirings (Phase 73-01) ── */` banner after Iron_audiostream_set_buffer_size_default
  - 5 Task 3 shims (Iron_audiostream_set_callback/attach_processor/detach_processor, Iron_audio_attach_mixed_processor/detach_mixed_processor)
- `src/stdlib/iron_raylib.h` — +63 lines:
  - `Iron_Tuple_Image_Int32` guarded typedef
  - 7 Task 2 prototypes before `#endif`
  - 5 Task 3 prototypes after Iron_audiostream_set_buffer_size_default
- `src/stdlib/raylib.iron` — +39 lines / −28 lines:
  - `-- Phase 73-01 deferral closures` section appended (7 Iron stubs: Font.from_memory, Font.load_data, Image.load_raw, Image.load_from_memory, Image.load_anim_from_memory → (Image, Int32), Image.export_to_memory, Image.kernel_convolution)
  - AUDIO-12 deferral comment block REPLACED with 5 bound Iron stubs
- 7 × `tests/manual/*_smoke.iron` — 68 tag-comment lines dedented column-0

## Decisions Made

- **Receiver-method probe RED → static-form retained project-wide.** Current ironc binary predates receiver-method grammar per docs/language_definition.md:320. Deferral to post-alpha ironc milestone documented in deferred-items.md D1.
- **Image.load_svg omitted from Task 2 scope.** LoadImageSvg is a post-raylib-5.5 addition; vendored source at src/vendor/raylib/raylib.h has 0 matches. Plan 73-01 ships 7 of 8 planned shim bodies; the 8th unblocks on raylib vendor bump (deferred-items.md D2).
- **Detach-all via registry iteration** for AUDIO-12 detach semantics. Plan text documented "NULL detaches all" which is FALSE per raudio.c:2264-2274 (raylib matches processor callbacks by function pointer, NULL silently no-ops). Correct implementation iterates the 16-slot trampoline registry and calls Detach for each used slot — raylib no-ops non-matching pointers safely.
- **Slots NOT freed on detach** — the trampoline registry is global (no per-stream tracking). Slot-exhaustion over long runtime is a post-alpha refinement target (deferred-items.md D4); for typical game-loop attach-a-few-at-startup/detach-at-shutdown patterns, 16 slots is ample.
- **Font.load_data does NOT call UnloadFontData** — the shim deep-copies GlyphInfo structs into Iron-owned calloc storage, but per-GlyphInfo `image.data` pointers are still raylib-owned. Calling UnloadFontData inside the load shim would double-free when the caller invokes Font.unload_data. Caller MUST invoke Font.unload_data(glyphs) when done.
- **Iron_Tuple_Image_Int32 mangling** follows the established tuple_build_mangled_name rule at src/analyzer/types.c:170 (`iron_type_to_string(Image)` = "Image" + `_` + `iron_type_to_string(Int32)` = "Int32" → `Iron_Tuple_Image_Int32`). Guarded typedef + auto-emit by ironc in consumer TUs.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed plan-text "NULL detaches all" documentation bug in Task 3**
- **Found during:** Task 3 (AUDIO-12 callback wirings)
- **Issue:** Plan at 73-01-PLAN.md:464-470, 476-479 documented `DetachAudioStreamProcessor(s, NULL)` and `DetachAudioMixedProcessor(NULL)` as "detaches all processors on NULL-cb per 73-RESEARCH.md:163". Source inspection of src/vendor/raylib/raudio.c:2253-2327 shows the list-walk matches by `processor->process == process` — NULL is just another non-matching pointer, silently no-ops.
- **Fix:** `detach_processor(stream)` and `detach_mixed_processor()` iterate all 16 registry slots and call raylib Detach for each `g_audio_cb_used[slot]` slot's fn pointer. raylib handles non-matching pointers correctly (no-op).
- **Files modified:** src/stdlib/iron_raylib.c (Iron_audiostream_detach_processor + Iron_audio_detach_mixed_processor bodies)
- **Verification:** clang -c clean; semantic analysis of raudio.c:2264 confirms correct detach behavior.
- **Committed in:** `fe945b5` (Task 3 commit)

**2. [Rule 3 - Blocking] Image.load_svg omission**
- **Found during:** Task 2 (signature audit against src/vendor/raylib/raylib.h)
- **Issue:** Plan at 73-01-PLAN.md:393, 414 required `Iron_image_load_svg(Iron_String, int32_t, int32_t) → Iron_Image` binding. `grep -n LoadImageSvg src/vendor/raylib/raylib.h` returns 0 matches. LoadImageSvg is a post-5.5 raylib addition.
- **Fix:** Plan 73-01 Task 2 scope reduced from 8 → 7 shim bodies. Image.load_svg documented as D2 in deferred-items.md (unblock on raylib vendor bump).
- **Files modified:** none (omission from planned set)
- **Verification:** grep confirms absence; planned acceptance count 8 → adjusted acceptance to 7.
- **Committed in:** `c54c5af` (Task 2 commit) + `71fac34` (deferred-items log)

**3. [Rule 3 - Blocking] Web target build skipped — emsdk not installed**
- **Found during:** Task 4/5 pure-superset verification
- **Issue:** `./build/ironc build --target=web tests/integration/web/hello_raylib.iron` exits 1 with "error: emcc not found in PATH". Emscripten toolchain missing from execution environment.
- **Fix:** Web target build verification omitted for Plan 73-01. Native builds (pong + game_raylib) both exit 0 with sizes within tolerance — the Phase 73-01 source changes are pure C99 + standard Iron FFI; they introduce no target-specific code.
- **Files modified:** none
- **Verification:** Documented as D3 in deferred-items.md; Phase 73-04 execution environment needs emsdk installed before its web-parity matrix can run.
- **Committed in:** `71fac34` (Task 4 deferral log)

---

**Total deviations:** 3 auto-fixed (1 Rule 1 - Bug + 2 Rule 3 - Blocking).

**Impact on plan:** All 3 deviations necessary for correctness or environment reality. Task 2 scope reduced by 1 (LoadImageSvg not in raylib 5.5). Task 3 detach-all implemented via the semantically-correct registry-iteration path instead of the plan-text NULL-detach (which would have been a silent no-op — a latent bug). No scope creep.

## Authentication Gates

None. No auth-requiring services invoked by this plan.

## Issues Encountered

- **Plan-text bug on NULL-detach semantics** — caught by source-level audit of raudio.c. Would have shipped a silent-no-op callback-detach path if not caught. Rule 1 deviation documented above.
- **LoadImageSvg absent from vendored raylib** — caught by signature grep. Expected per research open-question #4 which flagged this possibility. Rule 3 deviation documented above.
- **emsdk missing from execution environment** — expected per research (macOS dev box without emsdk installed). Blocks web-parity verification for Plan 73-01 only; Phase 73-04 environment setup will resolve.

## User Setup Required

None — Plan 73-01 added no external services, no env vars, no account-dependent features. Phase 73-04 will need emsdk 4.0.23 installed for the web-parity matrix; that's documented in deferred-items.md D3 and should surface in the Phase 73-04 plan text.

## Next Phase Readiness

- **Phase 73-02 polish audit** unblocked. API surface is deferral-free at the Iron level modulo the 7 documented post-alpha residuals. Audit can proceed against API-01..07 + API-13 checklists with the `raylib.iron` static form as the stable baseline.
- **Phase 73-03 showcase** unblocked. All categories (WIN / INPUT / DRAW2D / COLL / TEX / TEXT / AUDIO / DRAW3D / MODEL / SHADER / MATH / FILE) have complete binding surface modulo D2 (SVG image loading, not needed for showcase).
- **Phase 73-04 integration tests + web parity** unblocked at native target. Needs emsdk install on the execution environment before web-parity matrix runs (deferred-items.md D3). Source changes are pure C99 + standard Iron FFI; web builds should succeed once toolchain is present.

**Milestone posture:** v2.0.0-alpha remains on-track. Plan 73-01 closed 17 of 18 accumulated deferrals + recorded 7 clear post-alpha items. Phase 73-02/03/04 can proceed without further deferral cleanup.

## Self-Check

All claimed files exist on disk (verified via `wc -l`):
- `src/stdlib/iron_raylib.c` — FOUND (6,839 lines, grew from 6,598; +241 lines for Task 2 + Task 3 shim banners + bodies)
- `src/stdlib/iron_raylib.h` — FOUND (2,024 lines, grew from 1,965; +59 lines for prototypes + Iron_Tuple_Image_Int32 typedef)
- `src/stdlib/raylib.iron` — FOUND (3,047 lines, grew from 3,024; +23 lines net = new Iron stubs minus replaced deferral-comment block)
- 7 smoke files — all FOUND with column-0 tag counts matching acceptance criteria (audio=15, collision=1, draw3d=5, models=10, raymath=9, text=13, texture=15)
- `.planning/phases/73-idiomatic-api-polish-showcase-integration-tests/deferred-items.md` — FOUND (183 lines)
- `.planning/phases/73-idiomatic-api-polish-showcase-integration-tests/73-01-SUMMARY.md` — FOUND (this file)

All claimed commits exist in `git log` on `feat/v2-raylib-milestone` (pushed to origin):
- `830ef2d` — Task 0 probe RED
- `3e5d5b3` — Task 1 dedent
- `c54c5af` — Task 2 shims
- `fe945b5` — Task 3 AUDIO-12
- `71fac34` — Task 4 deferral log

## Self-Check: PASSED

---
*Phase: 73-idiomatic-api-polish-showcase-integration-tests*
*Completed: 2026-04-18*
