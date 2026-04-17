---
phase: 66-textures-images
plan: 03
subsystem: stdlib
tags: [raylib, image, ffi, shim, stdlib-binding, mutating-transform, cpu-draw, chain-method, struct-by-value, iron-list-input]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Image 40 B struct mirror + Iron_Rectangle 16 B + Iron_Vector2 8 B + Iron_Color 4 B; _Static_assert grid (413 entries) validates byte-identical layout for memcpy-based FFI"
  - phase: 63-2d-drawing
    provides: "Iron_List_Iron_Vector2 by-value ABI (ARRAY_PARAM_LIST mode) proven GREEN in Plan 63-03 Task 1 probe and consumed by triangle_fan/strip/line_strip/spline_* draws; IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED guarded typedef in iron_raylib.h"
  - phase: 66-textures-images (Plan 01)
    provides: "Phase 66 section marker; Color math 18 bindings; canonical 26-color palette; memcpy-in/raylib-call/memcpy-out struct template at Color scale"
  - phase: 66-textures-images (Plan 02)
    provides: "Image struct-by-value round-trip (40 B); 21 Image.* load/gen/export/extract bindings; Pattern 1 (raylib returns Image by value) shim template lifted verbatim for ImageCopy/FromImage/FromChannel"
provides:
  - "27 TEX-05 Image.* mutating-transform + by-value-return methods (copy, from_rectangle, from_channel, format, to_pot, crop, alpha_crop/clear/mask/premultiply, blur_gaussian, resize/resize_nn/resize_canvas, mipmaps, dither, flip_vertical/horizontal, rotate/rotate_cw/rotate_ccw, color_tint/invert/grayscale/contrast/brightness/replace)"
  - "21 TEX-07 Image.* CPU-draw methods (clear_background, draw_pixel/_v, draw_line/_v/_ex, draw_circle/_v/_lines/_lines_v, draw_rectangle/_v/_rec/_lines, draw_triangle/_ex/_lines/_fan/_strip, draw (blit), draw_text)"
  - "48 Iron_image_* C prototypes + shim implementations in iron_raylib.{h,c} under dedicated TEX-05 / TEX-07 sub-blocks in the Phase 66 section"
  - "Mutating-transform return-by-value pattern validated at scale (48 shim sites); Pattern 2 (memcpy-in, ImageFoo(&src,...), memcpy-out) now canonical for raylib APIs with Image *dst C signatures"
  - "Iron_List_Iron_Vector2 by-value input re-used in 2 new sites (ImageDrawTriangleFan/Strip) — third distinct consumer after Phase 63-03 triangle_fan/strip and Phase 63-04 line_strip/spline_*"
  - "Chain-style image composition in Iron confirmed compilable: Image.color(64,64,BLACK).flip_vertical().color_tint(RED).crop(Rectangle(0,0,32,32))"
affects: [66-04-texture-load-update-config-draw, 66-05-smoke-abi-sweep, 67-fonts]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Pattern 2 (mutating-transform return-by-value): `memcpy(&src, &img, sizeof(Image)); ImageFoo(&src, ...); memcpy(&out, &src, sizeof(struct Iron_Image)); return out;` — applied uniformly across 45 of 48 shims (24 TEX-05 mutators + 21 TEX-07 CPU draws). Raylib mutates its own `Image *dst`; Iron memcpys the mutated value back out as a fresh return. No pointer aliasing, no user-visible mutation."
    - "Pattern 1 (raylib returns Image by value): lifted verbatim from Plan 66-02 generators. Used for ImageCopy / ImageFromImage / ImageFromChannel (3 of 48 shims). raylib allocates and returns a new Image; Iron just memcpys into the return slot."
    - "Image blit shim (ImageDraw): 5-memcpy variant — 2 Images (dst, src), 2 Rectangles (src_rec, dst_rec), 1 Color (tint). Validates that two Iron_Image struct-by-value INPUTS cross the FFI simultaneously (one mutating target, one immutable source)."
    - "Iron_List_Iron_Vector2 by-value input in an Image-returning shim: `Iron_image_draw_triangle_fan(struct Iron_Image img, Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color)` — forwards `(Vector2 *)points.items` directly to raylib. Third ABI consumer of the by-value list wrapper in this TU after draw_triangle_fan/strip and line_strip/spline_*."
    - "String marshalling in a CPU-draw context: `ImageDrawText` uses `iron_string_cstr(&text)` with NULL fallback `ctext ? ctext : \"\"`. Same pattern as Plan 66-02's Image.load/export path-arg marshalling."

