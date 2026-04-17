# Phase 66: Textures & Images - Context

**Gathered:** 2026-04-17
**Status:** Ready for planning
**Source:** Smart discuss (autonomous)

<domain>
## Phase Boundary

Bind all ~65 `rtextures.c` functions + the 26-color palette as idiomatic Iron methods on `Image` / `Texture` / `RenderTexture` / `Color` + module-level `val` color constants. Closes requirements **TEX-01..14**.

**Function-count breakdown (raylib 5.5):**
- **Image loading/unloading (TEX-01, TEX-02):** 10 — `LoadImage`, `LoadImageRaw`, `LoadImageAnim`, `LoadImageAnimFromMemory`, `LoadImageFromMemory`, `LoadImageFromTexture`, `LoadImageFromScreen`, `IsImageValid`, `UnloadImage`, `ExportImageToMemory`
- **Image generation (TEX-03):** 9 — `GenImageColor`, `GenImageGradientLinear`, `GenImageGradientRadial`, `GenImageGradientSquare`, `GenImageChecked`, `GenImageWhiteNoise`, `GenImagePerlinNoise`, `GenImageCellular`, `GenImageText`
- **Image export (TEX-04):** 2 — `ExportImage`, `ExportImageAsCode` (plus `ExportImageToMemory` above)
- **Image transforms in place (TEX-05):** 28 — `ImageFormat`, `ImageToPOT`, `ImageCrop`, `ImageAlphaCrop`, `ImageAlphaClear`, `ImageAlphaMask`, `ImageAlphaPremultiply`, `ImageBlurGaussian`, `ImageKernelConvolution`, `ImageResize`, `ImageResizeNN`, `ImageResizeCanvas`, `ImageMipmaps`, `ImageDither`, `ImageFlipVertical`, `ImageFlipHorizontal`, `ImageRotate`, `ImageRotateCW`, `ImageRotateCCW`, `ImageColorTint`, `ImageColorInvert`, `ImageColorGrayscale`, `ImageColorContrast`, `ImageColorBrightness`, `ImageColorReplace`, `ImageCopy`, `ImageFromImage`, `ImageFromChannel`, `ImageText`, `ImageTextEx`
- **Image data extraction (TEX-06):** 4 — `LoadImageColors`, `LoadImagePalette`, `UnloadImageColors`, `UnloadImagePalette`, `GetImageAlphaBorder`, `GetImageColor`
- **Image CPU draw (TEX-07):** 12 — `ImageClearBackground`, `ImageDrawPixel`, `ImageDrawPixelV`, `ImageDrawLine`, `ImageDrawLineV`, `ImageDrawLineEx`, `ImageDrawCircle`, `ImageDrawCircleV`, `ImageDrawCircleLines`, `ImageDrawCircleLinesV`, `ImageDrawRectangle`, `ImageDrawRectangleV`, `ImageDrawRectangleRec`, `ImageDrawRectangleLines`, `ImageDrawTriangle`, `ImageDrawTriangleEx`, `ImageDrawTriangleLines`, `ImageDrawTriangleFan`, `ImageDrawTriangleStrip`, `ImageDraw`, `ImageDrawText`, `ImageDrawTextEx`
- **Texture loading (TEX-08, TEX-09):** 7 — `LoadTexture`, `LoadTextureFromImage`, `LoadTextureCubemap`, `LoadRenderTexture`, `IsTextureValid`, `IsRenderTextureValid`, `UnloadTexture`, `UnloadRenderTexture`
- **Texture update (TEX-10):** 2 — `UpdateTexture`, `UpdateTextureRec`
- **Texture config (TEX-11):** 3 — `SetTextureFilter`, `SetTextureWrap`, `GenTextureMipmaps`
- **Texture draw variants (TEX-12):** 6 — `DrawTexture`, `DrawTextureV`, `DrawTextureEx`, `DrawTextureRec`, `DrawTexturePro`, `DrawTextureNPatch`
- **Color math (TEX-13):** 18 — `ColorIsEqual`, `Fade`, `ColorToInt`, `ColorNormalize`, `ColorFromNormalized`, `ColorToHSV`, `ColorFromHSV`, `ColorTint`, `ColorBrightness`, `ColorContrast`, `ColorAlpha`, `ColorAlphaBlend`, `ColorLerp`, `GetColor`, `GetPixelColor`, `SetPixelColor`, `GetPixelDataSize`
- **Color palette constants (TEX-14):** 26 `val` module-level constants (LIGHTGRAY through RAYWHITE)

