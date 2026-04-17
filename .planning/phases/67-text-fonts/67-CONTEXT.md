# Phase 67: Text & Fonts - Context

**Gathered:** 2026-04-17
**Status:** Ready for planning

<domain>
## Phase Boundary

Bind all rtext.c functions (~30) + raylib's 18 Text string utilities + the two Phase-66-deferred Image text calls as idiomatic Iron methods on `Font` / `GlyphInfo` / `Image`, plus `Text.*` and `Draw.*` namespaces. Closes requirements **TEXT-01..13**.

**Function-count breakdown (raylib 5.5, raylib.h lines 1456-1518):**

- **Font loading/unloading (TEXT-01..05):** 10 — `GetFontDefault`, `LoadFont`, `LoadFontEx`, `LoadFontFromImage`, `LoadFontFromMemory`, `IsFontValid`, `LoadFontData`, `GenImageFontAtlas`, `UnloadFontData`, `UnloadFont`
- **Font export (TEXT-06):** 1 — `ExportFontAsCode`
- **Text draw (TEXT-07, TEXT-08):** 6 — `DrawFPS`, `DrawText`, `DrawTextEx`, `DrawTextPro`, `DrawTextCodepoint`, `DrawTextCodepoints`
- **Line spacing (TEXT-09):** 1 — `SetTextLineSpacing`
- **Measure (TEXT-10):** 2 — `MeasureText`, `MeasureTextEx`
- **Glyph lookup (TEXT-11):** 3 — `GetGlyphIndex`, `GetGlyphInfo`, `GetGlyphAtlasRec`
- **UTF-8 / codepoint (TEXT-12):** 9 — `LoadUTF8`, `UnloadUTF8`, `LoadCodepoints`, `UnloadCodepoints`, `GetCodepointCount`, `GetCodepoint`, `GetCodepointNext`, `GetCodepointPrevious`, `CodepointToUTF8`
- **Text string utilities (TEXT-13):** 18 — `TextCopy`, `TextIsEqual`, `TextLength`, `TextFormat` (varargs — 3 overloads), `TextSubtext`, `TextReplace`, `TextInsert`, `TextJoin`, `TextSplit`, `TextAppend`, `TextFindIndex`, `TextToUpper`, `TextToLower`, `TextToPascal`, `TextToSnake`, `TextToCamel`, `TextToInteger`, `TextToFloat`
- **Phase 66 deferrals (TEX-05, TEX-07 residual):** 2 — `ImageTextEx`, `ImageDrawTextEx` (require `Font` type)

**Approximate total:** ~52 functions. Larger than Phase 64 (~20), smaller than Phase 66 (~100).

**In scope:** every RLAPI function in raylib.h's rtext section + the two Phase 66 Font-dependent Image calls.

**Out of scope:**
- New types. `object Font` (baseSize, glyphCount, glyphPadding, texture embedded by value, `_recs` / `_glyphs` opaque pointers) and `object GlyphInfo` (value, offsetX, offsetY, advanceX, `image` embedded Image) already defined Phase 60-03 with `_Static_assert` pins.
- `rlgl.h` low-level text calls — out of milestone per PROJECT.md.
- Font rendering beyond what raylib natively supports (no SDF generation logic; raylib handles it).
- Custom font atlas layouts. `GenImageFontAtlas` wraps raylib's default.
- Async font loading — raylib is synchronous.

</domain>

<decisions>
## Implementation Decisions

### Inherited from Phase 60-66 (locked — no re-discussion)

