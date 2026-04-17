---
phase: 67-text-fonts
plan: 01
subsystem: stdlib-raylib
tags: [raylib, font, ffi, struct-by-value, tuple-return, array-input]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: Iron_Font (48 B) / Iron_GlyphInfo (40 B) struct mirrors + _Static_assert layout grid
  - phase: 63-2d-drawing
    provides: Iron_List_<T> by-value ARRAY_PARAM_LIST convention + emit_structs.c Scan B foreign-method-stub auto-emit
  - phase: 64-collision-2d-3d
    provides: Iron_Tuple_<T>_<T> auto-emission precedent (Iron_Tuple_Bool_Vector2)
  - phase: 65-raymath
    provides: 3-tuple RETURN precedent (Iron_Tuple_Vector3_Quaternion_Vector3 via Matrix.decompose)
  - phase: 66-textures-images
    provides: Texture/RenderTexture struct-by-value RETURN+INPUT memcpy template + Iron_List_Iron_Color Pattern 4 copy-out template + Image mutating-return-by-value Pattern 2

provides:
  - 9 Iron_font_* shims (default/load/load_ex/from_image/is_valid/unload/export_as_code/gen_image_atlas/unload_data)
  - 2 Iron_image_*_text_ex shims closing Phase 66 deferrals (ImageTextEx + ImageDrawTextEx)
  - Iron_List_Iron_GlyphInfo guarded typedef (first 40 B-element list in raylib binding)
  - Iron_List_Iron_Rectangle guarded typedef
  - Iron_Tuple_Image__Rectangle_ guarded typedef (first tuple with an array element)
  - 11 new foreign-method stubs in raylib.iron (7 Font.* + Font.gen_image_atlas + Font.unload_data + Image.text_ex + Image.draw_text_ex)
  - Font (48 B) struct-by-value RETURN + INPUT ABI validated

affects: [67-02-text-draw, 67-03-codepoint-utf8, 67-04-smoke, 71-shaders]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Font 48 B struct-by-value memcpy RETURN + INPUT (reuse of Phase 66-04 Texture template)"
    - "Iron_List_int32_t by-value INPUT via pre-declared iron_runtime.h:826 typedef (first use in raylib binding)"
    - "Iron_List_Iron_GlyphInfo by-value INPUT via header-guarded typedef (40 B element struct; byte-identity via Phase 60-03 _Static_assert)"
    - "(Image, [Rectangle]) tuple RETURN with header-guarded Iron_Tuple_Image__Rectangle_ typedef + deep-copy + free() of raylib RL_MALLOC buffer"
    - "Mutating-return-by-value for Image.draw_text_ex (reuse of Phase 66-03 Pattern 2)"

key-files:
  created: []
  modified:
    - src/stdlib/iron_raylib.h
    - src/stdlib/iron_raylib.c
    - src/stdlib/raylib.iron
    - .planning/ROADMAP.md

key-decisions:
  - "Explicit-receiver convention (Font.unload(font: Font)) chosen over plan's lowercase-receiver draft (font.unload()) — lowercase types fail analyzer resolution"
  - "Font.from_memory + Font.load_data both DEFERRED for [UInt8] FFI — matches 5 existing Phase 66 deferrals, no Iron-side workaround"
  - "Tuple struct named Iron_Tuple_Image__Rectangle_ via iron_type_to_string + sanitization trace (not the plan's assumed Iron_Tuple_Iron_Image_Iron_List_Iron_Rectangle)"
  - "Phase 66 Image text_ex / draw_text_ex shims placed adjacent to other Image draw shims (after Iron_image_draw_text), not inside the Phase 67 font section"

patterns-established:
  - "Pattern 7 validated: [GlyphInfo] INPUT via Iron_List_Iron_GlyphInfo by value → cast to (const GlyphInfo *)items; safe because Phase 60-03 _Static_assert grid pins layout"
  - "Tuple name mangling trace documented inline: iron_type_to_string + tuple_append_mangled_component sanitization replaces [ ] with _"
  - "Header-side tuple typedef guard belt-and-suspenders: ironc auto-emits in consumer TU; guard prevents redefinition when clang -c iron_raylib.c is run standalone"