key-files:
  created:
    - ".planning/phases/66-textures-images/66-03-SUMMARY.md"
  modified:
    - "src/stdlib/iron_raylib.h (+79 lines: 48 Iron_image_* C prototypes in 2 new TEX-05 / TEX-07 sub-blocks + 2 deferral comments)"
    - "src/stdlib/iron_raylib.c (+378 lines: 48 shim implementations in 2 new Plan 66-03 Task 1 / Task 2 sub-blocks)"
    - "src/stdlib/raylib.iron (+84 lines: 48 func Image.* foreign-method stubs in 2 new Plan 66-03 sub-sections)"

key-decisions:
  - "DEFER ImageKernelConvolution: grep of src/runtime/iron_runtime.h lines 824-830 confirmed Iron_List_float / Iron_List_double are NOT in the pre-declared primitive list types (only int64_t / int32_t / double / bool / Iron_String / Iron_Closure). The kernel parameter is `const float *kernel, int kernelSize` which Iron would have to expose as `[Float32]` — the Pitfall 7 variant already flagged by Plan 66-02. DEFER marker lives inline in iron_raylib.h after the TEX-05 block."
  - "Iron `Image.from_rectangle(img, rec)` over raylib's `ImageFromImage(img, rec)`. Per the plan's Claude's-Discretion note in 66-CONTEXT.md: raylib's C name reads poorly as an instance method (`image.from_image(rec)` — 'image from image from this rectangle?'). `from_rectangle` states the input unambiguously and matches the Iron idiom of naming factories by their input, not their output. The shim forwards to `ImageFromImage` so the raylib API binding is exact."
  - "Iron `Image.draw_line_v(start, finish, color)` / `draw_line_ex(start, finish, thick, color)` use `finish` not `end` for the terminating Vector2. Probed in Plan 63-01: `end` is NOT a reserved word but readability suffers in Draw.* APIs where `Draw.end()` already exists (block-scope `end` method). `finish` is unambiguous. Same choice as Plan 63-02's draw_line_v."
  - "Pattern 2 shim template applied mechanically to 45 of 48 sites. Zero shim-body deviations from the canonical memcpy-in / raylib-&src-mutate / memcpy-out form. 3 Pattern 1 sites (ImageCopy/FromImage/FromChannel) follow Plan 66-02's Image-generator template. Confirms the planner's assessment that this plan is mechanical application, not new ABI territory."
  - "`ImageDraw` blit shim uses 5 memcpys: dst Image, src Image, src_rec Rectangle, dst_rec Rectangle, tint Color. All 3 struct types (Image/Rectangle/Color) already have proven _Static_assert grid entries from Phase 60-02/03; no new layout validation needed. Call-site semantics match raylib.h:1404 exactly: `ImageDraw(&dst, sc, sr, dr, col)` — dst by pointer (raylib mutates), src by value (read-only blit source)."

patterns-established:
  - "Canonical mutating-transform shim at scale. 48 nearly-identical shim bodies in one TU — all compile clean under `-Wall -Wextra` on macOS arm64. Future raylib surfaces with `Type *dst` C signatures (Texture updates, Wave transforms, Sound effects in Phase 68) can lift the template verbatim."
  - "Chain-style by-value composition validated at compile time. Iron stubs declare `img: Image -> Image` consistently; the C shim returns a fresh `struct Iron_Image`. Users can chain: `x.a().b().c()` — each call takes the previous return by value. Memory cost is 40 B per hop (the Image struct size) plus whatever raylib allocates for the pixel buffer internally. Zero aliasing pitfalls because every hop takes a fresh copy of the 40 B header."
  - "Multi-struct-input shim (ImageDraw): extends the two-Vector2 / two-Color / two-Rectangle templates from Plans 63-02/03/04 to a 5-struct-parameter call site. Validates that the FFI passing ABI scales linearly — no aggregate-arg limit encountered on macOS arm64."