- **Binding architecture:** shim-only via the `/* ── Text & Fonts (Phase 67) ──────────── */` section marker that Plan 60-01 scaffolded in `iron_raylib.h` line 1203 and `iron_raylib.c`.
- **Method casing:** `snake_case`. `Font.load("x.ttf")`, `font.draw_ex(text, pos, size, spacing, tint)`, `Text.load_codepoints(s)`.
- **Method dispatch:** instance methods on data-carrying objects (Font, GlyphInfo) follow the Phase 64/66 pattern (`hir_to_lir.c:1096-1297`). Static constructors (`Font.default`, `Font.load`) use namespace form validated Phase 60-66.
- **`self` reserved** (E0101) — receivers use semantic names: `font`, `glyph`, `image`.
- **String arguments** — Iron `String` → C `const char *` compiler-managed marshalling (Phase 61 precedent).
- **Float32 vs Float:** raylib uses `float` — Iron `Float32` everywhere.
- **Struct-by-value:** Font (~40 B incl. Texture embed), GlyphInfo (~56 B incl. Image embed), Vector2/Rectangle/Color as before. All well under Phase 64's 120 B validated ceiling.
- **Opaque pointers:** Font._recs (Rectangle*) and Font._glyphs (GlyphInfo*) stay hidden behind the `_` prefix per Phase 60-03 convention. No direct user access.
- **Layout grid:** 393 `_Static_assert` entries through Phase 66; Font + GlyphInfo already pinned. No new assertions in Phase 67.
- **`Iron_List_<T>` auto-emission** (Phase 63-04 fix): foreign-method params with `[T]` auto-emit the typedef. Phase 67 exercises this for `[Int32]` (first time \u2014 INPUT for `Font.load_ex` codepoints and `font.draw_codepoints`; RETURN for `Text.load_codepoints`).
- **Enum-cast pattern:** `(int)type` for FontType param in `Font.from_memory`. Phase 62 precedent.
- **Memory discipline:** ironc ~10 GB per invocation — 1-2 smoke builds per plan.

### Phase 67 specific

#### Font static constructors \u2014 `Font.load / load_ex / from_image / from_memory / default`

Matches Phase 66 Image/Texture precedent exactly.

```iron
val f1 = Font.default()                                          -- GetFontDefault
val f2 = Font.load("assets/my.ttf")                              -- LoadFont
val f3 = Font.load_ex("assets/my.ttf", Int32(48), codepoints)    -- LoadFontEx, codepoints: [Int32]
val f4 = Font.from_image(image, key_color, Int32(32))            -- LoadFontFromImage
val f5 = Font.from_memory(".ttf", data, size, Int32(48), codepoints)  -- LoadFontFromMemory
```

#### Text draw API \u2014 Font instance methods + `Draw.*` shortcuts

- **Freestanding default-font draws** live on the `Draw` namespace (consistent with Phase 63):
  - `Draw.fps(x: Int32, y: Int32)` \u2192 `DrawFPS`
  - `Draw.text(text: String, x: Int32, y: Int32, size: Int32, color: Color)` \u2192 `DrawText`
- **Custom-font draws** are instance methods on `Font`:
  - `font.draw_ex(text: String, pos: Vector2, size: Float32, spacing: Float32, tint: Color)` \u2192 `DrawTextEx`
  - `font.draw_pro(text, pos, origin, rotation, size, spacing, tint)` \u2192 `DrawTextPro`
  - `font.draw_codepoint(cp: Int32, pos: Vector2, size: Float32, tint: Color)` \u2192 `DrawTextCodepoint`
  - `font.draw_codepoints(codepoints: [Int32], pos: Vector2, size: Float32, spacing: Float32, tint: Color)` \u2192 `DrawTextCodepoints`
- **Measure:** `Text.measure(text: String, size: Int32) -> Int32` (default font) + `font.measure_ex(text, size, spacing) -> Vector2` (custom font).
- **Line spacing:** `Text.set_line_spacing(spacing: Int32)` \u2192 `SetTextLineSpacing` (global state, lives on `Text.*` namespace).

#### Glyph lookup \u2014 all three as Font instance methods

- `font.get_glyph_index(cp: Int32) -> Int32` \u2192 `GetGlyphIndex`
- `font.get_glyph_info(cp: Int32) -> GlyphInfo` \u2192 `GetGlyphInfo` (GlyphInfo ~56 B struct-by-value return; validates Image-embedded struct crossing)
- `font.get_glyph_atlas_rec(cp: Int32) -> Rectangle` \u2192 `GetGlyphAtlasRec`

#### TEXT-13 \u2014 Bind ALL 18 string helpers under `Text.*` namespace for FFI parity

Even where Iron `String` has native equivalents. Rationale: TEXT-13 is an explicit requirement, and a uniform namespace means no per-function judgment about "which overlap." Iron users keep native String idioms; raylib-C-muscle-memory users get what they expect.

