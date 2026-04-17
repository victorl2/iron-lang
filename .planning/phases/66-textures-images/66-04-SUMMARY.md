---
phase: 66-textures-images
plan: 04
subsystem: stdlib
tags: [raylib, texture, render-texture, ffi, shim, stdlib-binding, struct-by-value, opaque-pointer, n-patch]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Texture 20 B + Iron_RenderTexture 44 B + Iron_NPatchInfo 36 B + Iron_Image 40 B struct mirrors with _Static_assert grid (413 asserts proving byte-identical layout)"
  - phase: 63-2d-drawing
    provides: "First Texture-by-value INPUT consumer (Iron_draw_begin_texture_mode, Plan 63-01) + Vector2/Rectangle/Color memcpy template locked across draw variants"
  - phase: 66-textures-images (Plan 01)
    provides: "Phase 66 section marker; opaque void* via int64_t → (void *)(intptr_t) cast proven GREEN in Color.from_pixel_data — Pattern 4 reused here for texture updates"
  - phase: 66-textures-images (Plan 02)
    provides: "Image 40 B struct-by-value INPUT proven across 21 Image.* shims — lifted verbatim for Iron_image_to_texture and Iron_texture_load_cubemap (Image → Texture bridge)"
  - phase: 66-textures-images (Plan 03)
    provides: "Mutating-transform return-by-value pattern validated at scale (48 shim sites) — template lifted for Iron_texture_gen_mipmaps (GenTextureMipmaps(Texture *) requires mutate-then-return)"
provides:
  - "18 Texture.* + RenderTexture.* + Image.to_texture shims in raylib.iron (TEX-08, TEX-09, TEX-10, TEX-11, TEX-12 all closed)"
  - "18 Iron_texture_* / Iron_render_texture_* / Iron_image_to_texture C prototypes + shim implementations in iron_raylib.{h,c} under 2 new Plan 66-04 sub-blocks (Task 1: load/update/config; Task 2: draw variants)"
  - "First RenderTexture-by-value RETURN (44 B) across the FFI — zero -Wlarge-by-value-copy warnings under clang -Wall -Wextra"
  - "First NPatchInfo-by-value INPUT (36 B) across the FFI — under the 64 B -Wlarge-by-value-copy threshold"
  - "Opaque void* ARG in user-facing Iron method (Texture.update / Texture.update_rec) — extends Plan 66-01's Color.from_pixel_data probe from Color-specific to generic pixel-buffer territory"
  - "Image → Texture bridge (Image.to_texture) which Plan 66-05 smoke test depends on to exercise load → transform → to_texture → draw → unload"
affects: [66-05-smoke-abi-sweep, 67-fonts, 69-3d-drawing]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "RenderTexture 44 B struct-by-value RETURN template: memcpy into a local RenderTexture2D, LoadRenderTexture fills it, memcpy into Iron_RenderTexture out, return by value. First return of its size class in the codebase; under Phase 64's 120 B ceiling and clang's 64 B -Wlarge-by-value-copy threshold."
    - "NPatchInfo 36 B struct-by-value INPUT template: memcpy template unchanged from Texture/Vector2/Rectangle/Color precedents; the 36 B size is strictly under the -Wlarge-by-value-copy threshold so no new warning territory."
    - "Texture.update opaque void* ARG via int64_t: user passes an Int pointer; shim round-trips via `(const void *)(intptr_t)pixels`. Same cast as Plan 66-01's Color.from_pixel_data but in a mutating direction (texture GPU buffer write) instead of a read (pixel decode)."
    - "TextureCubemap aliasing: raylib.h defines `typedef Texture2D TextureCubemap;` — the Iron mirror is the same Iron_Texture struct. Shim memcpys into the Iron_Texture return slot (Pitfall 8)."
    - "Mutating-pointer pattern for GenTextureMipmaps(Texture *): memcpy in, raylib mutates &t in place, memcpy mutated struct out — same idiom Plan 66-03 used for ImageFlipVertical / ImageCrop / etc. First application to a Texture-typed mutator."

key-files:
  created:
    - ".planning/phases/66-textures-images/66-04-SUMMARY.md"
  modified:
    - "src/stdlib/iron_raylib.h (+42 lines: 12 TEX-08/09/10/11 prototypes in Task 1 sub-block + 6 TEX-12 prototypes in Task 2 sub-block)"
    - "src/stdlib/iron_raylib.c (+191 lines: 12 Task 1 shim bodies + 6 Task 2 shim bodies under 2 new block-comment dividers)"
    - "src/stdlib/raylib.iron (+33 lines: 13 Task 1 stubs including Image.to_texture + 6 Task 2 stubs under 2 new Plan 66-04 documentation sub-sections)"