**Approximate total:** ~100 functions + 26 constants. Larger than Phase 65 (143 but slimmer shims).

**In scope:** every RLAPI function in raylib.h's Texture/Image/Color section, plus the 26-color palette (replacing Plan 60-08's 6-color rescue palette).

**Out of scope:**
- Font/GlyphInfo wiring through textures — Phase 67 owns text even though Font embeds Texture2D.
- OpenGL/rlgl low-level texture calls — out of milestone per PROJECT.md.
- Custom pixel-format conversions beyond raylib's built-in `ImageFormat`.
- Image callbacks / streaming textures / video texture.
- New types. Image/Texture/RenderTexture/NPatchInfo/Color/PixelFormat/TextureFilter/TextureWrap/CubemapLayout all defined Phase 60-03/60-07.

</domain>

<decisions>
## Implementation Decisions

### Inherited from Phase 60/61/62/63/64/65 (locked — no discussion)

- **Binding architecture:** shim-only via `/* ── Textures (Phase 66) ──── */` section marker that Plan 60-01 scaffolded.
- **Method casing:** `snake_case`. `image.color_tint(tint)`, `texture.set_filter(.bilinear)`, `Image.load("path.png")`, `color.to_hsv()`.
- **Method dispatch:** instance methods on data-carrying objects (Image, Texture, Color) use the pattern validated in Phase 64 (`hir_to_lir.c:1096-1297`). Static constructors (`Image.load`, `Texture.load`, `Color.from_hsv`) use namespace form validated across Phase 62-65.
- **`self` reserved** (E0101) — receivers use semantic names: `image`/`img`, `texture`/`tex`, `color`/`c`.
- **String arguments** — raylib's `const char *` path params are marshalled by Iron's compiler-managed `String → char *` (Phase 61 precedent, used ~50 times across later phases).
- **Float32 vs Float:** raylib uses `float` — Iron `Float32` everywhere.
- **Enum parameters:** typed — `PixelFormat`, `TextureFilter`, `TextureWrap`, `CubemapLayout`. Shim casts to `int` per Phase 62 convention.
- **Struct-by-value:** Image (40 B), Texture (20 B), RenderTexture (44 B), NPatchInfo (36 B), Color (4 B), Rectangle (16 B), Vector2 (8 B), Vector3 (12 B), Vector4/HSV (16 B). All straightforward memcpy per Phase 64's large-struct precedent.
- **Pointer/opaque handling:** Image has `void *data`, Texture has `unsigned int id`. Iron mirrors use `_data` (Int8 pointer) and `id` (UInt32) per Phase 60-03 convention.
- **Auto-generated C prototypes** via emit_c (commit e3e5eee).
- **Layout grid** pinned by Phase 60-03 (393 entries through Phase 65). No new types in Phase 66.
- **Memory discipline** ironc ~10 GB per invocation — 1-2 smoke builds per plan.

### Phase 66 specific

#### Method receiver + static-constructor mix — **ALL receivers (Image/Texture/Color) are data-carrying objects**

Phase 64 validated instance methods on data-carrying objects (Rectangle, Vector2). Phase 66 applies this to Image/Texture/RenderTexture/Color wholesale:

- **Static constructors:** `Image.load(path)`, `Image.from_memory(type, data, size)`, `Image.from_screen()`, `Image.color(w, h, color)`, `Image.gradient_linear(...)`, `Image.checked(...)`, `Image.text(text, size, color)`, `Texture.load(path)`, `Texture.from_image(image)`, `Texture.load_cubemap(image, layout)`, `RenderTexture.load(w, h)`, `Color.from_hsv(hsv)`, `Color.from_int(rgba)`, `Color.from_normalized(v)`, `Color.from_pixel_format(data, format)`.
- **Instance methods:** `image.crop(rec)`, `image.resize(w, h)`, `image.color_tint(tint)`, `image.draw_pixel(x, y, color)`, `image.to_texture()`, `image.unload()`, `image.export(path)`, `texture.update(pixels)`, `texture.set_filter(filter)`, `texture.draw(x, y, tint)`, `texture.draw_pro(source, dest, origin, rotation, tint)`, `texture.unload()`, `color.tint(other)`, `color.fade(alpha)`, `color.to_hsv()`, `color.alpha_blend(dst, src)`.

**Mutating transforms** (`ImageCrop`, `ImageResize`, `ImageFormat`, `ImageColorTint`, etc.) take `Image *` in raylib C. Iron side: since all args are by value, the shim pattern is:
```c
void Iron_image_crop(Iron_Image *self, Iron_Rectangle crop) {
    Image c; memcpy(&c, self, sizeof(Image));
    Rectangle r; memcpy(&r, &crop, sizeof(Rectangle));
    ImageCrop(&c, r);
    memcpy(self, &c, sizeof(Iron_Image));
}
```
But Iron's foreign-method stubs don't natively support `self: &mut Image` patterns. **Planner must choose:**
- **Option A:** Iron methods return a new `Image` by value (`image.crop(rec) -> Image`). Users reassign: `val new_img = old_img.crop(rec); old_img.unload()`. Simpler, idiomatic for immutable objects, but requires discipline around resource cleanup.
- **Option B:** Shim takes `self` by value and returns the mutated copy — same as Option A at the Iron level but slightly different C shim body (no `self` pointer). Recommended per CONTEXT.md Claude's Discretion: return-by-value. Skip the `&mut self` question — Iron doesn't have it.

**Lock Option A/B (semantically equivalent):** instance methods on Image return `Image` by value; users manage lifecycle explicitly.

**Caveat for image CPU draws (TEX-07):** `ImageDrawPixel(Image *dst, int x, int y, Color c)` mutates `dst` (adds a pixel). Pattern:
```iron
func Image.draw_pixel(self: Image, x: Int32, y: Int32, color: Color) -> Image {}
```
Returns the modified image. Users chain: `val canvas = Image.color(64, 64, BLACK).draw_pixel(10, 10, RED).draw_rectangle(20, 20, 30, 30, BLUE)`. Chain-style aligns with Iron idiom.

#### Color palette — **26 module-level `val` constants replacing Plan 60-08's 6-color rescue palette**

Plan 60-08 added 6 colors (RED, BLUE, GREEN, WHITE, BLACK, DARKGRAY) as temporary rescue. Phase 66 REPLACES the rescue block with the full 26-color raylib palette:
```iron
val LIGHTGRAY: Color = Color(UInt8(200), UInt8(200), UInt8(200), UInt8(255))
val GRAY: Color = Color(UInt8(130), UInt8(130), UInt8(130), UInt8(255))
val DARKGRAY: Color = Color(UInt8(80), UInt8(80), UInt8(80), UInt8(255))
-- (etc. for all 26)
val RAYWHITE: Color = Color(UInt8(245), UInt8(245), UInt8(245), UInt8(255))
```
RGBA values copied verbatim from raylib.h's `#define` macros (lines 212–240).

**Location in raylib.iron:** replaces the "Plan 60-08 rescue palette" block at end of file. Header comment changed from `-- PHASE 66 TEX-14 will replace this block wholesale` to `-- raylib 5.5 canonical 26-color palette (TEX-14 closed Phase 66)`.

#### Plan slicing — **5 plans**