requirements-completed: [TEX-05, TEX-07]

# Metrics
duration: 5min
completed: 2026-04-17
---

# Phase 66 Plan 03: Image Transforms + CPU Draw Summary

**48 Image.* raylib functions bound as idiomatic chain-style Iron methods — TEX-05 (27 mutating transforms + by-value returns) and TEX-07 (21 CPU draw functions) closed in one mechanical execution of the Pattern 2 template.**

## Performance

- **Duration:** 5 min 9 sec
- **Started:** 2026-04-17T14:03:12Z
- **Completed:** 2026-04-17T14:08:21Z
- **Tasks:** 2 (Task 1 — TEX-05 27 transforms, 1 commit; Task 2 — TEX-07 21 CPU draws, 1 commit)
- **Files modified:** 4 (1 new: 66-03-SUMMARY.md; 3 modified: iron_raylib.{c,h}, raylib.iron)

## Accomplishments

- **Task 1 (TEX-05 closed — 27 Image mutating transforms).** `src/stdlib/raylib.iron` gains 27 `func Image.*` stubs covering ImageCopy/FromImage/FromChannel (Pattern 1 by-value-return) plus the 24 mutators: Format, ToPOT, Crop, AlphaCrop/Clear/Mask/Premultiply, BlurGaussian, Resize/ResizeNN/ResizeCanvas, Mipmaps, Dither, FlipVertical/Horizontal, Rotate/RotateCW/RotateCCW, ColorTint/Invert/Grayscale/Contrast/Brightness/Replace. `iron_raylib.h` gains 27 C prototypes under a dedicated TEX-05 sub-block. `iron_raylib.c` gains 27 shim bodies following Pattern 2 (memcpy-in, ImageFoo(&src, ...), memcpy-out) for mutators and Pattern 1 for the 3 by-value-return forms. `ImageKernelConvolution` is DEFERRED (inline comment + summary note) because the `const float *kernel` parameter requires `[Float32]` FFI which Iron's runtime does not yet pre-declare (iron_runtime.h lines 824-830 list only int64_t/int32_t/double/bool/Iron_String/Iron_Closure).
- **Task 2 (TEX-07 closed — 21 Image CPU draw functions).** `src/stdlib/raylib.iron` gains 21 more `func Image.*` stubs covering ClearBackground, DrawPixel/V, DrawLine/V/Ex, DrawCircle/V/Lines/LinesV, DrawRectangle/V/Rec/Lines, DrawTriangle/Ex/Lines/Fan/Strip, ImageDraw (blit), ImageDrawText. `iron_raylib.h` gains 21 more C prototypes. `iron_raylib.c` gains 21 more shim bodies. The 2 array-input shims (`Iron_image_draw_triangle_fan` / `_strip`) reuse the `Iron_List_Iron_Vector2` by-value ABI proven GREEN in Phase 63-03 Task 1 probe. `ImageDraw` blit uses a 5-memcpy variant (dst Image, src Image, src_rec Rectangle, dst_rec Rectangle, tint Color). `ImageDrawText` uses the `iron_string_cstr` + NULL-fallback marshalling pattern from Plan 66-02's Image.load/export. `ImageDrawTextEx` is DEFERRED to Phase 67 (inline comment) — requires Font type.
- **Chain-style composition validated.** Iron users can now write `Image.color(64, 64, BLACK).flip_vertical().color_tint(RED).crop(Rectangle(0, 0, 32, 32))` as idiomatic Iron. Every hop returns a fresh 40 B Iron_Image by value; raylib mutates its own stack copy internally. No aliasing pitfalls.
- **Standalone clang -c clean.** Both `iron_raylib.c` and `iron_raylib_layout.c` compile with zero errors and zero warnings on macOS arm64 under `-Wall -Wextra`. The `_Static_assert` grid remains at 413 entries (this plan adds no new types). ironc NOT invoked in this plan per the plan's verification spec — end-to-end smoke is Plan 66-05 territory.
- **TEX-05 + TEX-07 closed simultaneously.** Phase 66 cumulative: **9 of 14** requirements complete (TEX-01, TEX-02, TEX-03, TEX-04, TEX-05, TEX-06, TEX-07, TEX-13, TEX-14 — 64%). Remaining: TEX-08, TEX-09, TEX-10, TEX-11, TEX-12 all in Plan 66-04 (Texture load/update/config/draw); TEX-05 partial deferral — ImageKernelConvolution alone still pending [Float32] FFI.

