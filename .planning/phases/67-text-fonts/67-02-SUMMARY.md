---
phase: 67-text-fonts
plan: 02
subsystem: stdlib-raylib
tags: [raylib, font, text, draw, glyph, ffi, struct-by-value, array-input, codepoint]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: Iron_Font (48 B) / Iron_GlyphInfo (40 B) struct mirrors + _Static_assert layout grid (Image embedded at GlyphInfo+16, Texture embedded at Font+16)
  - phase: 63-2d-drawing
    provides: Draw.* namespace + Color-by-value INPUT memcpy template; iron_string_cstr() consumer pattern
  - phase: 66-textures-images
    provides: Texture/Image struct-by-value RETURN+INPUT memcpy template (reused for Font 48 B + GlyphInfo 40 B)
  - phase: 67-text-fonts Plan 01
    provides: Font constructors (Font.default / Font.load / Font.load_ex / Font.from_image) + Iron_List_Iron_GlyphInfo / Iron_List_Iron_Rectangle guarded typedefs + Iron_List_int32_t INPUT call site precedent

provides:
  - 4 default-font shims: Iron_draw_fps, Iron_draw_text, Iron_text_measure, Iron_text_set_line_spacing
  - 8 Font.* instance-method shims: Iron_font_draw_ex / draw_pro / draw_codepoint / draw_codepoints / measure_ex / get_glyph_index / get_glyph_info / get_glyph_atlas_rec
  - new `object Text {}` namespace stub (matches Phase 60-06 Window/Draw/Audio/Files precedent)
  - 12 new foreign-method stubs in raylib.iron (2 Draw.* + 2 Text.* + 8 Font.*)
  - GlyphInfo (40 B) struct-by-value RETURN ABI validated — first Image-embedded struct crossing the FFI as a return
  - Second Iron_List_int32_t INPUT call site validated (Font.draw_codepoints)

affects: [67-03-codepoint-utf8, 67-04-smoke, 71-shaders]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "GlyphInfo (40 B) struct-by-value RETURN via memcpy — first Image-embedded struct crossing FFI as a return. 40 B is under clang's default -Wlarge-by-value-copy 64 B threshold; Phase 60-03 _Static_assert grid pins byte identity (including embedded Image at offset 16)."
    - "Font (48 B) struct-by-value INPUT repeatedly exercised — every Font.* instance method memcpys Iron_Font into a raylib Font local. Second plan consuming the pattern (after Plan 67-01's constructors)."
    - "Iron_List_int32_t by-value INPUT second call site (Font.draw_codepoints) — re-confirms Plan 67-01 Font.load_ex pattern."
    - "New `object Text {}` namespace alongside Phase 60-06 Window/Draw/Audio/Files — pure static-dispatch target, no fields."
    - "PascalCase explicit-receiver convention for Font.* instance methods (Rule 1 carry-over from Plan 67-01). Applied to all 8 Font.* methods even though plan draft used lowercase `func font.method()` form."

key-files:
  created: []
  modified:
    - src/stdlib/iron_raylib.h
    - src/stdlib/iron_raylib.c
    - src/stdlib/raylib.iron

key-decisions:
  - "Explicit-receiver PascalCase convention for all 8 Font.* instance methods (Rule 1 auto-fix) — plan draft used lowercase `func font.draw_ex(...)` which would fail analyzer resolution (lowercase `font` is not a declared type)"
  - "Draw.fps + Draw.text placed at END of Phase 63 Draw.* block, not interleaved — keeps Phase 63 methods contiguous and Phase 67 extensions identifiable by adjacent comment block"
  - "object Text {} placed alongside Phase 60-06 Window/Draw/Audio/Files stubs near line 960, NOT inside the Phase 67 Text & Fonts section — matches the existing namespace-object grouping convention"
  - "Text.measure / Text.set_line_spacing placed after Font.unload_data (end of Phase 67 section) — keeps default-font utilities adjacent to the Font.* instance methods under the same phase banner"
  - "Used Font type for Iron_font_* receiver even though it mirrors raylib C Font — byte-identity pinned by Phase 60-03 _Static_assert grid"