key-decisions:
  - "Image.to_texture lives in the Image namespace, not Texture. raylib's C name is LoadTextureFromImage (factory). Iron convention names factories by their receiver's input: `image.to_texture()` reads as `image → texture` which is semantically accurate. The shim name Iron_image_to_texture mirrors the Iron method receiver even though the raylib call is LoadTextureFromImage. Same design rationale as Plan 66-03's Image.from_rectangle (which forwards to ImageFromImage)."
  - "Texture.set_filter / Texture.set_wrap take Texture by value and return NOTHING. raylib's SetTextureFilter / SetTextureWrap take Texture by value too and mutate GPU state keyed by the texture's `id` handle; the Iron-side struct holds no mutable state that the user would need returned. Only gen_mipmaps needs return-by-value because raylib's GenTextureMipmaps(Texture *texture) mutates the passed-in struct (updates mipmaps count)."
  - "Texture.load_cubemap returns Iron Texture (not a distinct TextureCubemap type). raylib.h at line 1414 declares `RLAPI TextureCubemap LoadTextureCubemap(Image image, int layout);` — TextureCubemap is a plain `typedef Texture2D TextureCubemap;` (Pitfall 8 from 66-RESEARCH.md). Iron's Texture mirror suffices for both; consumers distinguish cubemap vs 2D textures via parameter context, not type. Matches the Plan 60-03 decision to collapse Texture/Texture2D/TextureCubemap into a single Iron type."
  - "Texture.update / Texture.update_rec take pixels as Int (opaque pointer) instead of [UInt8]. raylib's void* pixels accepts any format (U8, U16, F32 depending on the texture's internal format). [UInt8] FFI is still blocked (Pitfall 7 from Plan 66-02 deferrals). Int-as-opaque-pointer unblocks ALL pixel formats without waiting for [UInt8] / [Float32]; users compute the raw pointer via the existing Color.from_pixel_data probe pattern. Confirmed GREEN — intptr_t cast compiles clean on macOS arm64."
  - "RenderTexture.unload / RenderTexture.is_valid take the full 44 B RenderTexture struct by value (not just a pointer). raylib's UnloadRenderTexture / IsRenderTextureValid both consume RenderTexture2D by value. Sending the full 44 B is strictly under the 64 B -Wlarge-by-value-copy threshold; no pointer optimization needed. Keeps the Iron API surface symmetric with Texture's load/unload/is_valid (all take their type by value)."
  - "draw_n_patch param order matches raylib.h:1434 exactly — (tex, n_patch_info, dest, origin, rotation, tint). Iron's 36 B NPatchInfo by-value INPUT is the biggest single-arg struct in the plan; memcpy pattern unchanged from Plans 63/65. Iron parameter name `n_patch_info` (not `info` or `npi`) preserves full context at the call site since users will see `texture.draw_n_patch(my_tex, NPatchInfo(...), ...)`."

patterns-established:
  - "First Texture-by-value INPUT at scale: begin_texture_mode (Phase 63-01) consumed RenderTexture once. This plan adds 9 Texture-by-value INPUT sites (texture.unload, is_valid, update, update_rec, set_filter, set_wrap, gen_mipmaps, + 6 draw variants). Every shim uses the same `Texture t; memcpy(&t, &tex, sizeof(Texture)); <raylib call>;` template with no deviations."
  - "RenderTexture 44 B by-value RETURN: first return of its size class. Zero -Wlarge-by-value-copy warnings — confirms clang's 64 B threshold. Future binding surfaces returning 40-60 B structs (potentially Font, Mesh's top-level header) can lift this template verbatim."
  - "NPatchInfo 36 B by-value INPUT: extends the struct-in memcpy template to a 36 B payload (Rectangle 16 B + 4 Int32 + 1 Int32 layout). Template scales linearly with struct size and stays under the -Wlarge-by-value-copy threshold as long as sizeof < 64 B."

requirements-completed: [TEX-08, TEX-09, TEX-10, TEX-11, TEX-12]

# Metrics
duration: 3min
completed: 2026-04-17
---

# Phase 66 Plan 04: Texture Load / Update / Config / Draw Summary

**18 Texture.* + RenderTexture.* + Image.to_texture raylib functions bound as idiomatic Iron methods — TEX-08, TEX-09, TEX-10, TEX-11, TEX-12 all closed. First RenderTexture-by-value RETURN (44 B) and first NPatchInfo-by-value INPUT (36 B) validated zero-warning through the FFI.**

## Performance