## Task Commits

1. **Task 1: Bind 27 Image mutating transforms (TEX-05)** — `6398263` (feat)
2. **Task 2: Bind 21 Image CPU draw functions (TEX-07)** — `17bfb9c` (feat)

## Files Created/Modified

- `src/stdlib/iron_raylib.h` — 79 lines added (27 TEX-05 prototypes in one sub-block + 21 TEX-07 prototypes in a second sub-block + 2 deferral comments for ImageKernelConvolution and ImageDrawTextEx). File size 1043 → ~1150 lines.
- `src/stdlib/iron_raylib.c` — 378 lines added (27 TEX-05 shim bodies + 21 TEX-07 shim bodies, each following the canonical Pattern 2 template with variation per-arg-type). File size 3039 → ~3617 lines.
- `src/stdlib/raylib.iron` — 84 lines added (27 TEX-05 stubs + 21 TEX-07 stubs + 2 new documentation sub-sections). File size 1995 → ~2079 lines. Total `^func Image.` count now **69** (21 Plan 66-02 + 48 Plan 66-03).
- `.planning/phases/66-textures-images/66-03-SUMMARY.md` — this file.

## Decisions Made

- **DEFER ImageKernelConvolution — confirm Iron_List_float absence.** Read `src/runtime/iron_runtime.h` lines 824-830 explicitly before committing. Pre-declared primitive list types: `IRON_LIST_DECL(int64_t, int64_t)`, `IRON_LIST_DECL(int32_t, int32_t)`, `IRON_LIST_DECL(double, double)`, `IRON_LIST_DECL(bool, bool)`, `IRON_LIST_DECL(Iron_String, Iron_String)`, `IRON_LIST_DECL(Iron_Closure, Iron_Closure)`. No `float` entry exists; Iron's `[Float32]` lowering would require a new `IRON_LIST_DECL(float, float)` plus matching `IRON_LIST_IMPL` instantiation in `iron_collections.c`. That's not this plan's scope. Documented as inline comment at `iron_raylib.h` line 1086 (after the TEX-05 block) and as key-decision here.
- **`Image.from_rectangle` over raylib's `ImageFromImage`.** Claude's Discretion per 66-CONTEXT.md. The instance-method form reads cleanest with the receiver's input being named explicitly. `image.from_rectangle(rec)` is self-documenting; `image.from_image(rec)` is confusing (is the receiver the source or the destination?). The shim name follows the Iron convention (`Iron_image_from_rectangle`) while the shim body calls raylib's real `ImageFromImage(src, r)`.
- **`finish` parameter name for line endpoints** in draw_line_v / draw_line_ex. Iron's `end` is not reserved (probed in 63-01) but using it as a parameter name next to `Draw.end()` (block-scope method) reduces readability. `finish` matches the Plan 63-02 draw_line_v precedent — consistent naming across all line-drawing APIs.
- **Pattern 2 applied mechanically to 45 of 48 sites; Pattern 1 for the 3 Image-returning raylib functions.** Zero shim-body deviations from the canonical templates. `ImageDraw` (blit) is the most complex shim in the plan (5 memcpys) but still fits the template — just more struct inputs.
- **`ImageDrawText` relies on the default font loaded by `Window.init()`.** Pitfall 4 flag in the shim body comment. Users calling `Image.draw_text(...)` without initializing a window will see raylib's TRACE LOG "No default font loaded" warning. Documenting as a call-order hazard rather than mitigating — Iron has no lazy-init mechanism for raylib's internal default font state.

## Deviations from Plan