patterns-established:
  - "GlyphInfo-by-value RETURN pattern validated (40 B, Image embedded at offset 16). Reusable template for any future stdlib binding that returns an Image-embedded struct (none currently on the roadmap, but the ABI is now proven)."
  - "Pitfall 7 lifetime alias documented both in raylib.iron ABOVE Font.get_glyph_info and in the iron_raylib.c section banner — dual documentation point so users see the warning whether they're reading the Iron stub or the shim body."
  - "Plan-per-task commit protocol preserved — backed out the speculative full-plan edits and re-applied Task 1 alone before committing, so per-task commits remain atomic and reviewable."

requirements-completed: [TEXT-07, TEXT-08, TEXT-09, TEXT-10, TEXT-11]

# Metrics
duration: ~7min
completed: 2026-04-17
---

# Phase 67 Plan 02: Text Draws + Measure + Glyph Lookup Summary

**12 text-surface shims bound (Draw.fps/Draw.text + Text.measure/Text.set_line_spacing + 8 Font.* custom-font draws / measure / glyph lookup) — first GlyphInfo (40 B) struct-by-value RETURN crosses the FFI, Image-embedded at offset 16.**

## Performance

- **Duration:** ~7 min
- **Started:** 2026-04-17T19:39:32Z
- **Completed:** 2026-04-17T19:46:52Z
- **Tasks:** 2
- **Files modified:** 3 source (iron_raylib.h, iron_raylib.c, raylib.iron)

## Accomplishments

- 12 `Iron_*` shim bodies landed (4 under Phase 63 + Phase 67 markers for Task 1; 8 under the Phase 67 marker for Task 2)
- `object Text {}` namespace stub added alongside the Phase 60-06 Window/Draw/Audio/Files stubs; pure static-dispatch target
- First GlyphInfo (40 B) struct-by-value RETURN in the raylib binding — embedded Image at offset 16 held byte-identity via Phase 60-03 `_Static_assert` grid on first compile
- Second `Iron_List_int32_t` INPUT call site bound (`Font.draw_codepoints` — re-confirms Plan 67-01 `Font.load_ex` pattern)
- raylib.iron extended with 12 new foreign-method stubs: 2 `Draw.*` (fps/text) + 2 `Text.*` (measure/set_line_spacing) + 8 `Font.*` (draw_ex/draw_pro/draw_codepoint/draw_codepoints/measure_ex/get_glyph_index/get_glyph_info/get_glyph_atlas_rec)
- Rule 1 deviation carried over from Plan 67-01: every new `Font.*` instance method uses explicit PascalCase receiver `func Font.method(font: Font, ...)` rather than the plan draft's lowercase `func font.method(...)` form

## Task Commits

Each task was committed atomically:

1. **Task 1: Draw.fps/Draw.text + Text.* namespace (4 default-font shims + object Text)** — `9f218ee` (feat)
2. **Task 2: Font.* instance methods — draws + measure + glyph lookup (8 shims)** — `71dd947` (feat)

_Plan metadata commit pending — included in the docs commit after this SUMMARY lands._

## Files Created/Modified

- `src/stdlib/iron_raylib.h` — +56 lines: 4 prototypes in Phase 63 + Phase 67 sections (Task 1) + 8 Font.* instance-method prototypes under the Phase 67 section (Task 2) with Pitfall 7 documentation block
- `src/stdlib/iron_raylib.c` — +125 lines: 2 default-font Draw.* shims inside the Phase 63 section (Task 1) + 2 Text.* shims + 8 Font.* instance-method shims under the Phase 67 section marker (Task 2), each with documentation on Pitfall 1 (Window.init) and Pitfall 7 (GlyphInfo.image lifetime)
- `src/stdlib/raylib.iron` — +132 lines: `object Text {}` namespace (near Window/Draw/Audio/Files) + Draw.fps/Draw.text at end of Phase 63 Draw.* block (Task 1) + Text.measure/Text.set_line_spacing + 8 Font.* instance methods in Phase 67 section (Task 2)

## Decisions Made