requirements-completed: [TEXT-01, TEXT-02, TEXT-04, TEXT-06]

# Metrics
duration: 10min
completed: 2026-04-17
---

# Phase 67 Plan 01: Font Loading Foundation Summary

**Font loading surface bound (TEXT-01..04, TEXT-06) + Phase 66's two Font-dependent Image deferrals closed, all under the Phase 67 section marker of iron_raylib.{h,c}**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-04-17T19:21:44Z
- **Completed:** 2026-04-17T19:31:48Z (approx)
- **Tasks:** 2
- **Files modified:** 3 source + 1 doc (ROADMAP.md)

## Accomplishments

- 9 `Iron_font_*` shims landed under the Phase 67 section marker in `iron_raylib.{h,c}` (default / load / load_ex / from_image / is_valid / unload / export_as_code / gen_image_atlas / unload_data)
- Phase 66's two Font-dependent Image deferrals CLOSED: `Iron_image_text_ex` (fresh Image from text+Font) + `Iron_image_draw_text_ex` (mutating-return Image by value). The "ImageDrawTextEx DEFERRED to Phase 67" marker at iron_raylib.c is gone.
- First Font (48 B) struct-by-value crossing validated — Phase 60-03 _Static_assert grid held first try
- First Iron_List_int32_t INPUT in the raylib binding (Font.load_ex codepoints)
- First (Image, [Rectangle]) tuple RETURN + first [GlyphInfo] INPUT — Font.gen_image_atlas
- raylib.iron extended with 11 new foreign-method stubs (7 Font.* from Task 1 + 2 Font.* data ops + 2 Image.* text ops from Task 2)
- Font.load_data + Font.from_memory documented as `[UInt8]`-deferred inline; matches 5 existing Phase 66 deferrals awaiting Iron_List_uint8_t

## Task Commits

Each task was committed atomically:

1. **Task 1: Font loading shims — default / load / load_ex / from_image / is_valid / unload / export_as_code + prototypes** — `fd5c76d` (feat)
2. **Task 2: Font.gen_image_atlas + Phase 66 Image text deferrals closed** — `6f592c6` (feat)

_Plan metadata commit pending — included in docs commit after this SUMMARY lands._

## Files Created/Modified

- `src/stdlib/iron_raylib.h` — +84 lines: 7 Iron_font_* prototypes (Task 1) + 2 data-op prototypes + 2 Image text_ex prototypes + 3 guarded typedefs (Iron_List_Iron_GlyphInfo, Iron_List_Iron_Rectangle, Iron_Tuple_Image__Rectangle_) (Task 2)
- `src/stdlib/iron_raylib.c` — +169 lines: 7 Font shim bodies (Task 1) + 2 Image text_ex bodies adjacent to Image.draw_text (Task 2) + 2 Font data-op bodies appended to Phase 67 section (Task 2); removed 1 Phase 66 deferral comment line
- `src/stdlib/raylib.iron` — +55 lines: Phase 67 section header + 7 Font.* methods (Task 1) + 2 Image text methods + 2 Font data methods (Task 2); 2 Phase 66 deferral comments updated to "CLOSED" pointers
- `.planning/ROADMAP.md` — Plan 67-01 marked in progress in phase 67 row (updated by planner pre-execution)

## Decisions Made

