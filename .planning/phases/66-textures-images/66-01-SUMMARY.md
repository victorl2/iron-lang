---
phase: 66-textures-images
plan: 01
subsystem: stdlib
tags: [raylib, color, palette, ffi, shim, stdlib-binding, void-pointer, uint32]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Color 4 B struct mirror + Vector3/Vector4 layout pinned by 413-entry _Static_assert grid; 6-color rescue palette in raylib.iron:1216-1236"
  - phase: 62-input-keyboard-mouse-gamepad-touch-gestures
    provides: "Bool-from-int coercion pattern (bool)(ColorIsEqual(...) != 0); typed-enum arg cast convention (PixelFormat -> int32_t -> int)"
  - phase: 64-collision-2d-3d
    provides: "Instance-method-on-data-carrying-object dispatch; self-reserved E0101 guard rule (Color.* receivers named c/tint/dst/src/other)"
  - phase: 65-raymath
    provides: "memcpy-in / memcpy-out struct-by-value shim template (Iron_vector2_equals, Iron_matrix_identity); Float32 arg forwarding"
provides:
  - "18 raylib Color-math methods bound as idiomatic Iron methods on Color (TEX-13 closed): is_equal, fade, to_int, normalize, from_normalized, to_hsv, from_hsv, tint, brightness, contrast, alpha, alpha_blend, lerp, from_int, from_pixel_data, to_pixel_data, pixel_data_size"
  - "raylib 5.5 canonical 26-color palette installed in raylib.iron (TEX-14 closed): LIGHTGRAY, GRAY, DARKGRAY, YELLOW, GOLD, ORANGE, PINK, RED, MAROON, GREEN, LIME, DARKGREEN, SKYBLUE, BLUE, DARKBLUE, PURPLE, VIOLET, DARKPURPLE, BEIGE, BROWN, DARKBROWN, WHITE, BLACK, BLANK, MAGENTA, RAYWHITE"
  - "Opaque void* function-argument ABI probed GREEN — first in iron_raylib.c (Color.from_pixel_data / Color.to_pixel_data marshal Iron Int via (void *)(intptr_t)data)"
  - "Vector3 / Vector4 struct-by-value RETURN from a Color-receiver method (ColorToHSV -> Vector3, ColorNormalize -> Vector4) — reverse-direction small-struct return through the FFI"
  - "UInt32 primitive type-annotation handling in hir_lower.c's resolve_type_ann — first function-param use of UInt* in any Iron stdlib foreign-method stub (Color.from_int(hex_value: UInt32))"
affects: [66-02-image-load-gen-save-extract, 66-03-image-transforms-cpu-draw, 66-04-texture-load-update-config-draw, 66-05-smoke-abi-sweep, 67-fonts, 73-polish-performance]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Color-in / Color-out memcpy shim: Color local, memcpy from Iron_Color, raylib call, memcpy to Iron_Color out, return by value"
    - "Opaque void* arg marshalling: (void *)(intptr_t)data round-trip; sizeof(int64_t) == sizeof(void *) contract (iron_raylib.h top-of-file)"
    - "Bool return from Color receiver: (bool)(ColorIsEqual(a, b) != 0) — Phase 62 coercion applied to Color.is_equal"
    - "Vector3/Vector4 struct-by-value return from Color-receiver shim — extends Phase 65 raymath return template to a different receiver type"
    - "Module-level `val NAME = Color(r, g, b, a)` palette constants with integer literals (Iron narrows to UInt8 via typecheck.c:1326) — Phase 60-08 precedent scaled from 6 to 26 entries"

key-files:
  created:
    - ".planning/phases/66-textures-images/66-01-SUMMARY.md"
  modified:
    - "src/stdlib/iron_raylib.c (17 Iron_color_* shim implementations under Phase 66 section marker)"
    - "src/stdlib/iron_raylib.h (17 Iron_color_* C prototypes under Phase 66 section marker)"
    - "src/stdlib/raylib.iron (17 func Color.* foreign-method stubs + 26-color palette replacing 6-color rescue block)"
    - "src/hir/hir_lower.c (resolve_type_ann: add UInt/UInt8/UInt16/UInt32/UInt64 primitive resolution)"
    - ".gitignore (ignore root-level /pong build artifact)"

key-decisions:
  - "17 Color.* foreign-method stubs bound for 18 TEX-13 functions: Color.to_pixel_data is the void-return write helper corresponding to raylib's SetPixelColor (documented acceptance note in plan)"
  - "Opaque void* arg passes through the FFI as Iron Int (int64_t); the shim recovers the pointer via (void *)(intptr_t)data. First function-argument use of this pattern in iron_raylib.c — previously used only as a struct field cast (c:538)"
  - "UInt32 resolved via a symmetric addition to resolve_type_ann in hir_lower.c — aligns the UInt family with the Int8/16/32/64 + Float32/64 entries that were already present. No architectural change; pure name-table fill-in"
  - "26-color palette replaces the Plan 60-08 rescue block wholesale; all 6 legacy names (RED/BLUE/GREEN/WHITE/BLACK/DARKGRAY) are preserved exactly, so pong.iron / game_raylib.iron / hello_raylib.iron compile unchanged"
  - "Receiver names for the Color-math stubs: c / other / tint / dst / src / factor / alpha / contrast — NEVER `self` (E0101 reserved), following Phase 64-01 convention"