1. **Explicit-receiver convention enforced on all 8 Font.* instance methods (Rule 1 auto-fix carry-over).** Plan 67-01 established that lowercase-receiver `func font.method()` fails analyzer resolution — `font` is not a declared type. Plan 67-02's `<action>` and `<acceptance_criteria>` blocks still used the lowercase form. Applied the same Rule 1 fix uniformly: every new Font.* stub uses `func Font.method(font: Font, ...)`.
2. **Draw.fps + Draw.text placed at end of Phase 63 Draw.* block, not interleaved.** Keeps the 65 Phase 63 Draw.* methods contiguous and makes the Phase 67 extensions visually identifiable by the adjacent `-- Phase 67 extension:` comment banner.
3. **object Text {} placed alongside other namespace objects near line 960, not in the Phase 67 data-type section.** Matches the Phase 60-06 pattern (Window/Draw/Audio/Files are grouped together as pure namespace stubs with no C mirror). Placing it inside the Phase 67 data-type block would have broken the convention and confused readers.
4. **Text.measure / Text.set_line_spacing placed AFTER Font.unload_data (end of Phase 67 section), not at the top of a new Text.* section.** Keeps all default-font utilities adjacent to Font.* under the same "Text & Fonts (Phase 67)" banner — readers scanning for text-related methods find the whole surface in one contiguous block.
5. **Documentation dual-surface for Pitfall 7.** The GlyphInfo.image lifetime aliasing warning is stated both in `raylib.iron` above `Font.get_glyph_info` (so Iron users see it at the type-stub level) AND in `iron_raylib.c`'s Plan 67-02 Task 2 banner (so anyone debugging the shim sees it at the C level). Single documentation would have been insufficient because readers typically look at one file or the other, not both.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Lowercase receiver convention corrected to PascalCase explicit-receiver for all 8 Font.* instance methods**
- **Found during:** Task 2 drafting (Font.* instance-method stubs)
- **Issue:** Plan 67-02's `<action>` section directed `func font.draw_ex(text: String, position: Vector2, ...)` with lowercase `font` as implicit receiver. This is the same Rule 1 bug that fired in Plan 67-01 — Iron's analyzer resolves method type_names via global scope lookup (`src/analyzer/resolve.c:178`); a lowercase identifier is not a declared type, triggering E0101 / "method declared on undeclared type". The plan's `<acceptance_criteria>` also used lowercase (`grep -q "^func font.draw_ex..."`), reinforcing the bug.
- **Fix:** Applied `func Font.method(font: Font, ...)` uniformly to all 8 new instance methods, matching (a) Plan 67-01's established convention for Font.is_valid / Font.unload / Font.export_as_code, and (b) the repo-wide standard `Texture.unload(tex: Texture)` / `RenderTexture.unload(rt: RenderTexture)` / every other instance method in raylib.iron. Updated the Plan 67-02 acceptance criteria assumption in the SUMMARY via the PascalCase grep counts.
- **Files modified:** src/stdlib/raylib.iron
- **Verification:** `grep -c "^func Font\." src/stdlib/raylib.iron` returns 17 (9 from Plan 67-01 + 8 new from Plan 67-02); `grep -c "^func font\." src/stdlib/raylib.iron` returns 0; clang compile of iron_raylib.c exits 0.
- **Committed in:** 71dd947 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 bug — convention inconsistency between plan draft and repo standard).
**Impact on plan:** Essential for correctness — the lowercase-receiver directive would produce an Iron program that fails to analyze with E0101. Zero scope creep; the fix maps 1:1 from plan-suggested `font.*` to repo-standard `Font.* (font: Font, ...)`. The shim side (C prototypes + bodies) was unaffected — C-side names (`Iron_font_draw_ex` etc.) are derived from the lowercased type name per ironc's mangling convention (`Iron_<lowercased_type>_<method>`).

## Issues Encountered

- **Tooling/workflow friction:** Initially wrote both Task 1 and Task 2 changes into the files in a single pass before committing. To preserve the per-task atomic-commit protocol, backed out all changes via `git checkout -- <files>` and re-applied Task 1 alone before committing `9f218ee`, then re-applied Task 2 and committed `71dd947`. Net effect: identical to what two sequential Task passes would have produced, but a ~30-second detour. No technical issue — pure process discipline.

## Plan 67-02 ABI Validation Record