- **Plan 66-01 — Color math + palette (TEX-13, TEX-14):** 18 Color methods + 26 palette `val` constants. Smallest plan; does NOT require Image/Texture work. Exercises Color-in/Color-out shims, HSV conversion (`Vector3` helper return), int round-tripping. Wave 1. Depends on nothing (independent sub-graph).
- **Plan 66-02 — Image load/gen/save/extract (TEX-01, TEX-02, TEX-03, TEX-04, TEX-06):** ~25 functions. Wave 2. First String-arg marshalling stress (path args, text-rendering args). First raw-memory ArgByref for `LoadImageRaw`, `LoadImageFromMemory`, etc. — these take `const unsigned char *` + length; Iron mirrors with `[UInt8]` array by value (Phase 63-04 `Iron_List_Iron_UInt8` pattern).
- **Plan 66-03 — Image transforms + CPU draw (TEX-05, TEX-07):** ~50 functions. Largest plan. Every `Image*` mutating transform (Crop, Resize, Rotate, ColorTint, etc.) + every `ImageDraw*` call. Wave 3. depends_on 66-02.
- **Plan 66-04 — Texture load + update + config + draw (TEX-08, TEX-09, TEX-10, TEX-11, TEX-12):** ~18 functions. Wave 4. depends_on 66-03 (needs `image.to_texture()` from 66-02/03 path). First cubemap + RenderTexture end-to-end.
- **Plan 66-05 — Consumer re-enablement + MATH-08 equivalent ABI sweep:** Consumer file pong.iron / game_raylib.iron / hello_raylib.iron have zero `-- PHASE 66:` markers (texture work was deferred). **Skip consumer re-enablement.** Instead: standalone `tests/manual/texture_smoke.iron` + final ABI-round-trip audit across all ~100 Phase 66 functions. Wave 5. depends_on 66-04.

### Claude's Discretion

- **Exact method names where raylib doesn't map cleanly to snake_case.** E.g., `ImageFromImage(image, rec)` → `image.from_image(rec)` reads poorly; rename to `image.from_rectangle(rec)` or keep as `image.copy_rec(rec)`. Planner picks readable names.
- **Return type of `LoadImageColors` / `LoadImagePalette`.** raylib returns `Color *` + `int *count` out-param. Iron side: `image.load_colors() -> [Color]` (returns an Iron array) using Phase 63-03's `Iron_List_Iron_Color` reverse-direction. First time an Iron List flows BACK from C. Planner probes in Plan 66-02 Task 1.
- **Bool coercion for `*IsValid` / `ColorIsEqual`** — shim pattern established Phase 62 (`(bool)(... != 0)`).
- **Smoke test content.** Standalone `tests/manual/texture_smoke.iron` should exercise one Image load+transform pipeline + one Texture load+draw pipeline + Color math. Don't render interactively (no pixel verification); exit 0 = pass.
- **PNG test fixture.** A 64×64 PNG at `tests/manual/test_texture.png` needed for `Image.load(path)`. Planner can generate one via `ImageColor` → `image.export()` in a pre-run script, or use a checked-in fixture.
- **`GetFontDefault` stub.** Phase 67 owns Font — Phase 66 must NOT bind font-related calls. If `ImageText(text, size, color)` needs a font internally (raylib uses default font), the shim forwards transparently.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Phase 60-65 foundation
- `.planning/phases/60-type-enum-foundation/60-03-SUMMARY.md` — Image/Texture/RenderTexture/NPatchInfo layout, _Static_assert grid entries
- `.planning/phases/60-type-enum-foundation/60-08-SUMMARY.md` — 6-color rescue palette (Phase 66 REPLACES this)
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-CONTEXT.md` — typed enum param cast pattern
- `.planning/phases/63-2d-drawing/63-03-SUMMARY.md` — `Iron_List_<T>` by-value array ABI for `[Vector2]` / `[UInt8]` / `[Color]`
- `.planning/phases/64-collision-2d-3d/64-01-SUMMARY.md` — instance methods on data-carrying objects, tuple-return auto-emit, `self` E0101 rule
- `.planning/phases/65-raymath/65-CONTEXT.md` — RMath namespace rename (Iron_math_ reserved), raymath.h include order

### raylib upstream
- `src/vendor/raylib/raylib.h` lines 1368-1507 — Image functions (TEX-01..07)
- `src/vendor/raylib/raylib.h` lines 1508-1575 — Texture functions (TEX-08..12)
- `src/vendor/raylib/raylib.h` lines 1576-1611 — Color functions (TEX-13)
- `src/vendor/raylib/raylib.h` lines 212-240 — 26-color palette `#define` macros (TEX-14 RGBA source of truth)
- `src/vendor/raylib/rtextures.c` — all implementations (reference only; raylib builds it)