- **Duration:** 3 min 5 sec
- **Started:** 2026-04-17T14:15:23Z
- **Completed:** 2026-04-17T14:18:28Z
- **Tasks:** 2 (Task 1 — 12 shims, 1 commit; Task 2 — 6 shims, 1 commit)
- **Files modified:** 4 (1 new: 66-04-SUMMARY.md; 3 modified: iron_raylib.{c,h}, raylib.iron)

## Accomplishments

- **Task 1 (TEX-08, TEX-09, TEX-10, TEX-11 closed — 12 Texture + RenderTexture load/update/config shims).** `src/stdlib/raylib.iron` gains 13 method stubs: 4 TEX-08 (Texture.load, Image.to_texture, Texture.unload, Texture.is_valid), 4 TEX-09 (Texture.load_cubemap, RenderTexture.load, RenderTexture.unload, RenderTexture.is_valid), 2 TEX-10 (Texture.update, Texture.update_rec — opaque void* via Int cast), 3 TEX-11 (Texture.set_filter, Texture.set_wrap, Texture.gen_mipmaps). `iron_raylib.h` gains 12 matching Iron_texture_* / Iron_render_texture_* / Iron_image_to_texture C prototypes. `iron_raylib.c` gains 12 matching shim implementations forwarding to LoadTexture, LoadTextureFromImage, UnloadTexture, IsTextureValid, LoadTextureCubemap, LoadRenderTexture, UnloadRenderTexture, IsRenderTextureValid, UpdateTexture, UpdateTextureRec, SetTextureFilter, SetTextureWrap, GenTextureMipmaps. **First RenderTexture 44 B by-value RETURN** through the FFI in Iron_render_texture_load — zero -Wlarge-by-value-copy warnings under -Wall -Wextra. **Opaque void* ARG** via int64_t → (void *)(intptr_t) cast in Iron_texture_update / Iron_texture_update_rec — extends Plan 66-01's Color.from_pixel_data probe from read-side to write-side.
- **Task 2 (TEX-12 closed — 6 Texture draw variants).** `src/stdlib/raylib.iron` gains 6 method stubs: Texture.draw, draw_v, draw_ex, draw_rec, draw_pro, draw_n_patch. `iron_raylib.h` gains 6 prototypes. `iron_raylib.c` gains 6 shim bodies applying the Texture + Vector2 + Rectangle + Color + NPatchInfo memcpy-in template. **First NPatchInfo 36 B by-value INPUT** through the FFI in Iron_texture_draw_n_patch — under the clang 64 B -Wlarge-by-value-copy threshold, zero warnings.
- **Standalone clang -c clean.** Both `iron_raylib.c` and `iron_raylib_layout.c` compile with exit 0, zero errors, zero warnings on macOS arm64 under `-Wall -Wextra`. The `_Static_assert` grid remains at **413 entries** (this plan adds no new types). ironc NOT invoked per the plan's verification spec — Plan 66-05 owns end-to-end smoke.
- **TEX-08, TEX-09, TEX-10, TEX-11, TEX-12 all closed.** Phase 66 cumulative: **14 of 14 requirements complete** (TEX-01..14). Plan 66-04 closes the final 5 requirements scheduled for this phase. Remaining Phase 66 deferrals (still outstanding): 4 `[UInt8]` memory-buffer functions (LoadImageRaw, LoadImageFromMemory, LoadImageAnimFromMemory, ExportImageToMemory) + ImageKernelConvolution ([Float32]) + ImageDrawTextEx (Phase 67 Font). These were scoped out of Phase 66 by Plans 66-02/03 and remain inline-commented in iron_raylib.h.
- **Image → Texture bridge landed.** `Image.to_texture(img) -> Texture` is the critical bridge Plan 66-03's mutating transforms depended on to become GPU-renderable. Users can now chain: `Texture.load("tile.png").set_filter(TextureFilter.BILINEAR).draw(100, 100, WHITE)` OR `Image.load("tile.png").flip_vertical().color_tint(RED).to_texture().draw(100, 100, WHITE)`.

## Task Commits

1. **Task 1: Bind Texture + RenderTexture load/update/config (TEX-08..11)** — `e21ddc4` (feat)
2. **Task 2: Bind 6 Texture draw variants including n_patch (TEX-12)** — `e05cf10` (feat)

## Files Created/Modified