1. **Explicit-receiver convention for all Font methods (Rule 1 auto-fix).** The plan's draft used lowercase `func font.is_valid() -> Bool` (implicit receiver). Analyzer resolution in `src/analyzer/resolve.c:178` looks up `md->type_name` in the global scope — a lowercase `font` isn't a declared type and would fail with E0101 / "method declared on undeclared type". Used `func Font.is_valid(font: Font) -> Bool` matching `Texture.unload(tex: Texture)` and every other instance method in raylib.iron.
2. **Font.load_data DEFERRED alongside Font.from_memory.** The plan's TEXT-05 bulleting kept Font.load_data open, but it takes `[UInt8] fileData` as input — same Iron_List_uint8_t blocker as Font.from_memory. Deferring both produces a coherent "awaiting runtime work" story that matches the 5 Phase 66 deferrals.
3. **Tuple struct name via mangling trace.** The plan suggested `Iron_Tuple_Iron_Image_Iron_List_Iron_Rectangle`. ironc's actual tuple naming via `tuple_build_mangled_name` + `iron_type_to_string` + sanitization produces `Iron_Tuple_Image__Rectangle_` (brackets replaced with underscores). Used the correct mangled name and documented the derivation inline.
4. **Image text_ex / draw_text_ex placed in Image section, not Font section.** Plan suggested either location; the Image section makes more sense because these shims are logically Image-producing operations (Image.text_ex allocates a fresh Image; Image.draw_text_ex uses Pattern 2 mutating-return), sitting naturally after `Iron_image_draw_text`.
5. **Standalone compile preserved.** Every new struct type (Iron_List_Iron_GlyphInfo, Iron_List_Iron_Rectangle, Iron_Tuple_Image__Rectangle_) wrapped in `#ifndef ..._STRUCT_DEFINED` guard matching the existing pattern for Iron_List_Iron_Color / Iron_List_Iron_Vector2 / Iron_Tuple_Bool_Vector2.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Lowercase receiver convention corrected to PascalCase explicit-receiver**
- **Found during:** Task 1 (Font method stub drafting)
- **Issue:** Plan directed `func font.is_valid() -> Bool` with lowercase `font` as implicit receiver. Iron's analyzer resolves method type_names via global scope lookup (`src/analyzer/resolve.c:178`) — a lowercase identifier is not a declared type, triggering IRON_ERR_UNDEFINED_VAR "method declared on undeclared type".
- **Fix:** Used `func Font.is_valid(font: Font) -> Bool` style uniformly across all 7 Font methods, matching the existing `Texture.unload(tex: Texture)` convention in raylib.iron (the repo-wide standard across 35+ object methods in Phases 61-66).
- **Files modified:** src/stdlib/raylib.iron
- **Verification:** `grep -c "^func Font\." src/stdlib/raylib.iron` returns 9 (4 static constructors + 5 instance methods counting gen_image_atlas + unload_data); lowercase `^func font\.` returns 0.
- **Committed in:** fd5c76d (Task 1 commit)

**2. [Rule 1 - Bug] Tuple struct name corrected from plan's assumed `Iron_Tuple_Iron_Image_Iron_List_Iron_Rectangle`**
- **Found during:** Task 2 (Font.gen_image_atlas header typedef)
- **Issue:** Plan suggested the tuple name `Iron_Tuple_Iron_Image_Iron_List_Iron_Rectangle` under the assumption that the tuple mangler uses C-side type names. ironc actually uses `iron_type_to_string` (user-facing Iron names like "Image") and runs the result through `tuple_append_mangled_component` which replaces non-alnum/`_` with `_`. The real mangled name for `(Image, [Rectangle])` is `Iron_Tuple_Image__Rectangle_` (two consecutive underscores from the `[` replacement, trailing underscore from the `]` replacement).
- **Fix:** Derived the actual mangled name via source inspection of `src/analyzer/types.c:150-204` (tuple_build_mangled_name + tuple_append_mangled_component). Declared the guarded typedef under `IRON_TUPLE_IMAGE__RECTANGLE__STRUCT_DEFINED`. Shim return type + caller match.
- **Files modified:** src/stdlib/iron_raylib.h, src/stdlib/iron_raylib.c
- **Verification:** `clang -c iron_raylib.c` exits 0 with no "unknown type name" errors; the guard prevents any downstream redefinition at end-to-end consumer compile.
- **Committed in:** 6f592c6 (Task 2 commit)