### Iron stdlib precedents
- `src/stdlib/iron_raylib.h` — pre-scaffolded `/* ── Textures (Phase 66) ──── */` section marker (Plan 60-01)
- `src/stdlib/iron_raylib.c` — mirror section marker
- `src/stdlib/raylib.iron` — `object Image/Texture/RenderTexture/NPatchInfo/Color` all defined; module-level 6-color rescue palette to REPLACE
- `src/stdlib/iron_raylib.c` `Iron_draw_spline_linear` — `Iron_List_Iron_Vector2` by-value array ARG pattern; Phase 66 adapts for `Iron_List_Iron_UInt8` (raw image data) and `Iron_List_Iron_Color` (returned palette)
- `src/stdlib/iron_raylib.c` `Iron_collision_lines` — tuple-return template; Phase 66 may use for `LoadImageColors() -> (Array, Int32)` if array-count unpacking

### Project-level specs
- `.planning/REQUIREMENTS.md` lines 132-145 — TEX-01..14 detailed descriptions
- `.planning/ROADMAP.md` — Phase 66 goal, 5 success truths
- `.planning/PROJECT.md` — milestone scope, API style guidance

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`object Image`, `object Texture`, `object RenderTexture`, `object NPatchInfo`, `object Color`** all defined Phase 60-03 with `_Static_assert` layout pins. Phase 66 extends with methods; no layout changes.
- **6-color rescue palette (RED/BLUE/GREEN/WHITE/BLACK/DARKGRAY)** at end of `src/stdlib/raylib.iron` — Phase 66 Plan 66-01 REPLACES with full 26-color palette.
- **`Iron_List_Iron_<T>` auto-emission** — Phase 63-04 fix in emit_structs.c scans foreign-method-stub params for `[T]` and emits typedef. Phase 66 exercises this for `[UInt8]` (raw image data) and `[Color]` (palette returns).
- **Enum-cast pattern** — `(int)filter` / `(int)wrap` / `(int)layout` / `(int)format` for all typed-enum params. Phase 62 precedent.
- **String marshalling** — Iron `String` → C `const char *` is compiler-managed. Phase 66 uses it for path args (TEX-01 load, TEX-04 export, `ImageText` text arg).
- **Static constructor dispatch** — `Image.load(path)`, `Color.from_hsv(v)` use namespace-form dispatch validated Phase 64 (`object Collision {}`) and Phase 65 (`Matrix.identity()`, `Quaternion.from_euler()`).

### Established Patterns
- **Memcpy template** for all struct-by-value args/returns. Image 40B, Texture 20B, Color 4B, HSV as Vector3 12B — all well within Phase 64's validated 120B max.
- **Float32-to-double widening** (Phase 65 Frustum/Perspective/Ortho) — NOT needed for TEX functions (raylib Color math uses `float` throughout).
- **Tuple return** — needed only if planner picks it for `LoadImageColors() -> (Array, Int32)`. Likely overkill; array `[Color]` with `.count` inferred from the Iron list structure is cleaner.