patterns-established:
  - "Color-math shim template: Color local; memcpy from Iron_Color; raylib call; memcpy to Iron_Color out — lifts directly to every subsequent Color method"
  - "Opaque void* arg ABI: Iron `Int` (int64_t) on both sides, (void *)(intptr_t)data cast inside the shim. Applies to TEX-10 texture.update(pixels) in Plan 66-04"
  - "Primitive UInt* parameter annotations now work end-to-end through ironc (hir_lower.c -> hir_to_lir.c -> emit_c.c). Unblocks any future raylib binding that needs `unsigned int` hex codes, IDs, or bitflags as Iron UInt32 args"

requirements-completed: [TEX-13, TEX-14]

# Metrics
duration: 7min
completed: 2026-04-17
---

# Phase 66 Plan 01: Color Math + 26-Color Palette Summary

**18 raylib Color-math functions bound as idiomatic Iron methods on Color + raylib 5.5 canonical 26-color palette replaces Plan 60-08's 6-color rescue; opaque void* function-arg ABI and UInt32 primitive-param annotation both probe GREEN**

## Performance

- **Duration:** 7 min
- **Started:** 2026-04-17T13:26:31Z
- **Completed:** 2026-04-17T13:33:24Z
- **Tasks:** 2
- **Files modified:** 5 (1 new: 66-01-SUMMARY.md; 4 modified: iron_raylib.{c,h}, raylib.iron, hir_lower.c, .gitignore)

## Accomplishments
- 17 `func Color.*` foreign-method stubs in raylib.iron + 17 `Iron_color_*` C prototypes in iron_raylib.h + 17 matching shim implementations in iron_raylib.c — covering all 18 TEX-13 functions (Color.to_pixel_data is the void-return write helper). Every shim follows the memcpy-in / raylib-call / memcpy-out template; zero `-Wall -Wextra` warnings on macOS arm64.
- Plan 60-08 6-color rescue palette (RED/BLUE/GREEN/WHITE/BLACK/DARKGRAY at raylib.iron:1216-1236) replaced wholesale with the canonical 26-color raylib 5.5 palette. RGBA values copied verbatim from src/vendor/raylib/raylib.h lines 175-201. Existing consumers (pong.iron, game_raylib.iron, hello_raylib.iron) compile unchanged because all 6 legacy names are preserved.
- `Color.from_pixel_data(data: Int, format: PixelFormat) -> Color` and `Color.to_pixel_data(data: Int, c: Color, format: PixelFormat)` are the first opaque-`void *` function-argument shims in iron_raylib.c. The (void *)(intptr_t)data round-trip compiles clean — RESEARCH Open Question 2 resolved GREEN.
- Vector3 (ColorToHSV) and Vector4 (ColorNormalize) returns flow cleanly from Color-receiver shims — small-struct return paths exercised on a non-raymath receiver for the first time.
- `./build/ironc build examples/pong/pong.iron` produces a 2,685,816-byte Mach-O arm64 executable — palette replacement ships end-to-end.
- TEX-13 and TEX-14 requirements closed.

## Task Commits

Each task was committed atomically:

1. **Task 1: Bind 18 Color math functions (TEX-13)** - `6c80d43` (feat)
2. **Task 2: Install raylib 5.5 canonical 26-color palette (TEX-14)** - `ffcae94` (feat)

_Task 2's commit also contains the Rule 3 deviation fix to hir_lower.c — logical fit because the fix surfaced while verifying Task 2's pong build and is required for Color.from_int's UInt32 arg to link._

## Files Created/Modified
- `src/stdlib/iron_raylib.h` - 17 `Iron_color_*` prototypes under the Phase 66 Textures section marker (line 938).
- `src/stdlib/iron_raylib.c` - 17 shim implementations under the matching marker (line 2619). All follow the memcpy-in / raylib-call / memcpy-out template. Includes the first opaque void* function-arg in the file (Iron_color_from_pixel_data / Iron_color_to_pixel_data).
- `src/stdlib/raylib.iron` - 17 `func Color.*` foreign-method stubs added before the rescue palette block. Rescue palette block replaced with 26-color canonical palette + new header comment.
- `src/hir/hir_lower.c` - resolve_type_ann extended to recognise UInt/UInt8/UInt16/UInt32/UInt64 primitive type annotations (symmetric with the existing Int8..64 entries).
- `.gitignore` - ignore the root-level `/pong` build artifact produced by the palette verification build.
- `.planning/phases/66-textures-images/66-01-SUMMARY.md` - this file.

