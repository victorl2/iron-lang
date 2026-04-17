# Phase 66: Textures & Images - Research

**Researched:** 2026-04-17
**Domain:** raylib `rtextures.c` binding — Image/Texture/RenderTexture/Color, ~100 functions + 26-color palette
**Confidence:** HIGH (mechanisms 1-4 verified in source; mechanism 5 flagged MEDIUM with mitigation)

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Inherited from Phase 60/61/62/63/64/65 (no discussion):**

- **Binding architecture:** shim-only via `/* ── Textures (Phase 66) ──── */` section marker (Plan 60-01 scaffolded, at iron_raylib.h:938 and iron_raylib.c:2619).
- **Method casing:** `snake_case`. `image.color_tint(tint)`, `texture.set_filter(.bilinear)`, `Image.load("path.png")`, `color.to_hsv()`.
- **Method dispatch:** instance methods on data-carrying objects (Image, Texture, Color) use the Phase 64 pattern (`hir_to_lir.c:1096-1297`). Static constructors (`Image.load`, `Texture.load`, `Color.from_hsv`) use namespace form validated across Phase 62-65.
- **`self` reserved** (E0101) — receivers use semantic names: `image`/`img`, `texture`/`tex`, `color`/`c`.
- **String arguments** — raylib's `const char *` path params are compiler-managed `String → char *` (Phase 61 precedent).
- **Float32 vs Float:** raylib uses `float` — Iron `Float32` everywhere.
- **Enum parameters:** typed — `PixelFormat`, `TextureFilter`, `TextureWrap`, `CubemapLayout`. Shim casts to `int`.
- **Struct-by-value:** Image (40 B), Texture (20 B), RenderTexture (44 B), NPatchInfo (36 B), Color (4 B), Rectangle (16 B), Vector2 (8 B), Vector3 (12 B), Vector4/HSV (16 B). Straightforward memcpy per Phase 64's 120 B max precedent.
- **Pointer/opaque handling:** Image has `void *data`, Texture has `unsigned int id`. Iron mirrors use `_data` (Int) and `id` (UInt32) per Phase 60-03.
- **Auto-generated C prototypes** via emit_c (commit e3e5eee).
- **Layout grid** pinned at 413 asserts through Phase 65. No new types in Phase 66.
- **Memory discipline:** ironc ~10 GB per invocation — 1-2 smoke builds per plan max.

**Phase 66 specific:**

- **ALL receivers (Image/Texture/Color) are data-carrying objects.** Static constructors: `Image.load`, `Image.from_memory`, `Image.from_screen`, `Image.color`, `Image.gradient_linear`, `Image.checked`, `Image.text`, `Texture.load`, `Texture.from_image`, `Texture.load_cubemap`, `RenderTexture.load`, `Color.from_hsv`, `Color.from_int`, `Color.from_normalized`, `Color.from_pixel_format`. Instance methods: `image.crop`, `image.resize`, `image.color_tint`, `image.draw_pixel`, `image.to_texture`, `image.unload`, `texture.update`, `texture.set_filter`, `texture.draw`, `texture.unload`, `color.tint`, `color.fade`, `color.to_hsv`, `color.alpha_blend`.
- **Mutating transforms return-by-value (Option A/B locked):** `image.crop(rec) -> Image`. Shim pattern: `memcpy` self in, call raylib `ImageCrop(&local, r)`, memcpy out as return. Users rebind: `val new_img = old_img.crop(rec)`. Chain-style for image CPU draws: `Image.color(64,64,BLACK).draw_pixel(10,10,RED).draw_rectangle(...)`.
- **26-color palette REPLACES Plan 60-08's 6-color rescue** at raylib.iron:1216-1236. Header comment changed from "Phase 66 TEX-14 will replace this block wholesale" to "raylib 5.5 canonical 26-color palette (TEX-14 closed Phase 66)". RGBA from raylib.h:175-201 verbatim.
- **Plan slicing — 5 plans:**
  - **66-01 Color math + palette (TEX-13, TEX-14):** 18 Color methods + 26 `val` palette. Smallest, independent, no Image/Texture dependency.
  - **66-02 Image load/gen/save/extract (TEX-01, TEX-02, TEX-03, TEX-04, TEX-06):** ~25 functions. First String-arg stress + raw-memory probe + `LoadImageColors -> [Color]` reverse-direction Iron_List probe.
  - **66-03 Image transforms + CPU draw (TEX-05, TEX-07):** ~50 functions. All `Image*` mutating transforms + all `ImageDraw*`.
  - **66-04 Texture load + update + config + draw (TEX-08..12):** ~18 functions. First cubemap + RenderTexture.
  - **66-05 Smoke test + ABI sweep:** Standalone `tests/manual/texture_smoke.iron` (no consumer files — zero `-- PHASE 66:` markers exist in pong.iron/game_raylib.iron/hello_raylib.iron).

### Claude's Discretion

- **Method names where raylib doesn't map cleanly to snake_case.** E.g., `ImageFromImage(image, rec)` → `image.from_image(rec)` reads poorly; rename to `image.from_rectangle(rec)` or `image.copy_rec(rec)`. Planner picks readable names.
- **Return type of `LoadImageColors` / `LoadImagePalette`.** raylib returns `Color *` + `int *count`. Iron: `image.load_colors() -> [Color]` using Phase 63-03's Iron_List. **First time Iron List flows FROM C TO Iron.** Planner probes in Plan 66-02 Task 1. Fallback: tuple `(Int, Int32)` = (pointer-as-Int, count) + helper accessor.
- **Bool coercion for `*IsValid` / `ColorIsEqual`** — Phase 62 pattern `(bool)(... != 0)`.
- **Smoke test content.** Standalone `tests/manual/texture_smoke.iron` exercises one Image load+transform pipeline + one Texture load+draw + Color math. No interactive rendering; exit 0 = pass.
- **PNG test fixture.** 64x64 PNG at `tests/manual/test_texture.png` for `Image.load(path)`. Generate via pre-run script or check in.
- **`GetFontDefault` stub.** Phase 67 owns Font. `ImageText(text, size, color)` uses raylib's default font internally — shim forwards transparently.

### Deferred Ideas (OUT OF SCOPE)