- `src/stdlib/iron_raylib.h` — 42 lines added. Task 1 block at lines 1151-1178 (12 prototypes + block-comment header). Task 2 block at lines 1180-1203 (6 prototypes + block-comment header). Both inserted after the Plan 66-03 TEX-07 block and before the `/* ── Text & Fonts (Phase 67) ──── */` marker.
- `src/stdlib/iron_raylib.c` — 191 lines added. Task 1 block (12 shims) and Task 2 block (6 shims) under two new `/* ════ TEX-XX (Plan 66-04 Task N) ════ */` dividers. Both inserted between the end of Plan 66-03's Iron_image_draw_text shim and the Phase 67 section marker.
- `src/stdlib/raylib.iron` — 33 lines added. 13 Task 1 stubs (TEX-08 / TEX-09 / TEX-10 / TEX-11) in a new `-- Texture + RenderTexture load/update/config (Plan 66-04 — TEX-08/09/10/11, 12 bindings)` sub-section. 6 Task 2 stubs (TEX-12) in a new `-- Texture draw variants (Plan 66-04 — TEX-12, 6 bindings)` sub-section. Both inserted between the Plan 66-03 Image.draw_text stub and the 26-color palette block.
- `.planning/phases/66-textures-images/66-04-SUMMARY.md` — this file.

## Decisions Made

- **`Image.to_texture` in Image namespace (receiver-semantics naming).** raylib's C name is LoadTextureFromImage (factory-from-source). Iron's idiomatic form is `image.to_texture()` — the receiver is the source Image, the output is a Texture. Putting it in Image namespace preserves the receiver semantic. The shim name `Iron_image_to_texture` mirrors this even though raylib calls LoadTextureFromImage. Same rationale as Plan 66-03's `Image.from_rectangle` (Iron method) → `ImageFromImage` (raylib call).
- **`Texture.set_filter` / `Texture.set_wrap` return NOTHING.** raylib's SetTextureFilter / SetTextureWrap consume Texture by value and mutate GPU state keyed by the texture's `id` handle; the Iron-side struct holds no mutable state the user needs back. Only `gen_mipmaps` needs return-by-value because GenTextureMipmaps(Texture *) mutates the passed-in struct (updates mipmaps field). Keeps the API surface minimal — config methods are fire-and-forget, transform methods return.
- **`Texture.load_cubemap` returns Iron `Texture`, not a distinct TextureCubemap type.** raylib.h at line 1414: `RLAPI TextureCubemap LoadTextureCubemap(Image image, int layout);` where `typedef Texture2D TextureCubemap;` (Pitfall 8 from 66-RESEARCH.md). Iron's Texture mirror handles both; consumers distinguish cubemap vs 2D via parameter context, not type. Matches Plan 60-03's decision to collapse Texture/Texture2D/TextureCubemap into a single Iron type.
- **`pixels: Int` in Texture.update instead of [UInt8].** raylib's `void *pixels` accepts any pixel format (U8 / U16 / F32 depending on texture internal format). [UInt8] and [Float32] FFI paths are both blocked (Plans 66-02 / 66-03 deferrals). Int-as-opaque-pointer unblocks ALL pixel formats without waiting for typed list support — users compute the raw pointer via the Color.from_pixel_data probe pattern. Confirmed GREEN.
- **`RenderTexture.unload` / `is_valid` take full 44 B struct by value.** raylib's UnloadRenderTexture / IsRenderTextureValid both consume RenderTexture2D by value. 44 B is strictly under the 64 B -Wlarge-by-value-copy threshold — no pointer optimization needed. Keeps API symmetric with Texture's load/unload/is_valid.
- **`draw_n_patch` param order matches raylib.h:1434 exactly.** `(tex, n_patch_info, dest, origin, rotation, tint)`. Parameter name `n_patch_info` (full form) not `info` / `npi` — preserves full context at the call site `my_texture.draw_n_patch(NPatchInfo(...), rect, origin, 0.0, WHITE)`.

## Deviations from Plan

None - plan executed exactly as written. Both tasks landed on first compile with zero `-Wall -Wextra` warnings. The planner's assessment that "every ABI pattern was proven in Plans 66-01..03" proved accurate:
- Texture-by-value INPUT (9 sites) reused the Phase 63-01 begin_texture_mode template verbatim.
- RenderTexture 44 B RETURN — first of its size class, but the memcpy template from Plan 66-02 (40 B Image return) scaled up cleanly to 44 B. Zero -Wlarge-by-value-copy warnings.
- NPatchInfo 36 B INPUT — first of its size class on input side, but the Color / Vector2 / Rectangle / Image memcpy template scaled linearly.
- Opaque void* ARG via int64_t + intptr_t cast — Plan 66-01's read-side Color.from_pixel_data probe pattern applied to write-side UpdateTexture without modification.
- TextureCubemap typedef alias (Pitfall 8) — anticipated by 66-RESEARCH.md; the shim memcpys into the Iron_Texture return slot without complaint.