## Decisions Made
- **Color.to_pixel_data returns void, not Color.** raylib's SetPixelColor writes into the caller's pixel buffer and returns void; mirroring that exactly keeps the Iron API truthful about the side-effect pattern.
- **Opaque `void *` marshalled as Iron Int, not a new opaque type.** Matches the Image._data / Texture.id opaque-field convention from Plan 60-03 and is the minimum surface area that gets the work done. A dedicated `PixelBuffer` opaque-handle type is deferred to a later phase if the pattern proliferates.
- **UInt32 fix applied symmetrically.** resolve_type_ann already had Int8/16/32/64 and Float32/64; adding UInt/UInt8/UInt16/UInt32/UInt64 is a pure name-table fill-in, not an architectural change. Other codepaths (typecheck.c, emit_helpers.c, hir_to_lir.c, value_range.c) already handle IRON_TYPE_UINT*.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocker] resolve_type_ann did not recognise `UInt32` type annotation**
- **Found during:** Task 2 (pong.iron build verification)
- **Issue:** `./build/ironc build examples/pong/pong.iron` failed with `/tmp/iron_*.c:1014:37: error: argument may not have 'void' type` on `Iron_Color Iron_color_from_int(void hex_value);`. Root cause traced to `src/hir/hir_lower.c:210-220` — the local `resolve_type_ann` function only had name-table entries for Int, Float, Bool, String, Void, Int8..64, Float32..64. `UInt`, `UInt8`, `UInt16`, `UInt32`, `UInt64` all fell through to the named-type lookup path, which returned NULL because they are primitive aliases, not user-declared types. `emit_type_to_c(NULL, ...)` returns "void".
- **Fix:** Added five symmetric entries to resolve_type_ann — one per UInt family member — that call `iron_type_make_primitive(IRON_TYPE_UINT*)`. Matches the existing Int8..64 entries byte-for-byte in structure.
- **Files modified:** src/hir/hir_lower.c
- **Verification:** ironc rebuilt via `cmake --build build --target ironc` (3 objects, clean). `./build/ironc build examples/pong/pong.iron` produces a 2.68 MB Mach-O arm64 executable. clang -c iron_raylib.c still exits 0 with zero `-Wall -Wextra` warnings.
- **Committed in:** ffcae94 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 3 — blocker)
**Impact on plan:** The Task 1 plan explicitly called out UInt32 as "the primitive uint32_t arg path" probe. The probe surfaced a genuine compiler gap that was not recorded in any prior phase (because no stdlib foreign-method stub had used a UInt* primitive as a function parameter before — only as struct fields). The fix is trivial, symmetric, and unblocks every future raylib binding that needs `unsigned int` hex codes / IDs / bitflags as Iron UInt32 args. No scope creep.

## Issues Encountered
None beyond the Rule 3 deviation documented above. The opaque `void *` function-arg probe — flagged as RESEARCH Open Question 2 requiring a first-time exercise — compiled clean on the first try (sizeof(int64_t) == sizeof(void *) contract holds as expected). Vector3/Vector4 struct-by-value return from a Color receiver also compiled clean on the first try, as predicted from the Phase 65 precedent.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- **Plan 66-02 (Image load/gen/save/extract) is unblocked.** The memcpy shim template, Color-by-value, Vector3/Vector4 return paths, and the UInt32 primitive-arg fix are all in place. The first real probe for Plan 66-02 is the `-> [Color]` reverse-direction `Iron_List` return via `LoadImageColors`.
- **Memory-buffer deferrals confirmed:** LoadImageRaw / LoadImageFromMemory / LoadImageAnimFromMemory / ExportImageToMemory remain DEFERRED per Phase 66 RESEARCH Pitfall 7 (`[UInt8]` primitive-array FFI blocker). They belong to a follow-up phase when the `Iron_List_uint8_t` runtime is landed, not Plans 66-02..05.
- **Requirements traceability:** TEX-13 and TEX-14 fully closed. Phase 66 cumulative: 2/14 requirements complete (14.3%). Remaining: TEX-01..12 across Plans 66-02..04.

## Self-Check: PASSED

- `src/stdlib/iron_raylib.c` — FOUND (17 Iron_color_ shim implementations verified via `grep -c 'Iron_color_'` = 17)
- `src/stdlib/iron_raylib.h` — FOUND (17 Iron_color_ prototypes verified via `grep -c 'Iron_color_'` = 17)
- `src/stdlib/raylib.iron` — FOUND (17 `^func Color\.` stubs + 26 palette `^val NAME =` entries verified)
- `src/hir/hir_lower.c` — FOUND (5 new UInt resolve_type_ann entries)
- Commit `6c80d43` — FOUND in git log (Task 1: feat(66-01) bind 18 Color math functions)
- Commit `ffcae94` — FOUND in git log (Task 2: feat(66-01) install canonical 26-color palette + UInt fix)
- clang -c iron_raylib.c — EXIT 0, zero warnings
- clang -c iron_raylib_layout.c — EXIT 0, grid unchanged (413 asserts)
- `./build/ironc build examples/pong/pong.iron` — produces 2,685,816-byte Mach-O arm64 executable

---
*Phase: 66-textures-images*
*Completed: 2026-04-17*