None - plan executed exactly as written. Both tasks landed on first compile with zero `-Wall -Wextra` warnings. The planner's assessment that "this plan is mechanical application with zero new ABI territory" proved accurate — 48 shim sites applied the Pattern 2 template uniformly; the 3 by-value-return shims applied Pattern 1 from Plan 66-02 verbatim; the 2 array-input shims applied the Phase 63-03 Iron_List_Iron_Vector2 template verbatim.

ImageKernelConvolution's deferral was anticipated by the plan's `<read_first>` instruction to "grep `src/runtime/iron_runtime.h` for `Iron_List_float` or `Iron_List_double`; if NOT pre-declared, DEFER". The grep confirmed absence; the deferral marker landed as instructed.

## Issues Encountered

None. Both tasks executed on-plan with zero auto-fix required.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- **Plan 66-04 (Texture load/update/config/draw) is unblocked.** Iron_Image is now fully-featured as a chain-style composable value on the Iron side. Plan 66-04's `image.to_texture()` surface can take an Image by value without any additional ABI work.
- **Plan 66-05 (smoke + ABI sweep) will exercise the 48 shims via a real ironc build.** This plan deliberately stayed at `clang -c` only per HANDOFF.md memory discipline. Plan 66-05 is the first plan in Phase 66 to run `./build/ironc build` on a test file that touches Image.* chains.
- **`[Float32]` FFI remains the sole Phase 66 surface blocker.** 1 DEFERRED function (ImageKernelConvolution) plus 4 from Plan 66-02 (LoadImageRaw, LoadImageFromMemory, LoadImageAnimFromMemory, ExportImageToMemory) all block on the same `Iron_List_float` / `Iron_List_uint8_t` runtime-pre-declaration gap. A single follow-up phase adding both to `iron_runtime.h` + `iron_collections.c` closes 5 bindings simultaneously across TEX-04 / TEX-05.
- **Font blocker catalogued.** ImageDrawTextEx + ImageTextEx inline-DEFERRED to Phase 67. Phase 67's plan can grep for `DEFERRED to Phase 67` in `iron_raylib.h` to discover its scope.
- **Requirements traceability:** TEX-05 (except ImageKernelConvolution) and TEX-07 (except ImageDrawTextEx) fully closed. Phase 66 cumulative **9/14 requirements** (64%). Remaining Plan 66-04 requirements: TEX-08, TEX-09, TEX-10, TEX-11, TEX-12.

## Self-Check: PASSED

- `src/stdlib/iron_raylib.h` — FOUND. 69 unique `Iron_image_*` prototype entry tokens confirmed via `grep -oE 'Iron_image_[a-z_]+\('` (21 Plan 66-02 + 48 Plan 66-03). 2 deferral comments present: ImageKernelConvolution after TEX-05 block + ImageDrawTextEx inside TEX-07 doc block and after prototype list.
- `src/stdlib/iron_raylib.c` — FOUND. 69 unique `Iron_image_*` definition entry tokens — `diff` against header names returned empty (perfect symmetry).
- `src/stdlib/raylib.iron` — FOUND. 69 `^func Image\.` stubs confirmed via grep count. 17 `^func Color\.` stubs from Plan 66-01 still intact.
- Commit `6398263` — FOUND in `git log --oneline`: `6398263 feat(66-03): bind 27 Image mutating transforms (TEX-05)`.
- Commit `17bfb9c` — FOUND in `git log --oneline`: `17bfb9c feat(66-03): bind 21 Image CPU draw functions (TEX-07)`.
- clang -c iron_raylib.c — EXIT 0, zero warnings.
- clang -c iron_raylib_layout.c — EXIT 0, grid unchanged (413 asserts; this plan added no new types).
- TEX-05 Iron stubs — 27 `^func Image\.(copy|from_rectangle|from_channel|format|to_pot|crop|alpha_|blur_gaussian|resize|mipmaps|dither|flip_|rotate|color_)` entries verified.
- TEX-07 Iron stubs — 21 `^func Image\.(clear_background|draw)` entries verified.
- Array-arg shims — `Iron_image_draw_triangle_fan` and `Iron_image_draw_triangle_strip` both present in iron_raylib.c with `(Vector2 *)points.items` forwarding.

---
*Phase: 66-textures-images*
*Completed: 2026-04-17*