```iron
Text.copy(dst, src)                   -- TextCopy \u2192 Int32
Text.is_equal(a, b)                   -- TextIsEqual \u2192 Bool
Text.length(s)                        -- TextLength \u2192 UInt32
Text.subtext(s, position, length)     -- TextSubtext \u2192 String
Text.replace(s, find, repl)           -- TextReplace \u2192 String (shim copies + frees raylib buf)
Text.insert(s, insert, position)      -- TextInsert \u2192 String
Text.join(parts, delim)               -- TextJoin \u2192 String (parts: [String] \u2014 if feasible; fallback: omit, defer to Phase 73)
Text.split(s, delim)                  -- TextSplit \u2192 [String] (if feasible; fallback: defer)
Text.append(buf, s, position)         -- TextAppend (mutates buf) \u2014 return style TBD in Plan 67-04
Text.find_index(s, find)              -- TextFindIndex \u2192 Int32
Text.to_upper(s)                      -- TextToUpper \u2192 String
Text.to_lower(s)                      -- TextToLower \u2192 String
Text.to_pascal(s)                     -- TextToPascal \u2192 String
Text.to_snake(s)                      -- TextToSnake \u2192 String
Text.to_camel(s)                      -- TextToCamel \u2192 String
Text.to_integer(s)                    -- TextToInteger \u2192 Int32
Text.to_float(s)                      -- TextToFloat \u2192 Float32
```

**`TextFormat` (varargs) \u2014 3 fixed-arity overloads:**
- `Text.format_i(fmt: String, value: Int32) -> String`
- `Text.format_f(fmt: String, value: Float32) -> String`
- `Text.format_s(fmt: String, value: String) -> String`

Covers ~95% of real usage (single scalar interpolation). Richer formatting falls back to Iron String concat.

**Deferred sub-items (Plan 67-04 research will flag if blocker):** `Text.join([String])` and `Text.split(...) -> [String]` both depend on `Iron_List_Iron_String` auto-emit \u2014 if ironc lacks `[String]` array lowering, planner drops those two and notes as known gap.

#### TEXT-12 \u2014 Codepoint / UTF-8 ABI

- **`Text.load_codepoints(s: String) -> [Int32]`** \u2014 shim calls `LoadCodepoints`, copies into a fresh `Iron_List_Iron_Int32` (heap-alloc items, set count), calls `UnloadCodepoints` to free raylib's buffer. Single call, no user-facing unload. **First time `[Int32]` flows BACK from C to Iron.**
- **`Text.codepoint_next(s: String, offset: Int32) -> (Int32, Int32)`** \u2014 tuple return `(codepoint, byteSize)`. User iterates: `val (cp, step) = Text.codepoint_next(s, pos); pos = pos + step`. Same tuple pattern for `Text.codepoint_previous`.
- **`Text.codepoint_at(s: String, offset: Int32) -> (Int32, Int32)`** \u2014 `GetCodepoint`, same tuple shape as codepoint_next.
- **`Text.codepoint_count(s: String) -> Int32`** \u2014 `GetCodepointCount`.
- **`Text.load_utf8(codepoints: [Int32]) -> String`** \u2014 `LoadUTF8`. Shim copies raylib's `char *` into an Iron-managed String, then calls `UnloadUTF8`. No user-facing unload.
- **`Text.codepoint_to_utf8(cp: Int32) -> String`** \u2014 `CodepointToUTF8`. Shim copies the returned `const char *` (already raylib-owned static buf) into an Iron String.

#### Phase 66 deferrals \u2014 land in Plan 67-01

- `Image.text_ex(font: Font, text: String, size: Float32, spacing: Float32, tint: Color) -> Image` \u2192 `ImageTextEx`
- `image.draw_text_ex(font: Font, text: String, pos: Vector2, size: Float32, spacing: Float32, tint: Color) -> Image` \u2192 `ImageDrawTextEx` (mutating \u2014 returns new Image by value, same pattern as every other TEX-07 draw).

Both use the Phase 66 memcpy template. Once Font has static constructors (Task 1 of Plan 67-01), these slot in naturally and close the Phase 66 deferral markers.

#### Plan slicing \u2014 4 plans