- **GlyphInfo (40 B) struct-by-value RETURN:** First crossing in the raylib binding. Embeds Image at offset 16 (Image itself is a 24 B struct with void*/int32/int32/int32/int32 layout). Phase 60-03 `_Static_assert` grid confirmed byte identity for all 5 GlyphInfo fields (`value` / `offsetX` / `offsetY` / `advanceX` / `image`) at plan startup time — held first-compile.
- **clang -Wlarge-by-value-copy not triggered.** 40 B is under the default 64 B threshold. Verified via `clang -c -Wall iron_raylib.c` producing zero warnings.
- **Font (48 B) struct-by-value INPUT:** Exercised 8× in Task 2 shims (every Font.* instance method memcpys into a raylib Font local). No warning fires (48 B also under threshold; Phase 60-03 grid held).
- **Iron_List_int32_t by-value INPUT:** Second call site (Font.draw_codepoints), re-confirms Plan 67-01 Font.load_ex. `(const int *)codepoints.items` cast is safe on all 64-bit targets Iron supports (Iron Int32 is int32_t, raylib takes `int` which is 32-bit).

## Phase 67 Handoff

- **Plan 67-03 (codepoint + UTF-8)** — UNBLOCKED. Has `object Text {}` namespace (introduced this plan), `Font.*` instance methods for any follow-up font-aware codepoint helpers, and iron_string_cstr() consumer precedent. Will bind UTF-8 iterators + TextToUpper/Lower/Format-family static-buffer helpers.
- **Plan 67-04 (smoke + pong re-enablement)** — UNBLOCKED as a whole once 67-03 lands. Has Font.load + Draw.text + Font.draw_ex + Text.measure all available for smoke-test canvas. Pong re-enablement path ready: pong.iron's score display can switch from `-- PHASE 67:` commented-out DrawText calls to real `Draw.text(...)` calls with `Text.measure(...)` for centering.

## Next Phase Readiness

- All Phase 67 Plan 02 acceptance criteria met.
- Phase 67 progress: 2/4 plans complete. Plan 67-03 + 67-04 remaining.
- Requirements closed this plan: TEXT-07, TEXT-08, TEXT-09, TEXT-10, TEXT-11 (5 requirements).
- Cumulative Phase 67 requirements closed: TEXT-01, TEXT-02, TEXT-04, TEXT-06 (from 67-01) + TEXT-07..11 (from 67-02) = 9 of 13 TEXT requirements. TEXT-03 + TEXT-05 remain deferred pending `[UInt8]` FFI (Iron_List_uint8_t); TEXT-12, TEXT-13 land in Plan 67-03.

## Self-Check: PASSED

Created files verified:
- FOUND: `.planning/phases/67-text-fonts/67-02-SUMMARY.md` (this file)

Commits verified:
- FOUND: `9f218ee` — Task 1 (Draw.fps/Draw.text + object Text + Text.measure/Text.set_line_spacing)
- FOUND: `71dd947` — Task 2 (8 Font.* instance methods)

Verification command outputs:
- `clang -c -Wall iron_raylib.c` → exit 0, zero warnings
- `clang -c -Wall iron_raylib_layout.c` → exit 0, zero warnings
- `grep -c "^int32_t Iron_text_measure|^void Iron_text_set_line_spacing|^void Iron_draw_fps|^void Iron_draw_text|^void Iron_font_draw_ex|^void Iron_font_draw_pro|^void Iron_font_draw_codepoint|^void Iron_font_draw_codepoints|^struct Iron_Vector2 Iron_font_measure_ex|^int32_t Iron_font_get_glyph_index|^struct Iron_GlyphInfo Iron_font_get_glyph_info|^struct Iron_Rectangle Iron_font_get_glyph_atlas_rec" iron_raylib.c` → **12** (matches plan expected)
- `grep -c "^func Draw.fps|...|^func Font.get_glyph_atlas_rec" raylib.iron` → **12** (matches plan expected)
- `grep -c "^func Draw\." raylib.iron` → **67** (>= 67 expected; 65 Phase 63 + 2 Plan 67-02 additions)
- `grep -c "^func Font\." raylib.iron` → **17** (9 from Plan 67-01 + 8 from Plan 67-02)
- `grep -c "^func Text\." raylib.iron` → **2** (Text.measure + Text.set_line_spacing)
- `grep -c "^object Text {}" raylib.iron` → **1** (namespace stub present)
- `grep -c "^func font\." raylib.iron` → **0** (Rule 1 convention uniformly applied)

---
*Phase: 67-text-fonts*
*Completed: 2026-04-17*