### Integration Points
- `src/stdlib/iron_raylib.c` Textures section — ~100 shims
- `src/stdlib/iron_raylib.h` Textures section — ~100 prototypes (auto-emitted)
- `src/stdlib/raylib.iron` — extend Image/Texture/RenderTexture/Color objects with methods; REPLACE 6-color palette with 26-color palette
- (Optional) `tests/manual/texture_smoke.iron` — Plan 66-05 standalone smoke test
- (Optional) `tests/manual/test_texture.png` — 64×64 PNG fixture for load tests

</code_context>

<specifics>
## Specific Ideas

- **Instance-mutating methods return-by-value** — E.g., `image.crop(rec) -> Image`. Users rebind: `val img = Image.load("x.png").crop(Rectangle(10,10,50,50)).flip_vertical()`. Raylib C mutates through `Image *`; Iron shim does `memcpy; mutate; memcpy-out-as-return`.
- **26-color palette RGBA values verbatim from raylib.h lines 212–240** — copy exactly, no reordering, no renames.
- **`LoadImageColors` returns `[Color]` array** — first time Iron List flows from C back to Iron via `Iron_List_Iron_Color`. Confirm auto-generated typedef exists before writing shim (Plan 66-02 Task 1 probe).
- **Texture draw-variants mirror Phase 63 `Draw.*` pattern** — `Texture.draw(self, x, y, tint)` + 5 variants. Same memcpy template; first time Draw.* binding flows through a texture-receiver method instead of a namespace static.
- **Cubemap layout is an enum param** — `Texture.load_cubemap(image, layout: CubemapLayout)`. Phase 60-07 already defined CubemapLayout with 6 values (AUTO_DETECT through PANORAMA).
- **`GetPixelColor(void *srcPtr, int format) -> Color`** — opaque pointer arg. Iron: `Color.from_pixel_data(data: Int, format: PixelFormat) -> Color`. The `data: Int` is the user's raw pointer (e.g., from an Image.data accessor, not yet exposed).
- **`SetPixelColor(void *dstPtr, Color color, int format)`** — opaque pointer out-write. Iron: `Color.to_pixel_data(self, data: Int, format: PixelFormat)`. Same opaque-Int treatment.
- **Memory ownership:** raylib's `Image` and `Texture` own their GPU/CPU data. `image.unload()` frees CPU; `texture.unload()` frees GPU. Iron does NOT auto-unload on scope exit (no Drop trait yet). Users call `.unload()` explicitly — documented in raylib.iron header comments.
- **`ImageText(text, size, color) -> Image`** uses raylib's default font internally. The Iron binding is a static `Image.text(text, size, color) -> Image`; shim forwards transparently. Phase 67 (Fonts) will add `Image.text_ex(text, font, size, spacing, color)`.
- **Phase 66 does NOT touch consumer files** (pong.iron / game_raylib.iron / hello_raylib.iron) — zero `-- PHASE 66:` markers exist. Smoke test is standalone.

</specifics>

<deferred>
## Deferred Ideas

- **Automatic `Drop` / RAII for Image/Texture.** Iron has no Drop trait yet. Resource cleanup via explicit `.unload()` call. Phase 73 API polish could explore if Iron adds the feature.
- **Image streaming / video textures.** Out of raylib 5.5 core scope.
- **Custom pixel format conversion** beyond `ImageFormat`. Out of scope.
- **Texture compression format transcoding** (BC7 → ASTC etc.). raylib only loads; no transcoding.
- **Async Image loading.** raylib is synchronous. Users compose async via their own threading.
- **Font wiring through Image/Texture.** Phase 67 owns Font. Phase 66 binds `ImageText` with default font; custom font variants are Phase 67's `Image.text_ex`.
- **Pixel-level verification smoke test.** Phase 66 smoke is load/build-only; actual pixel correctness is user-visual. Phase 73 showcase could add a render-to-texture + pixel-diff test.
- **Gradient color constants** (LIGHTGRAY_ALPHA_50 etc.). Raylib only ships 26 opaque colors. User composition.

</deferred>

---

*Phase: 66-textures-images*
*Context gathered: 2026-04-17 via smart discuss (autonomous)*