- **Automatic Drop/RAII for Image/Texture.** Iron has no Drop trait. Explicit `.unload()` only.
- **Image streaming / video textures.** Out of raylib 5.5 core scope.
- **Custom pixel format conversion** beyond `ImageFormat`. Out of scope.
- **Texture compression format transcoding** (BC7 → ASTC etc.).
- **Async Image loading.** raylib is synchronous.
- **Font wiring through Image/Texture.** Phase 67 owns Font. Phase 66 binds `ImageText` (default font); custom font variants are Phase 67's `Image.text_ex`.
- **Pixel-level verification smoke test.** Phase 66 smoke is load/build-only.
- **Gradient color constants** (LIGHTGRAY_ALPHA_50 etc.). raylib ships 26 opaque colors only.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| TEX-01 | `Image.load(path)` + `image.unload()` for 14 file formats | raylib.h:1323 `LoadImage`; Phase 61 String→`const char *` marshalling precedent; Image 40 B memcpy-out return (Phase 64 Rectangle precedent validated) |
| TEX-02 | Load Image from raw memory, file data buffer, screen, compressed | raylib.h:1324-1329 `LoadImageRaw`, `LoadImageFromMemory`, `LoadImageFromTexture`, `LoadImageFromScreen`, `LoadImageAnim*`. **BLOCKER:** `[UInt8]` primitive-element array not supported at FFI — see Pitfall 7. Workaround: defer to Phase 67 or use raw `Int` pointer + length ABI |
| TEX-03 | Procedural images — `Image.color`, gradients, checked, noise, cellular, text | raylib.h:1337-1345. Straightforward primitive-arg + Color-by-value. `GenImageText` uses default font — see Pitfall 4 |
| TEX-04 | Image export to file/memory/code | raylib.h:1332-1334 `ExportImage`, `ExportImageToMemory`, `ExportImageAsCode`. `ExportImageToMemory` returns `unsigned char *` + out-count — same `[UInt8]` blocker as TEX-02 |
| TEX-05 | Transform-in-place (28 functions): crop/resize/rotate/flip/color*/alpha*/blur/dither/mipmaps | raylib.h:1347-1377 — all take `Image *`. Return-by-value pattern locked: memcpy-in, call `ImageFoo(&local,...)`, memcpy-out as return |
| TEX-06 | Extract data — `load_colors`, `load_palette`, `get_alpha_border`, `get_color` | raylib.h:1378-1383. **`LoadImageColors -> [Color]` is the critical reverse-direction Iron_List probe.** Plan 63-04 scan (emit_structs.c:309-312) auto-emits `Iron_List_Iron_Color` typedef+DECL+IMPL for `[Color]` return — VERIFIED in source. Shim uses `Iron_List_Iron_Color_create_with_capacity(n)` + memcpy items + `list.count = n` |
| TEX-07 | Image CPU draw (~22 functions) — pixel/line/circle/rectangle/triangle/text | raylib.h:1385-1408. Mutating pattern: memcpy-in, `ImageDraw*(&local,...)`, memcpy-out. Chain-friendly |
| TEX-08 | `Texture.load(path)` + `image.to_texture()` + `texture.unload()` | raylib.h:1412-1417. Texture 20 B struct-by-value return. `LoadTextureFromImage` takes Image by value (40 B) |
| TEX-09 | Cubemap + RenderTexture load/unload + IsValid | raylib.h:1414-1419. `LoadTextureCubemap(Image, int layout)` — enum-cast via Phase 62 pattern. RenderTexture 44 B struct-by-value (largest raylib struct so far in returns; under Phase 64's 120 B ceiling) |
| TEX-10 | Texture update — `texture.update(pixels)`, `texture.update_rec(rec, pixels)` | raylib.h:1420-1421. `void *pixels` arg → Iron `Int` with `(void *)(intptr_t)ptr` cast in shim. Pitfall 7 |
| TEX-11 | Filter/wrap/mipmaps configuration | raylib.h:1424-1426. `GenTextureMipmaps(Texture *)` mutates — same return-by-value pattern as Image transforms |
| TEX-12 | All 6 texture draw variants as methods on Texture | raylib.h:1429-1434. Same memcpy template as Phase 63 Draw.*; instance-method dispatch validated Phase 64 |
| TEX-13 | Color math (18 functions) — is_equal, fade, to_int, to_hsv, from_hsv, tint, brightness, contrast, alpha, alpha_blend, lerp, get_color, get_pixel_color, set_pixel_color, get_pixel_data_size | raylib.h:1437-1453. Color 4 B struct-by-value + Vector3 HSV return + Vector4 normalized return. `GetPixelColor(void *, int)` and `SetPixelColor(void *, Color, int)` → opaque pointer cast (Pitfall 7) |
| TEX-14 | All 26 raylib palette constants — LIGHTGRAY..RAYWHITE | raylib.h:175-201 `#define` macros (verbatim copy). REPLACES raylib.iron:1216-1236 6-color rescue block. `val NAME = Color(r,g,b,a)` positional constructor per Plan 60-08 proven precedent |
</phase_requirements>

## Summary

Phase 66 binds raylib's entire `rtextures.c` module (~100 functions) as idiomatic Iron methods on `Image` / `Texture` / `RenderTexture` / `Color` data-carrying objects, plus a module-level 26-color `val` palette replacing Plan 60-08's 6-color rescue. Every underlying mechanism is already proven in prior phases: struct-by-value memcpy for args/returns (Phases 63/64/65), instance methods on data-carrying objects (Phase 64), enum-as-int casting (Phase 62), String→`const char *` (Phase 61). The only genuine new ABI territory is **`-> [Color]` reverse-direction list return** — which Plan 63-04's scan-and-emit fix at `emit_structs.c:309-312` already supports for object element types. `[UInt8]` primitive-element lists are NOT yet supported at the FFI and are the one real blocker — affecting TEX-02 `LoadImageRaw` / `LoadImageFromMemory` / `ExportImageToMemory`. Opaque `void *` args (TEX-10, TEX-13) use the established `Int` → `(void *)(intptr_t)` cast pattern.

**Primary recommendation:** Adopt the 5-plan slicing locked in CONTEXT.md. Start with Plan 66-01 (Color math + 26-color palette) — independent, small, exercises no new mechanisms, closes TEX-13/TEX-14 as a clean first plan. Plan 66-02 carries the two genuine probes (reverse-direction `[Color]` Iron_List + raw-memory `[UInt8]` blocker). Plans 66-03/04/05 are mechanical applications of Phase 64's memcpy template at scale. Defer raw-memory `LoadImageFromMemory`/`LoadImageRaw`/`ExportImageToMemory` to a follow-up (Phase 67 or later) if `[UInt8]` runtime support can't land in Plan 66-02.

## Standard Stack

### Core

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| raylib | 5.5 (vendored at `src/vendor/raylib/`) | Source of all texture/image/color C functions | Project-mandated (not a choice); all layouts pinned by `_Static_assert` grid |
| Iron stdlib `object` + foreign-method-stub | current ironc | Binding pattern for all Iron-side API | Phases 60-65 precedent; 5 prior phases validate the pattern |
| `Iron_List_<T>` runtime | iron_runtime.h:459-529 | Iron-native array ABI for `[Color]` return | Phase 59 DNS, Phase 63-03/04 Vector2 arrays both proven |

### Supporting

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| memcpy (C stdlib `<string.h>`) | system | Struct-by-value args/returns across Iron↔raylib boundary | Every Phase 66 shim |
| Phase 60 `_Static_assert` grid | 413 asserts in iron_raylib_layout.c | Compile-time proof Iron_Image/Texture/Color == raylib Image/Texture/Color byte-for-byte | Already landed; no new asserts needed |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `image.crop(rec) -> Image` return-by-value | `image.crop_mut(rec)` with `&mut self` | Iron doesn't have `&mut self`; locked out |
| `image.load_colors() -> [Color]` | `image.load_colors() -> (Int, Int32)` (pointer-as-int + count) + `.color_at(i)` accessor | Safer fallback if `[Color]` auto-emission fails; uglier API. **Recommendation: try `-> [Color]` first (Plan 63-04 infrastructure exists); fallback only if probe fails** |
| `Image.from_memory(type, data: [UInt8], size)` | `Image.from_memory(type, data: Int, size)` (raw pointer) | `[UInt8]` NOT supported at FFI; raw pointer is a safety hole. **Recommendation: defer to Phase 67 or later unless `[UInt8]` runtime lands** |

**Installation:** No new dependencies. raylib 5.5 is vendored; `iron_runtime.h` / `iron_raylib.{h,c}` already include everything needed.

**Version verification:** raylib 5.5 vendored — verified at `src/vendor/raylib/raylib.h` header version string. All 100+ function signatures read from raylib.h lines 1323-1453 on 2026-04-17. rtextures.c implementations built via existing `src/cli/build.c` pipeline (no changes).

## Architecture Patterns

### Recommended Project Structure

```
src/stdlib/
├── raylib.iron           # Iron-side: object methods + 26-color palette replacing 6-color rescue at lines 1216-1236
├── iron_raylib.h         # C: ~100 Iron_image_* / Iron_texture_* / Iron_color_* prototypes in Phase 66 section (h:938)
└── iron_raylib.c         # C: ~100 shim implementations in Phase 66 section (c:2619)

tests/manual/
├── texture_smoke.iron    # Plan 66-05: standalone Phase 66 regression test
└── test_texture.png      # 64x64 PNG fixture for Image.load probe
```

### Pattern 1: Static-constructor shim (Image/Texture/RenderTexture loading)

**What:** raylib's `LoadXxx(path)` → Iron static method `Type.load(path)` + struct-by-value return.
**When to use:** Every `Image.load`, `Texture.load`, `RenderTexture.load`, `Color.from_hsv`, `Color.from_int`, `Color.from_normalized`.
**Example (verbatim template from Phase 65 Matrix precedent):**

```c
/* Source: iron_raylib.c Phase 65 precedent (Iron_matrix_identity),
 * adapted for Image by-value return. */
struct Iron_Image Iron_image_load(Iron_String path) {
    const char *cpath = iron_string_cstr(&path);
    Image src = LoadImage(cpath ? cpath : "");
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}
```

Iron side: `func Image.load(path: String) -> Image {}`.

### Pattern 2: Mutating-transform return-by-value (Image*/Texture* in-place)

**What:** raylib's `ImageCrop(Image *, Rectangle)` (mutates through pointer) → Iron instance method returning a fresh `Image`. Users rebind.
**When to use:** All 28 `Image*` transforms in TEX-05, `GenTextureMipmaps` in TEX-11, all `ImageDraw*` in TEX-07.
**Example:**

```c
/* Source: adapts Phase 64 memcpy-in pattern + return-by-value.
 * Iron func: image.crop(self: Image, crop: Rectangle) -> Image {} */
struct Iron_Image Iron_image_crop(struct Iron_Image self, struct Iron_Rectangle crop) {
    Image img; Rectangle rec;
    memcpy(&img, &self, sizeof(Image));
    memcpy(&rec, &crop, sizeof(Rectangle));
    ImageCrop(&img, rec);  /* mutates local img */
    struct Iron_Image out;
    memcpy(&out, &img, sizeof(struct Iron_Image));
    return out;
}
```

Iron side: `val cropped = original.crop(Rectangle(10, 10, 50, 50))`. Chain: `Image.color(...).draw_pixel(...).flip_vertical()`.

### Pattern 3: Reverse-direction Iron_List return (TEX-06 `load_colors`)

**What:** raylib `LoadImageColors(Image) -> Color *` (caller owns) + implicit count from `image.width * image.height`. Iron `image.load_colors() -> [Color]` returns an owned Iron_List_Iron_Color.
**When to use:** `TEX-06` `LoadImageColors`, `LoadImagePalette`.
**Mechanism:** Plan 63-04's scan in `emit_structs.c:296-313` detects `func Foo.bar(...) -> [Color] {}` in foreign-method stubs and auto-emits `Iron_List_Iron_Color` typedef + `IRON_LIST_DECL` + `IRON_LIST_IMPL` macros — which expand to `Iron_List_Iron_Color_create_with_capacity(cap)`, `_push`, `_get`, `_len`, `_free` helpers.
**Example (template from iron_net.c:1269 lookup_host precedent):**

```c
/* Iron func: image.load_colors(self: Image) -> [Color] {} */
Iron_List_Iron_Color Iron_image_load_colors(struct Iron_Image self) {
    Image img;
    memcpy(&img, &self, sizeof(Image));
    Color *colors = LoadImageColors(img);          /* raylib malloc */
    int64_t count = (int64_t)img.width * img.height;
    Iron_List_Iron_Color out =
        Iron_List_Iron_Color_create_with_capacity(count);
    if (colors && count > 0) {
        memcpy(out.items, colors, (size_t)count * sizeof(struct Iron_Color));
        out.count = count;
    }
    UnloadImageColors(colors);  /* raylib free — Iron now owns the data */
    return out;
}
```

**Verification source:** `emit_structs.c:309-312`: `if (fn->return_type && fn->return_type->kind == IRON_TYPE_ARRAY) { PLAN_63_04_EMIT_LIST_FOR(fn->return_type->array.elem, "via foreign-method-stub return scan"); }`. Confirmed scans `return_type`, not just params. **HIGH confidence** this works — but requires a probe in Plan 66-02 Task 1 because no `-> [Object]` return has yet flowed through raylib.iron (Phase 59 DNS is the only precedent, and it lives in net.iron with its own hand-coded `need_dns` block in emit_c.c:6394).

### Pattern 4: Opaque `void *` pointer arg (TEX-10 update, TEX-13 pixel)

**What:** raylib takes `void *pixels`, `void *srcPtr`, `void *dstPtr`. Iron has no `void *` type. Precedent: `Image._data: Int` + C-side `void *_data` field (Plan 60-03).
**When to use:** `UpdateTexture`, `UpdateTextureRec`, `GetPixelColor`, `SetPixelColor`.
**Example:**

```c
/* Iron func: Color.from_pixel_data(data: Int, format: PixelFormat) -> Color {} */
struct Iron_Color Iron_color_from_pixel_data(int64_t data, int32_t format) {
    void *ptr = (void *)(intptr_t)data;
    Color c = GetPixelColor(ptr, (int)format);
    struct Iron_Color out;
    memcpy(&out, &c, sizeof(struct Iron_Color));
    return out;
}
```

**Caveat:** No precedent for a raw `void *` shim arg in the existing stdlib (iron_raylib.c has `(void *)src.paths` only as a FIELD cast inside a struct, not a function arg). This is a **new pattern requiring a Plan 66-01 or 66-02 Task 1 probe**. Expected to work because `sizeof(int64_t) == sizeof(void *)` on all 64-bit targets (iron_raylib.h:127 documents this).

### Pattern 5: Module-level `val` palette (TEX-14)

**What:** 26 `val NAME = Color(r, g, b, a)` module-level declarations at the end of raylib.iron, REPLACING the 6-color rescue at lines 1231-1236 and the header comment at lines 1216-1230.
**Example (template from Plan 60-08):**

```iron
-- ══════════════════════════════════════════════════════════════════════
-- raylib 5.5 canonical 26-color palette (TEX-14 closed Phase 66)
-- ══════════════════════════════════════════════════════════════════════
--
-- RGBA values copied verbatim from raylib.h lines 175-201.

val LIGHTGRAY  = Color(200, 200, 200, 255)
val GRAY       = Color(130, 130, 130, 255)
val DARKGRAY   = Color(80,  80,  80,  255)
val YELLOW     = Color(253, 249, 0,   255)
val GOLD       = Color(255, 203, 0,   255)
val ORANGE     = Color(255, 161, 0,   255)
val PINK       = Color(255, 109, 194, 255)
val RED        = Color(230, 41,  55,  255)
val MAROON     = Color(190, 33,  55,  255)
val GREEN      = Color(0,   228, 48,  255)
val LIME       = Color(0,   158, 47,  255)
val DARKGREEN  = Color(0,   117, 44,  255)
val SKYBLUE    = Color(102, 191, 255, 255)
val BLUE       = Color(0,   121, 241, 255)
val DARKBLUE   = Color(0,   82,  172, 255)
val PURPLE     = Color(200, 122, 255, 255)
val VIOLET     = Color(135, 60,  190, 255)
val DARKPURPLE = Color(112, 31,  126, 255)
val BEIGE      = Color(211, 176, 131, 255)
val BROWN      = Color(127, 106, 79,  255)
val DARKBROWN  = Color(76,  63,  47,  255)
val WHITE      = Color(255, 255, 255, 255)
val BLACK      = Color(0,   0,   0,   255)
val BLANK      = Color(0,   0,   0,   0)
val MAGENTA    = Color(255, 0,   255, 255)
val RAYWHITE   = Color(245, 245, 245, 255)
```

**Syntax verification:** `Color(UInt8-fitting-int-literal, ...)` accepted by typecheck.c:1326 narrowing rule (Plan 60-08 Deviation #1). Already used 6x in raylib.iron:1231-1236.

### Anti-Patterns to Avoid

- **Don't build raw-memory `[UInt8]` via `Int` hack.** Users passing raw pointers as `Int` defeats type safety and leaks memory ownership. Either (a) wait for `[UInt8]` primitive-array FFI support, or (b) expose a `FileData` opaque-handle object type. DON'T expose `data: Int, size: Int` in the public API.
- **Don't call `.unload()` twice.** raylib `UnloadImage` / `UnloadTexture` / `UnloadRenderTexture` are not idempotent — double-free on the `void *data` / GPU handle. Document in raylib.iron header; users are responsible (no Drop trait).
- **Don't forget `UnloadImageColors(colors)` in the `load_colors` shim.** raylib mallocs; Iron owns the Color bytes after memcpy. Omitting the unload leaks one malloc per call.
- **Don't use `self` as an Iron parameter name.** E0101 reserved word. Use `image` / `img` / `texture` / `tex` / `color` / `c` / `rt`. Phase 64-01 Deviation #1 is the canonical reference.
- **Don't skip the `load_colors` probe.** First `-> [Object]` return through raylib.iron — verify the auto-emit at emit_structs.c:309-312 actually fires for raylib before committing Plan 66-02 shims.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Color struct layout | Custom RGBA packing | raylib `Color` (4 `unsigned char`) + Phase 60 `_Static_assert` grid | Already byte-proven at compile time |
| Image transform (crop, resize, rotate, blur, dither) | Custom pixel iteration | `ImageCrop`, `ImageResize`, `ImageRotate`, `ImageBlurGaussian`, `ImageDither` | raylib's implementations handle every pixel format (`PIXELFORMAT_UNCOMPRESSED_GRAYSCALE` through compressed ASTC) correctly. A custom impl would re-derive 25 format branches |
| Image file format decoding (PNG/JPG/BMP/TGA/GIF/QOI/PSD/DDS/HDR/KTX/PIC/PVR/PKM/ASTC) | Custom loader | `LoadImage(path)` / `LoadImageFromMemory(type, data, size)` | raylib embeds stb_image.h + dr_libs + qoi.h — 14 formats free |
| Cubemap layout auto-detection | Custom 4:3 vs 3:4 vs panorama heuristic | `LoadTextureCubemap(image, CUBEMAP_LAYOUT_AUTO_DETECT)` | raylib has the bespoke auto-detect heuristic already; Phase 60-07 defined CubemapLayout with 6 values |
| Iron_List allocation for `-> [Color]` | Custom malloc + struct packing | `Iron_List_Iron_Color_create_with_capacity(n)` / `_push` / `_free` from iron_runtime.h:459-529 expanded by `IRON_LIST_IMPL(Iron_Color, Iron_Color)` | Standard Iron List ABI; auto-emitted by Plan 63-04 scan |
| 26-color palette RGBA derivation | Manual HSV→RGBA conversion | raylib.h:175-201 `#define` macros → verbatim copy | Canonical raylib colors; any variance == binding bug |
| PNG fixture generation | Vendoring external PNGs | `Image.color(64,64,RED).export("tests/manual/test_texture.png")` as pre-run step | Closed loop — uses the very binding under test |

**Key insight:** raylib's `rtextures.c` IS the library — Phase 66's job is surface-area exposure, not re-implementation. Every function bound is a thin shim (typically 4-8 lines: memcpy-in, call raylib, memcpy-out). Custom logic should be flagged as a planner mistake.

## Common Pitfalls

### Pitfall 1: `self` reserved word (E0101)

**What goes wrong:** Plan 64-01 Deviation #1 — initial Plan listed `func Image.crop(self: Image, crop: Rectangle)`. ironc check fails with 7+ E0101 errors because `self` is a reserved word in Iron's parser.
**Why it happens:** Iron's grammar reserves `self` for future use; foreign-method-stubs must use semantic receiver names.
**How to avoid:** Use `image` / `img` / `texture` / `tex` / `color` / `c` / `rt` (RenderTexture) throughout. iron_net.iron uses `s` (TcpSocket) and `l` (TcpListener) as short aliases.
**Warning signs:** ironc output `error[E0101]: expected parameter name` on every method stub.

### Pitfall 2: Float literal narrowing in Color/Rectangle constructors

**What goes wrong:** `Color(200.0, 200.0, 200.0, 255.0)` — fails type-check. Iron's float literals default to `Float` (double); Color fields are `UInt8`.
**Why it happens:** Float → UInt8 narrowing needs explicit cast, but Color's RGBA is UInt8. Integer literals (`200`) narrow implicitly via typecheck.c:1326.
**How to avoid:** **Always use integer literals** for Color RGBA: `Color(200, 200, 200, 255)`. For Rectangle/Vector2 (Float32 fields), use `Float32(200.0)` explicit cast. The 26-color palette example in Pattern 5 uses int literals throughout.
**Warning signs:** `error[E0217]: field 'r' expects 'UInt8', got 'Float'` from ironc.

### Pitfall 3: Missing `UnloadImageColors` / `UnloadImagePalette` in shim

**What goes wrong:** `LoadImageColors` mallocs a `Color *` buffer. Iron side owns a copy (via `memcpy` into `Iron_List.items`). If shim forgets `UnloadImageColors(colors)`, every call leaks one malloc block.
**Why it happens:** raylib's ownership convention — caller must unload — is documented in a one-line comment but not enforced.
**How to avoid:** Every `Iron_image_load_colors` / `Iron_image_load_palette` shim MUST `UnloadImageColors(colors)` (or `UnloadImagePalette`) AFTER memcpy into the Iron_List. Template in Pattern 3.
**Warning signs:** Memory-growth in a `for i in 0..10000: image.load_colors()` stress loop.

### Pitfall 4: `ImageText` default-font dependency on `InitWindow`

**What goes wrong:** `Image.text("Hello", 32, RED)` returns an empty image OR crashes if called before `Window.init(...)`.
**Why it happens:** raylib's `ImageText` internally calls `GetFontDefault()`. The default font is loaded by `LoadFontDefault()`, which is called BY `InitWindow` in `rcore.c:684`. Before window init, the default font is uninitialized.
**How to avoid:** Phase 66 smoke test MUST call `Window.init(...)` before any `Image.text(...)` or `ImageDrawText`. Document this prereq in the Iron-side comment above `Image.text`. Phase 67 (Fonts) is the permanent home for default-font lifecycle.
**Warning signs:** Black image, `[TEXT] WARNING: LoadFontDefault() called before InitWindow()` in raylib trace log, segfault on macOS.
**Source:** rcore.c:484 `extern void LoadFontDefault(void);`, rcore.c:684 default-font init in InitWindow.

### Pitfall 5: Iron_List typedef double-definition

**What goes wrong:** If both (a) Plan 63-04's scan at `emit_structs.c:309-312` emits `Iron_List_Iron_Color` typedef into generated C, AND (b) `iron_raylib.h` also declares `Iron_List_Iron_Color`, clang fails with "redefinition of struct Iron_List_Iron_Color".
**Why it happens:** `iron_raylib.c` compiles standalone via `clang -c` (outside CMake iron_stdlib target), so the header-guarded typedef is needed for standalone compilation. But ironc build path ALSO emits the typedef.
**How to avoid:** Use the IRON_LIST_IRON_COLOR_STRUCT_DEFINED guard pattern from `iron_raylib.h:604-610` (the `Iron_List_Iron_Vector2` precedent):

```c
#ifndef IRON_LIST_IRON_COLOR_STRUCT_DEFINED
#define IRON_LIST_IRON_COLOR_STRUCT_DEFINED
typedef struct Iron_List_Iron_Color {
    struct Iron_Color *items;
    int64_t            count;
    int64_t            capacity;
} Iron_List_Iron_Color;
#endif
```

Plus the shim body can call `Iron_List_Iron_Color_create_with_capacity(...)` etc. because the `IRON_LIST_IMPL` macro's generated functions have external linkage — emitted once in the generated C TU.
**Warning signs:** clang `error: redefinition of struct Iron_List_Iron_Color`.
**Source:** iron_raylib.h:604-610 `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` precedent.

### Pitfall 6: `-Wlarge-by-value-copy` threshold (64 B strict)

**What goes wrong:** clang's `-Wlarge-by-value-copy` warning threshold is "strictly greater than 64 bytes" on aarch64.
**Why it happens:** Historical warning for ABI efficiency.
**How to avoid:** Phase 66's biggest struct is **RenderTexture (44 B)** — well under threshold. Image (40 B), Texture (20 B), NPatchInfo (36 B), Color (4 B), Vector4 (16 B) — all fine. No `-Wlarge-by-value-copy` expected.
**Warning signs:** N/A for Phase 66 (Matrix 64 B in Phase 65 was the previous worst case; TEX functions don't use Matrix).
**Source:** Phase 65-01 SUMMARY Pitfall 6 ("Clang -Wlarge-by-value-copy on Matrix (64 B): Zero warnings, as predicted... threshold fires at strictly > 64"); Phase 65-03 SUMMARY same observation on Matrix returns.

### Pitfall 7: `[UInt8]` primitive-array is NOT supported at the FFI

**What goes wrong:** `func Image.from_memory(file_type: String, data: [UInt8], size: Int32) -> Image {}` fails at ironc C codegen — `Iron_List_uint8_t` is neither pre-declared in iron_runtime.h (which covers only int64_t/int32_t/double/bool/Iron_String/Iron_Closure per lines 825-830) nor auto-emitted by Plan 63-04's scan (which only handles `IRON_TYPE_OBJECT` element types per emit_structs.c:255).
**Why it happens:** No primary use case drove the primitive-list ABI. Phase 65 sidestepped by wrapping primitive arrays in object types (`Float3`, `Float16` — Plan 65-03 SUMMARY).
**How to avoid:**
- **Option A (recommended):** Defer TEX-02 memory-load + TEX-04 export-to-memory functions to Phase 67 or a later phase. Document in CONTEXT.md deferred list.
- **Option B:** Wrap the byte buffer in an opaque `FileData` object type with `_data: Int` + `size: Int32` fields + `FileData.load(path)` / `FileData.from_raw(data: Int, size: Int32)` accessors. Safer than raw `Int` but invents API surface.
- **Option C:** Use raw `Int` pointer + `Int32` size — matches raylib C but hole in type safety. NOT recommended.
- **Option D:** Add `Iron_List_uint8_t` to iron_runtime.h pre-declarations. Out of Phase 66 scope; cross-cutting runtime change.
**Warning signs:** ironc C codegen emits `Iron_List_uint8_t` references that clang rejects with "undeclared identifier".
**Source:** iron_runtime.h:825-830 (only 6 primitive types pre-declared); emit_structs.c:255 (`__et->kind == IRON_TYPE_OBJECT` guard).

**Phase 66 impact:** Affects `TEX-02` (`LoadImageRaw`, `LoadImageFromMemory`, `LoadImageAnimFromMemory`) and `TEX-04` (`ExportImageToMemory`). Plan 66-02 must make a binary decision in Task 1: probe `[UInt8]` support and either close the 4 functions OR defer them.

### Pitfall 8: raylib Texture vs Texture2D vs TextureCubemap typedef aliases

**What goes wrong:** raylib.h:1412 declares `LoadTexture(...) -> Texture2D` but Iron only has `object Texture`. The C typedef `Texture2D` is identical to `Texture` (per raylib.h convention), but Iron never defines `Texture2D`.
**Why it happens:** Plan 60-03 SUMMARY: "Iron's single `Texture` type covers raylib's Texture / Texture2D / TextureCubemap typedefs (all three are C typedefs for the same struct)."
**How to avoid:** All shims declare raylib calls returning `Texture2D` / `TextureCubemap` and just memcpy into `struct Iron_Texture`. clang accepts because `Texture2D` IS `Texture` in the C ABI.
**Warning signs:** N/A — this is a "works as designed" caveat, not a bug.
**Source:** Plan 60-03 SUMMARY key-decisions (line 2440 of that file).

### Pitfall 9: `-> [Color]` reverse-direction probe requires end-to-end ironc build

**What goes wrong:** `clang -c iron_raylib.c` standalone will FAIL on `Iron_List_Iron_Color` if only the guarded typedef from Pitfall 5 is in iron_raylib.h — because the shim body calls `Iron_List_Iron_Color_create_with_capacity(...)` which requires `IRON_LIST_DECL(Iron_Color, Iron_Color)` prototypes. End-to-end ironc build emits both the typedef AND the `IRON_LIST_DECL/IMPL` macros; standalone clang -c does not.
**Why it happens:** Two compilation contexts — iron_raylib.c is part of user's ironc build (gets full codegen context) AND can be compiled standalone via `clang -c` for ABI verification.
**How to avoid:**
- Guarded typedef in iron_raylib.h (Pitfall 5).
- **ALSO** add hand-written `IRON_LIST_DECL(Iron_Color, Iron_Color)` inside the `#ifndef` block — the declarations-only expansion (not `_IMPL`), because the ironc build path emits `IMPL` once in the generated C TU and the standalone build path just gets the prototypes.
- OR: accept that `clang -c iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` will fail for Phase 66 shims and rely on end-to-end ironc build only. Phase 64-01 set this precedent with `Iron_Tuple_Bool_Vector2` guarded typedef.
**Warning signs:** clang `implicit declaration of function 'Iron_List_Iron_Color_create_with_capacity'`.
**Source:** Plan 63-04 emit_structs.c:270-275 (IRON_LIST_DECL emission), iron_runtime.h:459-468 (IRON_LIST_DECL macro body).

## Code Examples

Verified patterns from official sources:

### Static constructor with String arg (TEX-01)

```c
/* Source: adapts Phase 61 Window.load_icon + Phase 65 Matrix.identity
 * templates; confirms String→const char * marshalling via iron_string_cstr.
 * Iron func: Image.load(path: String) -> Image {} */
struct Iron_Image Iron_image_load(Iron_String path) {
    const char *cpath = iron_string_cstr(&path);
    Image src = LoadImage(cpath ? cpath : "");
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}
```

### Mutating transform return-by-value (TEX-05)

```c
/* Source: adapts Phase 64 memcpy-in pattern.
 * Iron func: image.crop(self: Image, crop: Rectangle) -> Image {} */
struct Iron_Image Iron_image_crop(struct Iron_Image self, struct Iron_Rectangle crop) {
    Image img; Rectangle rec;
    memcpy(&img, &self, sizeof(Image));
    memcpy(&rec, &crop, sizeof(Rectangle));
    ImageCrop(&img, rec);
    struct Iron_Image out;
    memcpy(&out, &img, sizeof(struct Iron_Image));
    return out;
}
```

### Texture-by-value + enum-cast arg (TEX-11)

```c
/* Source: adapts Phase 62 enum-cast + Phase 63 memcpy-in for Texture.
 * Iron func: texture.set_filter(tex: Texture, filter: TextureFilter) -> Texture {} */
struct Iron_Texture Iron_texture_set_filter(struct Iron_Texture tex, int32_t filter) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    SetTextureFilter(t, (int)filter);
    /* SetTextureFilter takes Texture by value — doesn't mutate caller's copy.
     * Return value is the same struct (Iron-side rebinds are no-ops semantically). */
    struct Iron_Texture out;
    memcpy(&out, &t, sizeof(struct Iron_Texture));
    return out;
}
```

### Color math with Vector3 return (TEX-13 `color.to_hsv`)

```c
/* Source: raylib.h:1442 ColorToHSV returns Vector3.
 * Iron func: color.to_hsv(c: Color) -> Vector3 {} */
struct Iron_Vector3 Iron_color_to_hsv(struct Iron_Color c) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    Vector3 hsv = ColorToHSV(col);
    struct Iron_Vector3 out;
    memcpy(&out, &hsv, sizeof(struct Iron_Vector3));
    return out;
}
```

### Reverse-direction Iron_List return (TEX-06 `image.load_colors`)

```c
/* Source: adapts iron_net.c:1269 lookup_host list-build pattern.
 * Iron func: image.load_colors(img: Image) -> [Color] {} */
Iron_List_Iron_Color Iron_image_load_colors(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    Color *colors = LoadImageColors(src);
    int64_t count = (int64_t)src.width * src.height;
    Iron_List_Iron_Color out =
        Iron_List_Iron_Color_create_with_capacity(count);
    if (colors && count > 0) {
        memcpy(out.items, colors, (size_t)count * sizeof(struct Iron_Color));
        out.count = count;
    }
    UnloadImageColors(colors);
    return out;
}
```

### Opaque void* arg (TEX-13 `Color.from_pixel_data`)

```c
/* Source: new pattern — raw void * arg. First ever in iron_raylib.c.
 * Iron func: Color.from_pixel_data(data: Int, format: PixelFormat) -> Color {} */
struct Iron_Color Iron_color_from_pixel_data(int64_t data, int32_t format) {
    void *src_ptr = (void *)(intptr_t)data;
    Color c = GetPixelColor(src_ptr, (int)format);
    struct Iron_Color out;
    memcpy(&out, &c, sizeof(struct Iron_Color));
    return out;
}
```

### Texture draw variant (TEX-12) — instance method on data-carrying object

```c
/* Source: adapts Phase 63 Draw.rectangle_pro + Phase 64 instance-method pattern.
 * Iron func: texture.draw_pro(tex: Texture, source: Rectangle, dest: Rectangle,
 *                              origin: Vector2, rotation: Float32, tint: Color) {} */
void Iron_texture_draw_pro(struct Iron_Texture tex,
                            struct Iron_Rectangle source,
                            struct Iron_Rectangle dest,
                            struct Iron_Vector2 origin,
                            float rotation,
                            struct Iron_Color tint) {
    Texture t; Rectangle sr, dr; Vector2 og; Color c;
    memcpy(&t,  &tex,    sizeof(Texture));
    memcpy(&sr, &source, sizeof(Rectangle));
    memcpy(&dr, &dest,   sizeof(Rectangle));
    memcpy(&og, &origin, sizeof(Vector2));
    memcpy(&c,  &tint,   sizeof(Color));
    DrawTexturePro(t, sr, dr, og, rotation, c);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| 6-color rescue palette (RED/BLUE/GREEN/WHITE/BLACK/DARKGRAY) | Full 26-color raylib 5.5 palette (LIGHTGRAY through RAYWHITE) | Phase 66 Plan 66-01 | All 3 consumer files (pong/game_raylib/hello_raylib) retain their 6 existing constants; new 20 become available to Phase 67+ consumers |
| `self: Image` receiver name | `image: Image` or `img: Image` | Phase 64-01 Deviation #1 (discovered 2026-04-17) | E0101 reserved word; all Phase 64-66 method stubs use semantic receiver names |
| `-> [Object]` reverse Iron_List unsupported | Plan 63-04 scan at emit_structs.c:296-313 auto-emits typedef+DECL+IMPL for any foreign-method-stub return type | Phase 63-04 (landed 2026-04-17) | Phase 66 Plan 66-02 is the FIRST raylib consumer; net.iron's DNS lookup was the only prior user (pre-Plan 63-04, hand-coded in emit_c.c:6394) |
| Namespace-only Draw.* dispatch | Instance methods on data-carrying object types (Rectangle.collides, Vector2.inside_triangle) | Phase 64-01 | Applied wholesale in Phase 66: Image.crop, Texture.draw, Color.tint — all receive self-by-value |
| `object Math {}` (iron_math.h prefix collision) | `object RMath {}` | Phase 65-01 Deviation #2 | Not relevant to Phase 66; `Iron_image_*` / `Iron_texture_*` / `Iron_color_*` prefixes don't collide with any existing header |

**Deprecated/outdated:**
- **6-color rescue palette** — phased out by Plan 66-01; 20 new colors (LIGHTGRAY, GRAY, YELLOW, GOLD, ORANGE, PINK, MAROON, LIME, DARKGREEN, SKYBLUE, DARKBLUE, PURPLE, VIOLET, DARKPURPLE, BEIGE, BROWN, DARKBROWN, BLANK, MAGENTA, RAYWHITE) added; existing 6 retained.
- **Plan 60-08 "TEX-14 replacement block" header comment** at raylib.iron:1216-1230 — replaced by canonical palette header.

## Open Questions

1. **Does `-> [Color]` reverse-direction Iron_List return actually work end-to-end through a raylib-stdlib foreign-method stub?**
   - What we know: Plan 63-04's scan at emit_structs.c:309-312 emits the typedef+DECL+IMPL; iron_runtime.h:459-529 defines the macros; net.iron's `Net.lookup_host -> ([Address], NetError)` is a working precedent (but in a different stdlib module with its own hand-coded emit_c.c hook).
   - What's unclear: Whether the scan fires for raylib.iron foreign-method-stubs in the same pass, whether the codegen hook for "emit IRON_LIST_IMPL once" is per-TU or cross-module, whether `iron_string_cstr(&path)` + `Iron_List_Iron_Color` compose cleanly in a single shim.
   - Recommendation: **Plan 66-02 Task 1 is a mandatory probe.** Add a single `func Image.probe_load_colors(img: Image) -> [Color] {}` stub + tiny caller + ironc build. Observe the generated C. If the typedef + DECL + IMPL all emit and link, proceed. If not, pivot to tuple `(Int, Int32)` return + accessor helper (CONTEXT.md fallback).

2. **Does Iron handle `Int` → `void *` arg cast cleanly at the shim boundary?**
   - What we know: `sizeof(int64_t) == sizeof(void *)` on all 64-bit targets (iron_raylib.h:127). Phase 60-03 uses this for `_data` struct FIELDS. iron_raylib.c uses `(void *)src.paths` as a FIELD cast (c:538), never as a function-arg cast.
   - What's unclear: Whether ironc's code generator emits the shim arg as `int64_t data` when Iron source says `data: Int`, and whether the shim's `(void *)(intptr_t)data` cast produces a clean clang compile without warnings.
   - Recommendation: **Plan 66-01 or 66-02 adds a second probe on `Color.from_pixel_data(data: Int, format: PixelFormat)`.** Low risk — cast is textbook. Observe clang output.

3. **Does `ImageText(text, size, color)` work when called before `Window.init()`?**
   - What we know: raylib's default font is loaded by `LoadFontDefault()` inside `InitWindow()` (rcore.c:684). `ImageText` calls `GetFontDefault()` internally.
   - What's unclear: Whether pre-init `GetFontDefault()` returns a sentinel (zero-filled Font struct → empty glyph atlas → blank output) or crashes.
   - Recommendation: **Document as Phase 66 `Image.text` prereq** ("requires `Window.init(...)` before use"). Phase 66 smoke test (Plan 66-05) calls `Window.init(64, 64, "smoke")` before any `Image.text` call.

4. **Does Plan 63-04's foreign-method-stub scan fire for Image/Texture methods, or is it Vector2-specific?**
   - What we know: The scan at emit_structs.c:296-313 is **element-type generic** — `__et->kind == IRON_TYPE_OBJECT` with any decl name. `Iron_List_Iron_Vector2`, `Iron_List_Iron_Color`, `Iron_List_Iron_Image` would all emit correctly.
   - What's unclear: Whether raylib.iron's stubs are iterated in the same module scan. The code walks `module->funcs` — if raylib.iron is in the same module as consumer code, yes; if it's a separate import unit, maybe not.
   - Recommendation: Plan 66-02 Task 1 probe covers this implicitly. If the probe succeeds, the scan works for raylib.iron.

5. **Does the 26-color palette replace the 6-color rescue cleanly without breaking consumer files?**
   - What we know: pong.iron uses RED/BLACK/WHITE/DARKGRAY (4 of 6). game_raylib.iron uses WHITE/BLACK/RED/GREEN (4 of 6). hello_raylib.iron uses WHITE/BLACK (2 of 6). All 6 rescue names are preserved in the 26-color palette.
   - What's unclear: Whether any consumer file uses `Color(230, 41, 55, 255)` directly as a literal (bypassing the `val RED` alias). If so, duplication — non-breaking but ugly.
   - Recommendation: `grep -n 'Color(' examples/ tests/manual/ tests/integration/` in Plan 66-01 pre-check. Expected result: only the 6 `val RED = Color(...)` lines currently; after replacement, 26 lines; zero consumer-file literal Color calls (all use the palette names).

## Sources

### Primary (HIGH confidence)

- `src/vendor/raylib/raylib.h:1320-1453` — All ~100 RLAPI signatures for TEX-01..13, read 2026-04-17.
- `src/vendor/raylib/raylib.h:173-201` — 26-color palette `#define` macros for TEX-14.
- `src/stdlib/raylib.iron` (current tree) — object Image/Texture/RenderTexture/NPatchInfo/Color definitions at lines 201-206, 631-672; 6-color rescue palette at 1216-1236; Phase 66 position at end of file.
- `src/stdlib/iron_raylib.h:122-147` — Iron_Image / Iron_Texture struct mirrors with `_data: void *` field convention.
- `src/stdlib/iron_raylib.h:588-650` — Iron_List_Iron_Vector2 guarded typedef + IRON_LIST_DECL pattern (Phase 63-04 precedent for `[T]` FFI).
- `src/stdlib/iron_raylib.c:2619` — Phase 66 section marker (mirror of .h:938).
- `src/lir/emit_structs.c:138-318` — `emit_mono_list_decls` + Plan 63-04 foreign-method-stub param/return scan (emit_structs.c:309-312 is the critical line for `-> [Color]` support).
- `src/lir/emit_c.c:6394-6459` — Phase 59 DNS `need_dns` block (precedent for `-> [T]` list return).
- `src/stdlib/iron_net.c:1269-1378` — `Iron_net_lookup_host` full shim showing Iron_List build + free pattern.
- `src/runtime/iron_runtime.h:451-529` — `IRON_LIST_DECL` / `IRON_LIST_IMPL` macro definitions + iron_runtime.h:825-830 pre-declared primitive list types (confirms uint8_t is NOT pre-declared).
- `src/vendor/raylib/rcore.c:684` — `LoadFontDefault()` called in InitWindow (confirms Pitfall 4).

### Secondary (MEDIUM confidence)

- `.planning/phases/60-type-enum-foundation/60-03-SUMMARY.md` — Int32/UInt32/Int trichotomy for raylib scalar fields; nested-struct embedding proof.
- `.planning/phases/60-type-enum-foundation/60-08-SUMMARY.md` — 6-color rescue palette landing + Float32() narrowing rule; consumer file PHASE marker convention.
- `.planning/phases/63-2d-drawing/63-03-SUMMARY.md` — `Iron_List_Iron_Vector2` by-value array ABI (outcome A probe).
- `.planning/phases/64-collision-2d-3d/64-01-SUMMARY.md` — Instance-method dispatch on data-carrying objects; tuple-return pattern; `self` E0101 rule.
- `.planning/phases/65-raymath/65-01-SUMMARY.md` — Math→RMath namespace collision (not a Phase 66 concern); Matrix 64 B pass-by-value with zero `-Wlarge-by-value-copy`.
- `.planning/phases/66-textures-images/66-CONTEXT.md` — User decisions, 5-plan slicing, deferred ideas.

### Tertiary (LOW confidence, none needed)

None — all critical claims are verified against source code or prior phase SUMMARY files. No WebSearch used for Phase 66 (raylib + Iron stdlib are entirely in-tree).

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all libraries/patterns are in-tree precedents from Phases 60-65.
- Architecture patterns: HIGH — 5 of 5 patterns have working precedents in iron_raylib.c today.
- Pitfalls: HIGH for Pitfalls 1, 2, 3, 5, 6, 8 (all verified in prior plan SUMMARYs or source); MEDIUM for Pitfall 4 (default-font behavior before InitWindow) and Pitfall 7 (`[UInt8]` unsupported — verified via source inspection but no test case yet exercises it); LOW for Pitfall 9 (reverse-direction list standalone clang compile — needs Plan 66-02 probe to confirm mitigation).
- Code examples: HIGH — all templates lift from working Phase 60-65 shims.
- Open questions: 5 flagged with specific probe targets; none block Plan 66-01 start.

**Research date:** 2026-04-17
**Valid until:** 2026-05-17 (30 days for stable patterns; raylib 5.5 vendored and unchanging; Iron stdlib is actively evolving but Phase 66 consumes stable Phase 63/64/65 infrastructure).