- **Plan 67-01 \u2014 Font loading + Image text deferrals (TEXT-01..06 + Phase 66 TEX-05/07 residual):** Font.default / Font.load / Font.load_ex / Font.from_image / Font.from_memory / Font.unload / Font.is_valid / Font.export_as_code / Font.load_data / Font.gen_image_atlas / Font.unload_data + Image.text_ex + image.draw_text_ex. Wave 1. ~12 functions. First plan to exercise a Font struct-by-value crossing the FFI. Closes Phase 66's last two deferrals.
- **Plan 67-02 \u2014 Text draw + measure + glyph lookup (TEXT-07..11):** Draw.fps / Draw.text / Text.measure / Text.set_line_spacing + font.draw_ex / font.draw_pro / font.draw_codepoint / font.draw_codepoints / font.measure_ex / font.get_glyph_index / font.get_glyph_info / font.get_glyph_atlas_rec. Wave 2. depends_on 67-01. ~12 functions. First plan to return GlyphInfo by value across FFI (~56 B with Image embed).
- **Plan 67-03 \u2014 Codepoint + UTF-8 (TEXT-12):** Text.load_codepoints + Text.codepoint_* (5 queries) + Text.load_utf8 + Text.codepoint_to_utf8. Wave 3. depends_on 67-02 (shares Text.* namespace plumbing). ~8 functions. **Owns both novel ABI probes:** Task 1 probes `[Int32]` RETURN via Text.load_codepoints (Iron_List_Iron_Int32 auto-emit from foreign-method-stub return-type scan \u2014 extension of Phase 63-04 compiler fix). Task 2 probes Iron String allocation in shim via Text.codepoint_to_utf8 (shim mallocs Iron-owned copy of raylib's `const char *`).
- **Plan 67-04 \u2014 TEXT-13 string utilities + smoke test + pong re-enablement:** Text.copy / is_equal / length / 3 format_X overloads / subtext / replace / insert / join / split / append / find_index / 5 to_X case functions / to_integer / to_float. PLUS standalone `tests/manual/text_smoke.iron` exercising TEXT-01..13 via tagged sections (Phase 66-05 pattern). PLUS re-enable pong.iron's 2 `-- PHASE 67:` markers (score, GAME OVER) with real `Draw.text` calls. Wave 4. depends_on 67-03. ~20 functions + 2 consumer edits + 1 new test file.

#### Novel ABIs \u2014 both probed in Plan 67-03

- **`[Int32]` flowing BACK from C to Iron.** Phase 66 validated `[Color]` return path; Plan 67-03 Task 1 confirms ironc's foreign-method-stub return-type scan emits `Iron_List_Iron_Int32` for `Text.load_codepoints(s: String) -> [Int32]`. If the emit_structs.c scan only handles params and not returns, extend it (follow Phase 63-04 template).
- **Iron String allocation inside shim.** Phase 61 clipboard_get_text already does this for raylib's static `char *` return; Plan 67-03 Task 2 confirms the pattern works for raylib-owned `char *` (`LoadUTF8` returns caller-must-free memory) via shim `memcpy` + `UnloadUTF8`.

#### Smoke test \u2014 `tests/manual/text_smoke.iron` + pong re-enablement

- `text_smoke.iron` walks TEXT-01..13 via tagged sections, grep-countable per Phase 66-05 pattern. Uses `Font.default()` everywhere (no external TTF fixture \u2014 default font is embedded in raylib).
- Load path tested with intentional invalid path + `is_valid` guard (validates error path without shipping a TTF).
- pong.iron's 2 `-- PHASE 67:` markers restored:
  - Line 105: score display via `Draw.text(Text.format_i("%d", score), 200, 20, 40, fg)`.
  - Line 106: `Draw.text("GAME OVER", 300, 280, 40, accent)`.
- End-to-end `./build/ironc build examples/pong/pong.iron` validates full pipeline.

### Claude's Discretion

- **Exact method names where raylib doesn't map cleanly to snake_case.** E.g., `LoadFontFromImage` \u2192 `Font.from_image` vs `Font.load_from_image` \u2014 planner picks the readable form consistent with Phase 66 choices.
- **Font.load_data return shape.** raylib's `LoadFontData` returns `GlyphInfo *` with count. Iron: `Font.load_data(data: [UInt8], size: Int32, font_size: Int32, codepoints: [Int32], type: FontType) -> [GlyphInfo]`. First time `[GlyphInfo]` flows from C to Iron \u2014 planner probes in Plan 67-01 Task 2 or defers if ironc's List emit doesn't handle struct-element List returns.
- **Font.gen_image_atlas return.** raylib's `GenImageFontAtlas` writes `Rectangle **glyphRecs` via out-param. Iron: tuple return `(Image, [Rectangle])` or just `Image` with separate Font getter for recs. Planner picks; lean toward tuple for raylib-parity.
- **TextAppend mutating return.** raylib's `TextAppend` mutates `char *text` via in-place write + advances `int *position`. Iron side: either drop (Iron String is immutable) or wrap as `Text.append(s: String, insert: String, position: Int32) -> (String, Int32)` tuple return. Planner decides.
- **Bool coercion for `IsFontValid`** \u2014 shim pattern established Phase 62 (`(bool)(... != 0)`).
- **Font equality.** raylib has no FontIsEqual. Iron does not add one; Font comparison is not in-scope.
- **Error handling for Font.load on missing file.** raylib returns a Font with `baseSize=0` and prints a TRACELOG warning. Iron binding does the same \u2014 users check `font.is_valid()` before use. No exception.
- **Smoke test size.** Target ~200 lines like `texture_smoke.iron`. If closer to 400, split per-TEX-requirement.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Phase 60-66 foundation
- `.planning/phases/60-type-enum-foundation/60-03-SUMMARY.md` \u2014 Font / GlyphInfo layout; `_Static_assert` grid entries for Font.baseSize/glyphCount/glyphPadding, GlyphInfo.value/offsetX/offsetY/advanceX, embedded Image + Texture positions
- `.planning/phases/60-type-enum-foundation/60-07-SUMMARY.md` \u2014 FontType enum (DEFAULT/BITMAP/SDF) with ordinal anchors
- `.planning/phases/63-2d-drawing/63-03-SUMMARY.md` \u2014 `Iron_List_<T>` by-value array ABI; Plan 63-04 compiler fix extended the scan; Plan 67-03 may further extend for Iron_List return from foreign-method stubs
- `.planning/phases/64-collision-2d-3d/64-01-SUMMARY.md` \u2014 instance methods on data-carrying objects; tuple-return auto-emit template (`Iron_Tuple_Int32_Int32` for codepoint_next/previous)
- `.planning/phases/65-raymath/65-CONTEXT.md` \u2014 namespace rename precedent (RMath); not directly applied to Phase 67 but informs the Text.* namespace decision
- `.planning/phases/66-textures-images/66-CONTEXT.md` \u2014 Phase 66 decisions; specifically the TEX-05 mutating-return-by-value pattern that Image.draw_text_ex reuses
- `.planning/phases/66-textures-images/66-03-SUMMARY.md` \u2014 mutating-transform return-by-value shim pattern (Image + memcpy + raylib mutate + memcpy-out)
- `.planning/phases/66-textures-images/66-04-SUMMARY.md` \u2014 Texture struct-by-value crossing template; Plan 67-01 reuses for Font
- `.planning/phases/66-textures-images/66-05-SUMMARY.md` \u2014 standalone smoke-test layout with tagged sections; Plan 67-04 follows same pattern

### raylib upstream (rtext.c section)
- `src/vendor/raylib/raylib.h` lines 306-323 \u2014 `GlyphInfo` and `Font` C struct definitions (layout ground truth for Phase 60's assertions)
- `src/vendor/raylib/raylib.h` lines 889-894 \u2014 `FontType` enum (DEFAULT/BITMAP/SDF) used by `Font.from_memory`
- `src/vendor/raylib/raylib.h` lines 1456-1470 \u2014 Font loading/unloading (TEXT-01..06)
- `src/vendor/raylib/raylib.h` lines 1472-1486 \u2014 Text drawing + measurement + glyph lookup (TEXT-07..11)
- `src/vendor/raylib/raylib.h` lines 1488-1497 \u2014 Codepoint + UTF-8 utilities (TEXT-12)
- `src/vendor/raylib/raylib.h` lines 1500-1518 \u2014 Text string utilities (TEXT-13)
- `src/vendor/raylib/raylib.h` lines 1352, 1407-1408 \u2014 `ImageTextEx`, `ImageDrawTextEx` (Phase 66 deferrals being closed)
- `src/vendor/raylib/rtext.c` \u2014 all implementations (reference only; raylib builds it)

### Iron stdlib precedents
- `src/stdlib/iron_raylib.h` line 1203 \u2014 `/* \u2500\u2500 Text & Fonts (Phase 67) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 */` pre-scaffolded section marker (Plan 60-01)
- `src/stdlib/iron_raylib.c` line 3319 \u2014 `/* ImageDrawTextEx DEFERRED to Phase 67: requires Font type */` marker in the Image section (Plan 66-03)
- `src/stdlib/raylib.iron` lines 676-695 \u2014 `object GlyphInfo` and `object Font` declarations (Plan 60-03)
- `src/stdlib/iron_raylib.c` \u2014 Image section shims (Plan 66-01..05) as template for mutating-return-by-value; Draw section shims (Plan 63-01..04) as template for `Draw.*` freestanding functions
- `examples/pong/pong.iron` lines 105-106 \u2014 two `-- PHASE 67:` DrawText markers awaiting re-enablement (Plan 67-04 Task 3)

### Project-level specs
- `.planning/REQUIREMENTS.md` lines 149-161 \u2014 TEXT-01..13 detailed descriptions (source of truth for the 52-function surface)
- `.planning/ROADMAP.md` lines 223-238 \u2014 Phase 67 goal + 5 success truths
- `.planning/PROJECT.md` \u2014 milestone scope, API style guidance (method-on-type where idiomatic, typed enums at every call site)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`object Font` and `object GlyphInfo`** at `src/stdlib/raylib.iron:676-695` \u2014 layout pinned by Phase 60-03 `_Static_assert` grid. Phase 67 adds methods; no layout changes.
- **`object Image` / `object Texture` / `object Color`** \u2014 Phase 66 methods (Image.text default-font variant already bound; Image.text_ex deferred) \u2014 Plan 67-01 extends Image with `text_ex` and `draw_text_ex`.
- **Pre-scaffolded `/* \u2500\u2500 Text & Fonts (Phase 67) \u2500\u2500 */` section markers** in both `iron_raylib.h` and `iron_raylib.c` \u2014 shims land directly under each.
- **`Draw.*` namespace** (Phase 63) \u2014 Plan 67-02 adds `Draw.fps` and `Draw.text` shortcuts alongside existing 65 Draw.* bindings.
- **FontType enum** (Phase 60-07) \u2014 used by `Font.from_memory(type: String, ...)` shim-internal cast (if FontType exposed) or as raw String-based extension detection.
- **`Iron_List_Iron_<T>` auto-emit** (Phase 63-04 compiler fix) \u2014 Plan 67-01 uses `Iron_List_Iron_Int32` for codepoints input; Plan 67-03 extends the scan (if needed) for `[Int32]` RETURN from foreign-method stubs.
- **Iron String allocation in shim** (Phase 61 clipboard precedent) \u2014 Plan 67-03 Task 2 confirms the pattern for raylib-owned `char *` from `LoadUTF8`.

### Established Patterns
- **Memcpy template** for struct-by-value args/returns. Font ~40 B, GlyphInfo ~56 B \u2014 both within Phase 64's validated 120 B ceiling.
- **Mutating-return-by-value** (Phase 66-03) for `image.draw_text_ex(font, ...) -> Image` \u2014 shim does `Image local; memcpy; ImageDrawTextEx(&local, ...); memcpy-out`.
- **Tuple auto-emit** (Phase 64 `Collision.lines`, Phase 65 `MatrixDecompose`) for `(Int32, Int32)` codepoint iteration returns.
- **Static constructor + instance method mix** (Phase 66 precedent) for Font \u2014 `Font.load("x.ttf")` namespace form, `font.draw_ex(...)` instance form.
- **Enum-param cast** (`(int)type`) for FontType in Font.from_memory shim body.
- **Bool coercion** (`(bool)(... != 0)`) for IsFontValid.

### Integration Points
- `src/stdlib/iron_raylib.c` Text & Fonts section \u2014 ~52 shims under the Phase 67 marker
- `src/stdlib/iron_raylib.h` Text & Fonts section \u2014 ~52 prototypes (auto-emitted)
- `src/stdlib/iron_raylib.c` Image section \u2014 2 late additions for Phase 66 deferrals (ImageTextEx, ImageDrawTextEx)
- `src/stdlib/iron_raylib.c` Draw section \u2014 2 additions for default-font draws (Draw.fps, Draw.text)
- `src/stdlib/raylib.iron` \u2014 extend Font + GlyphInfo + Image objects with methods; add `object Text {}` namespace stub (Plan 67-02 or 67-03)
- `tests/manual/text_smoke.iron` \u2014 new standalone smoke (Plan 67-04)
- `examples/pong/pong.iron` lines 105-106 \u2014 restore 2 `-- PHASE 67:` DrawText markers (Plan 67-04)

</code_context>

<specifics>
## Specific Ideas

- **Font constructors** mirror Phase 66 Image naming: `Font.load`, `Font.load_ex`, `Font.from_image`, `Font.from_memory`, `Font.default()`.
- **Default-font draws are freestanding on `Draw.*`**; custom-font draws are instance methods on `Font` \u2014 carves the natural receiver split.
- **`Draw.text` not `Text.draw`** \u2014 freestanding draws live where all other freestanding draws live (Phase 63 convention), not under Text namespace.
- **GlyphInfo-by-value return validates Image-embedded struct crossing** \u2014 GlyphInfo embeds Image (40 B) at an offset; ~56 B total. Within Phase 64's ceiling but the first time Image has been embedded inside another returned struct.
- **`font.draw_codepoints(codepoints: [Int32], ...)` is the first `[Int32]` INPUT param in the raylib binding.** Confirms `Iron_List_Iron_Int32` auto-emit works identically to `[UInt8]` / `[Color]` / `[Vector2]`.
- **Tuple return for iteration:** `Text.codepoint_next(s, offset) -> (Int32, Int32)` yields `(codepoint, byteSize)`. Same for `codepoint_previous` and `codepoint_at`. User advances position manually.
- **Text.load_utf8 / codepoint_to_utf8 return Iron String; shim owns raylib buffer lifecycle.** No user-facing unload. Matches Phase 61 clipboard_get_text pattern.
- **Text.load_codepoints returns [Int32]; shim owns raylib buffer lifecycle.** No user-facing UnloadCodepoints. Matches Phase 66 pattern for `[Color]` returns.
- **TextFormat \u2192 three fixed-arity overloads** (`Text.format_i`, `format_f`, `format_s`). Covers most real use; richer cases fall back to Iron String.
- **TEXT-13 all 18 helpers bound under `Text.*` for FFI parity**, even where Iron String has equivalents.
- **`Text.join([String])` and `Text.split(...) -> [String]` conditional** on `Iron_List_Iron_String` auto-emit working; if it doesn't, planner drops those two as known gaps.
- **Phase 66 deferrals close here:** `Image.text_ex(font, ...)` and `image.draw_text_ex(font, ...)` land in Plan 67-01 once Font constructors exist.
- **pong.iron's 2 `-- PHASE 67:` markers restored** in Plan 67-04 via `Draw.text` + `Text.format_i` \u2014 end-to-end build validates the default-font path in a real user program.
- **Smoke test uses `Font.default()` only** \u2014 no TTF binary asset shipped. Font.load path tested with intentional invalid path + is_valid guard.
- **Memory ownership for loaded fonts:** raylib owns Font GPU memory (atlas texture) and RAM (glyph arrays). Iron does not auto-unload on scope exit (no Drop). Users call `font.unload()` explicitly \u2014 documented in raylib.iron header.

</specifics>

<deferred>
## Deferred Ideas

- **Automatic `Drop` / RAII for Font.** Iron has no Drop trait. Font cleanup via explicit `.unload()`. Phase 73 API polish could revisit.
- **`Font.equals` / structural comparison.** raylib has no FontIsEqual; Iron does not add one.
- **SDF font generation logic.** raylib's LoadFontEx handles it internally when size > 32; no Iron-side SDF tuning.
- **Text iterator type.** `Text.codepoints(s)` as an iterator yielding `(cp, offset)` pairs would be cleanest \u2014 requires Iron iterator plumbing (Phase 73+).
- **Rich text formatting.** `TextFormat` beyond single-scalar overloads requires va_args FFI \u2014 out of scope.
- **`Text.join([String])` / `Text.split -> [String]`** \u2014 conditional on `Iron_List_Iron_String` auto-emit. If blocked, deferred to Phase 73 or a later compiler improvement.
- **Ship a public-domain TTF** for `Font.load` path smoke testing. Uses Font.default() for now.
- **Font atlas customization beyond raylib's built-in `GenImageFontAtlas`.** Custom packers / layouts out of scope.
- **Async font loading / streaming font updates.** raylib is synchronous.
- **Font kerning / ligature APIs.** raylib doesn't expose them; neither does Iron.

</deferred>

---

*Phase: 67-text-fonts*
*Context gathered: 2026-04-17*