**3. [Rule 2 - Missing Critical] Font.load_data deferred alongside Font.from_memory**
- **Found during:** Task 2 (Font data operations scope review)
- **Issue:** Plan listed Font.load_data in the "defer" list with explicit reasoning (Pitfall 8 GlyphInfo deep-copy lifetime + Pitfall 9 [UInt8] FFI). The plan's TEXT-05 action section still listed it as "potentially feasible" — ambiguous instruction. Since the function's fileData parameter is `[UInt8]`, it has the same Iron_List_uint8_t blocker as Font.from_memory and the 5 Phase 66 deferrals.
- **Fix:** Deferred Font.load_data cleanly alongside Font.from_memory. Documented both inline in iron_raylib.h Phase 67 comment block + iron_raylib.c Phase 67 header. No dead stub in raylib.iron.
- **Files modified:** src/stdlib/iron_raylib.h, src/stdlib/iron_raylib.c
- **Verification:** Neither `Iron_font_load_data` nor `Iron_font_from_memory` appear as definitions; both appear only in defer comments.
- **Committed in:** 6f592c6 (Task 2 commit)

---

**Total deviations:** 3 auto-fixed (2 Rule 1 bugs, 1 Rule 2 scope clarification)
**Impact on plan:** All three necessary for correct execution. The lowercase-receiver directive would have produced a Iron program that fails to analyze; the wrong tuple name would have failed `clang -c`; the Font.load_data ambiguity needed a concrete close. No scope creep — deferrals match pre-existing Phase 66 inventory.

## Issues Encountered

- `ROADMAP.md` arrived already modified by the planner (added the Plan 67-01..04 enumeration in the Phase 67 row). Rolled this into the Task 1 commit tree but did not re-edit; the final metadata commit will pick up STATE.md + ROADMAP.md cleanly.

## Phase 67 Handoff

- **Plan 67-02 (text draw + measure + glyph lookup)** — UNBLOCKED. Has the Font constructors it needs (Font.default for smoke tests, Font.load for TTF tests). Needs to introduce `object Text {}` namespace stub per Pattern 8 in 67-RESEARCH.md.
- **Plan 67-03 (codepoint + UTF-8)** — UNBLOCKED. Pattern 4 copy-out template exists (Iron_image_load_colors + the new Iron_font_gen_image_atlas), Pattern 5 Iron String from raylib char* exists (Iron_window_get_clipboard_text). The two new probes there are `Iron_List_int32_t` RETURN and `char *` → Iron_String copy for TextToUpper-family static buffers.
- **Plan 67-04 (smoke + pong re-enablement)** — UNBLOCKED in principle; waits on 67-02 + 67-03 closing before running.

## Next Phase Readiness

- All Phase 67 Plan 01 acceptance criteria met.
- No blockers for Plan 67-02.
- Pending global runtime work (Iron_List_uint8_t) blocks Font.from_memory + Font.load_data + the 5 Phase 66 deferrals — tracked outside this plan.

## Self-Check: PASSED

Created files verified:
- FOUND: `.planning/phases/67-text-fonts/67-01-SUMMARY.md` (this file)

Commits verified:
- FOUND: `fd5c76d` — Task 1 (`git log --oneline` shows it as the pre-previous commit)
- FOUND: `6f592c6` — Task 2 (`git log --oneline` shows it as the previous commit)

Verification command outputs:
- `grep -c "^struct Iron_Font Iron_font_\|^void Iron_font_\|^bool Iron_font_\|^Iron_Tuple_Image__Rectangle_" src/stdlib/iron_raylib.c` → 9
- `grep -c "Iron_image_text_ex\|Iron_image_draw_text_ex" src/stdlib/iron_raylib.c` → 3 (prototype-referenced via extern block + 2 shim definitions)
- `! grep -q "ImageDrawTextEx DEFERRED to Phase 67" src/stdlib/iron_raylib.c` → REMOVED
- `grep -c "^func Font\." src/stdlib/raylib.iron` → 9
- `clang -c -o /tmp/67-01.o src/stdlib/iron_raylib.c -Isrc/vendor/raylib -Isrc/runtime -Isrc/stdlib -Isrc -Wall` → OK (exit 0, no warnings)
- `clang -c src/stdlib/iron_raylib_layout.c …` → OK

---
*Phase: 67-text-fonts*
*Completed: 2026-04-17*