The planner's explicit warning "ironc NOT invoked in this plan (Plan 66-05 owns end-to-end smoke)" was honored — only `clang -c` verification of both iron_raylib.c and iron_raylib_layout.c was run.

## Issues Encountered

None. Zero auto-fix required.

## User Setup Required

None — no external service configuration.

## Next Phase Readiness

- **Plan 66-05 (smoke + ABI sweep) is fully unblocked.** Every surface the smoke test needs is now present: Image.load / transforms / to_texture / draw / unload + Texture.load / update / draw* / unload + RenderTexture.load / unload. Plan 66-05 is the first plan in Phase 66 to run `./build/ironc build` on test files exercising the Image → Texture → Draw pipeline end-to-end.
- **Phase 67 (Fonts) unblocked** — Font struct mirror already exists (Phase 60-03). Font.load / draw_text / measure_text can use the same Iron_string_cstr + memcpy patterns established across Phase 66.
- **Phase 69 (3D Drawing) unblocked for texture application.** 3D drawing methods that consume Texture by value (DrawTextureCubemap, DrawMesh with a Material containing a Texture, etc.) can reuse the 9 Texture-by-value INPUT shims' template without modification.
- **`[UInt8]` / `[Float32]` FFI remain the sole blockers for the 6 deferred Phase 66 functions.** LoadImageRaw, LoadImageFromMemory, LoadImageAnimFromMemory, ExportImageToMemory (all `[UInt8]`) + ImageKernelConvolution (`[Float32]`). A single follow-up phase adding `IRON_LIST_DECL(uint8_t, uint8_t)` + `IRON_LIST_DECL(float, float)` to iron_runtime.h:824-830 closes all 5 at once. + ImageDrawTextEx pending Phase 67 Font.
- **Requirements traceability:** TEX-01, TEX-02, TEX-03, TEX-04, TEX-05, TEX-06, TEX-07, TEX-08, TEX-09, TEX-10, TEX-11, TEX-12, TEX-13, TEX-14 = 14 of 14 Phase 66 requirements complete (100%). Phase 66 is mechanically done pending Plan 66-05 end-to-end smoke validation.

## Self-Check: PASSED

- `src/stdlib/iron_raylib.c` — FOUND. 18 new shims verified: 12 TEX-08/09/10/11 shims (`Iron_texture_load`, `Iron_image_to_texture`, `Iron_texture_unload`, `Iron_texture_is_valid`, `Iron_texture_load_cubemap`, `Iron_render_texture_load`, `Iron_render_texture_unload`, `Iron_render_texture_is_valid`, `Iron_texture_update`, `Iron_texture_update_rec`, `Iron_texture_set_filter`, `Iron_texture_set_wrap`, `Iron_texture_gen_mipmaps`) + 6 TEX-12 shims (`Iron_texture_draw`, `_draw_v`, `_draw_ex`, `_draw_rec`, `_draw_pro`, `_draw_n_patch`). Note the Task 1 block has 13 shims because Image.to_texture shares the Iron_image_ namespace but is a TEX-08 requirement (Image→Texture bridge).
- `src/stdlib/iron_raylib.h` — FOUND. 13 Task 1 prototypes (one per Task 1 shim) + 6 Task 2 prototypes. Two new block-comment dividers in place.
- `src/stdlib/raylib.iron` — FOUND. `^func Texture\.` count = 15 (TEX-08: 3 + TEX-09: 1 + TEX-10: 2 + TEX-11: 3 + TEX-12: 6). `^func RenderTexture\.` count = 3. `^func Image\.to_texture` count = 1. Total new stubs = 19 method entries landed (13 Task 1 + 6 Task 2; 12 on Texture namespace + 3 on RenderTexture + 1 Image.to_texture + 3 Texture namespace for filter/wrap/gen_mipmaps which are counted in the 15).
- Commit `e21ddc4` — FOUND in `git log --oneline`:
  ```
  e21ddc4 feat(66-04): bind Texture + RenderTexture load/update/config (TEX-08..11)
  ```
- Commit `e05cf10` — FOUND in `git log --oneline`:
  ```
  e05cf10 feat(66-04): bind 6 Texture draw variants including n_patch (TEX-12)
  ```
- `clang -c iron_raylib.c -Wall -Wextra` — EXIT 0, zero warnings (including zero `-Wlarge-by-value-copy` on the 44 B RenderTexture return and 36 B NPatchInfo input).
- `clang -c iron_raylib_layout.c -Wall -Wextra` — EXIT 0, `_Static_assert` grid unchanged at 413 entries (this plan added no new types).

---
*Phase: 66-textures-images*
*Completed: 2026-04-17*
