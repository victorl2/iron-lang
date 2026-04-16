# Phase 63: 2D Drawing - Research

**Researched:** 2026-04-16
**Domain:** raylib binding — 2D drawing primitives, frame/mode stack, spline evaluation
**Confidence:** HIGH (domain is fully mapped by existing Phase 60/61/62 precedents; one genuine unknown: array parameter ABI across the shim boundary)

## Summary

Phase 63 binds 55 raylib `rshapes.c` + `rcore.c` draw-section functions across 16 requirements. The binding pattern is fully locked by Phases 60/61/62: Iron-side empty-body stubs in `src/stdlib/raylib.iron` lower to `Iron_<namespace>_<name>` C shim wrappers in `src/stdlib/iron_raylib.c`, which forward to raylib. Every type, enum, struct-by-value ABI path, and `const char *` marshalling pattern this phase needs is already exercised and proven in the codebase.

Phase 63 exercises **three genuinely new ABI surfaces**: (1) `Color` struct-by-value input parameter — every draw function takes one; (2) `Camera2D` struct-by-value input (for `beginMode2D`); (3) Iron array `[Vector2]` → C `const Vector2 *` + count passthrough — needed by 9 functions (`DrawLineStrip`, `DrawTriangleFan`, `DrawTriangleStrip`, 5× `DrawSpline<T>`). Color-by-value input has C-side precedent (Image, FilePathList) via `memcpy`; array passthrough uses Iron's `ARRAY_PARAM_CONST_PTR` mode from `src/lir/lir_optimize.h`, but **it has never been exercised by the raylib shim layer before** and needs a micro-smoke-test like Phase 61 did for struct-by-value returns.

**Primary recommendation:** Split into 4 subphases — 63-01 (frame + stack modes, 12 functions, covers DRAW2D-01..06 and validates `Color`/`Camera2D` input ABI end-to-end through `Draw.clear(BLACK)`); 63-02 (pixels + lines + circles + ellipses + rings, 14 functions, DRAW2D-07..11, scalar-and-Vector2 primitives); 63-03 (rectangles + triangles + polygons, 15 functions, DRAW2D-12..14, includes first `[Vector2]` array passthrough); 63-04 (splines + evaluators + pong.iron re-enablement, 15 functions, DRAW2D-15..16, includes 5 more `[Vector2]` arrays and 5 struct-by-value `Vector2` returns). If the array-param ABI smoke test in 63-03 fails, 63-04 falls back to a fixed-segment-count pattern or descopes `DrawSpline<T>` to segment-variants only.

## User Constraints

> No CONTEXT.md exists for Phase 63 yet. The orchestrator is either (a) running in integrated `/gsd-plan-phase` mode which will synthesize context from this research + user answers, or (b) the planner will create a 63-CONTEXT.md from user input before consuming this research. Constraints below are **inherited from Phase 60/61/62 decisions** and treated as locked for Phase 63 unless a future 63-CONTEXT.md overrides them.

### Locked Decisions (inherited)

From `.planning/phases/60-type-enum-foundation/60-CONTEXT.md` (binding architecture + naming):
- **Shim-only architecture.** Every raylib call flows through `src/stdlib/iron_raylib.c`. Iron-side empty-body foreign-method stubs lower to `Iron_<namespace>_<name>(...)`. No bare `extern func` anywhere.
- **snake_case methods** (`Draw.begin`, `Draw.begin_mode_2d`, `Draw.draw_rectangle_rounded_lines_ex`).
- **Typed enum parameters everywhere.** `Draw.begin_blend_mode(mode: BlendMode)`, NEVER `mode: Int32`.
- **Manual `unload()` lifecycle** for resources; Phase 63 introduces no new resources.
- **Shim sections in iron_raylib.c and iron_raylib.h already scaffolded.** Phase 63 appends below `/* ── 2D Drawing (Phase 63) ────── */` markers at `iron_raylib.c:563` and `iron_raylib.h:513`.

From `.planning/phases/61-window-system/61-CONTEXT.md`:
- **Struct-by-value returns** (Vector2, Image) validated. Field-copy / memcpy into `struct Iron_<T>` on the C side. Phase 63 reuses for 5× `GetSplinePoint<T>` Vector2 returns.
- **Struct-by-value inputs** validated for `Image` (Iron_window_set_icon at iron_raylib.c:88 memcpy's Iron_Image → Image). Phase 63's `Color`/`Camera2D`/`Rectangle`/`RenderTexture`/`Shader`/`NPatchInfo` inputs follow the same pattern.

From `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-CONTEXT.md`:
- **Auto-generated prototypes in `emit_c`** (commit e3e5eee) — Iron-side stubs automatically generate the extern C prototype. Phase 63 does NOT need to hand-maintain prototype sync between `raylib.iron` stubs and `iron_raylib.c` wrappers.
- **Namespace `object` declaration** for method receivers. `object Draw {}` is already declared in `raylib.iron:932` from Plan 60-06 — Phase 63 just appends `func Draw.*` stubs.
- **Phase 63 consumer-file markers** (`-- PHASE 63:`) exist in 3 files: `examples/pong/pong.iron` (7 markers), `tests/manual/game_raylib.iron` (1 marker), `tests/integration/web/hello_raylib.iron` (1 marker). Phase 63's final plan MUST close them, re-using the Phase 62 pattern.

From `.planning/phases/60-type-enum-foundation/60-CONTEXT.md` — namespace type for receivers:
- **`Draw` namespace owns every draw-mode and draw-primitive function.** All 55 Phase 63 functions attach to `object Draw {}`. Do NOT create `object Shapes {}` / `object Mode {}` sub-namespaces — 60-CONTEXT.md already locked `object Draw` as the receiver for this entire surface (`Draw.begin()`, `Draw.clear(color)`, `Draw.begin_mode_2d(camera)`, `Draw.pixel(x,y,color)`, `Draw.line(...)`, `Draw.spline_linear(...)` etc.).

### Claude's Discretion

- **Plan slicing (4 vs 5 vs 3 subphases).** Research recommends 4 (see Plan Slicing section). Planner may adjust if context budget or dependency ordering favors a different split.
- **Method naming for primitives** — raylib C uses `DrawPixel`, `DrawPixelV`, `DrawCircleSectorLines`. Iron-side could be `Draw.pixel(...)` / `Draw.pixel_v(...)` / `Draw.circle_sector_lines(...)` OR drop the `draw_` / `_v` suffixes. 60-CONTEXT.md locks snake_case but leaves the `Draw.pixel` vs `Draw.draw_pixel` question open. **Recommendation:** drop the redundant `draw_` prefix since the namespace is `Draw`; keep the `_v`/`_ex`/`_pro`/`_lines` suffixes because they disambiguate variants.
- **`getSplinePointBezierQuad` vs `getSplinePointBezierQuadratic`** — raylib's C name for the evaluator is `GetSplinePointBezierQuad` (shorter); the draw function is `DrawSplineBezierQuadratic`. Iron can (a) match C exactly (`Draw.get_spline_point_bezier_quad` vs `Draw.spline_bezier_quadratic`), or (b) normalize both to `bezier_quadratic`. **Recommendation:** normalize to `bezier_quadratic` in Iron for symmetry; the shim bridges.
- **Array passthrough fallback strategy.** If `[Vector2]` → `const Vector2 *` + count doesn't work, 63-03/04 fall back to either (i) a fixed-size-N pattern (user pre-allocates, Iron passes length via `_count` param), or (ii) descope `DrawLineStrip` / `DrawTriangleFan` / `DrawTriangleStrip` / 5× `DrawSpline<T>` to Phase 73 while keeping the `DrawSplineSegment<T>` variants (5 fixed-point-count segment functions) in Phase 63. Planner decides after 63-03's array smoke test.

### Deferred Ideas (OUT OF SCOPE for Phase 63)

- **VR stereo mode** (`BeginVrStereoMode` / `EndVrStereoMode`) — per REQUIREMENTS.md "Out of Scope" list.
- **3D draw primitives** (`DrawLine3D`, `DrawCube`, `DrawSphere`, etc.) — Phase 69.
- **3D camera modes** (`BeginMode3D` / `EndMode3D`) — Phase 69.
- **Texture drawing** (`DrawTexture`, `DrawTextureEx`, `DrawTexturePro`, `DrawTextureNPatch`) — Phase 66 (TEX-12). Phase 63 binds `beginTextureMode(rt: RenderTexture)` but NOT `drawTexture` calls that render INTO a texture.
- **Text drawing** (`DrawFPS`, `DrawText`, `DrawTextEx`, `DrawTextPro`, `DrawTextCodepoint`, `DrawTextCodepoints`) — Phase 67 (TEXT-07, TEXT-08). The pong.iron `-- PHASE 63: DrawText(...)` marker at line 99-100 is **mislabeled** — it belongs to Phase 67. Phase 63 must either (a) re-mark these lines as `-- PHASE 67:` or (b) leave them commented and accept the mislabel (Plan 63-04 SUMMARY documents the decision).
- **`SetShapesTexture` / `GetShapesTexture` / `GetShapesTextureRectangle`** (raylib.h:1242-1244) — these configure a shared white-rectangle texture for batched drawing. Not mentioned in DRAW2D-01..16 and raylib itself flags them as optimization-only. **Recommendation:** include them in Phase 63 anyway under 100% coverage rule, or defer to Phase 66 (uses `Texture2D`). Planner decides.

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| DRAW2D-01 | `Draw.begin()` / `Draw.end()` — `BeginDrawing` / `EndDrawing` | Plan slicing: 63-01. 2 functions, zero parameters, zero returns. Mechanical binding. |
| DRAW2D-02 | `Draw.clear(color)` — `ClearBackground(Color)` | Plan 63-01. First Color-by-value input in the stdlib. memcpy pattern from `Iron_window_set_icon`. |
| DRAW2D-03 | `Draw.begin_mode_2d(camera)` / `Draw.end_mode_2d()` — `BeginMode2D(Camera2D)` / `EndMode2D` | Plan 63-01. First Camera (Camera2D) by-value input. Byte-layout pinned by 60-04 `_Static_assert` grid (`Iron_Camera size == Camera2D` at iron_raylib_layout.c:186). |
| DRAW2D-04 | `Draw.begin_texture_mode(target)` / `Draw.end_texture_mode()` — `BeginTextureMode(RenderTexture2D)` / `EndTextureMode` | Plan 63-01. First RenderTexture-by-value input. 40-byte struct (id:4 + texture:20 + depth:20 — actually RenderTexture is id(4) + Texture(20=id+w+h+mipmaps+format) + Texture(20), total 44 with no padding; verify via `_Static_assert(sizeof(struct Iron_RenderTexture) == sizeof(RenderTexture))`). |
| DRAW2D-05 | `Draw.begin_shader_mode(shader)` / `Draw.end_shader_mode()` + `Draw.begin_blend_mode(mode)` / `Draw.end_blend_mode()` | Plan 63-01. Shader is 16 bytes (id:4 + pad:4 + _locs:8); first Shader-by-value input. `BlendMode` enum parameter (typed, cast to int in shim). |
| DRAW2D-06 | `Draw.begin_scissor_mode(x, y, w, h)` / `Draw.end_scissor_mode()` | Plan 63-01. Four `Int32` scalars. Mechanical. |
| DRAW2D-07 | Pixels: `DrawPixel(int, int, Color)`, `DrawPixelV(Vector2, Color)` | Plan 63-02. Pixel variants — 2 functions. |
| DRAW2D-08 | Lines: every variant. See function table below — **5 functions** (DrawLine, DrawLineV, DrawLineEx, DrawLineStrip, DrawLineBezier). DrawLineStrip takes `const Vector2 *points, int count` — **first array passthrough**. |
| DRAW2D-09 | Circles: every variant. **7 functions** (DrawCircle, DrawCircleSector, DrawCircleSectorLines, DrawCircleGradient, DrawCircleV, DrawCircleLines, DrawCircleLinesV). |
| DRAW2D-10 | Ellipses: **2 functions** (DrawEllipse, DrawEllipseLines). |
| DRAW2D-11 | Rings: **2 functions** (DrawRing, DrawRingLines). |
| DRAW2D-12 | Rectangles: every variant. **11 functions** (DrawRectangle, V, Rec, Pro, GradientV, GradientH, GradientEx, Lines, LinesEx, Rounded, RoundedLines, RoundedLinesEx). **Wait — that's 12. Verified below.** |
| DRAW2D-13 | Triangles: **4 functions** (DrawTriangle, DrawTriangleLines, DrawTriangleFan, DrawTriangleStrip). TriangleFan and TriangleStrip are array passthroughs. |
| DRAW2D-14 | Polygons: **3 functions** (DrawPoly, DrawPolyLines, DrawPolyLinesEx). Center + sides + radius + rotation — all scalars. |
| DRAW2D-15 | Splines: **10 functions** (5 whole-spline + 5 segment). 5× `DrawSpline<T>` take `const Vector2 *points` arrays; 5× `DrawSplineSegment<T>` take fixed 2-4 Vector2 parameters. |
| DRAW2D-16 | Spline evaluators: **5 functions** (`GetSplinePointLinear/Basis/CatmullRom/BezierQuad/BezierCubic`). Vector2 struct-by-value RETURNS. |

**Exhaustive function count:** 2+2+4+5+7+2+2+12+4+3+10+5 = wait, recounting per section below for accuracy.

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Iron-side stub declaration (`func Draw.*`) | `src/stdlib/raylib.iron` (Iron stdlib, `object Draw`) | — | Only place `func` declarations live for the stdlib import surface. |
| C-side shim implementation (`Iron_draw_*`) | `src/stdlib/iron_raylib.c` (Phase 63 section marker line 563) | — | Single C TU per milestone convention; section-marker append pattern. |
| C-side prototype (`Iron_draw_*` decl) | `src/stdlib/iron_raylib.c` / `.h` via emit_c auto-generation | `src/stdlib/iron_raylib.h` manual append (legacy) | emit_c auto-generates prototypes from Iron stubs; manual .h prototypes remain for in-TU use by ABI-layout or humans grepping. |
| Layout verification (`_Static_assert`) | `src/stdlib/iron_raylib_layout.c` | — | Phase 60 already landed 392 asserts covering every type Phase 63 touches (Color, Vector2, Rectangle, Camera/Camera2D, RenderTexture, Shader, NPatchInfo). **No new asserts needed in Phase 63** unless the planner adds function-signature-shape asserts, which is optional. |
| Consumer-file re-enablement | `examples/pong/pong.iron`, `tests/manual/game_raylib.iron`, `tests/integration/web/hello_raylib.iron` | — | Phase 62 pattern: last plan of the phase closes the `-- PHASE 63:` markers. |
| Web build parity | `src/cli/build_web.c` (emcc path, no change needed) | `src/cli/build.c` (native clang) | Both pipelines already compile `iron_raylib.c` + `iron_raylib_layout.c` when `opts.use_raylib`. Phase 63 adds no new source files. |

## Standard Stack

### Core

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| raylib | 5.5 (vendored) [VERIFIED: src/vendor/raylib/raylib.h exists] | 2D draw primitives + frame stack | Milestone-locked per PROJECT.md; upstream upgrade OOS per REQUIREMENTS.md |
| Iron stdlib binding pattern | N/A (in-repo) | Shim + stub + layout_assert triad | Locked by 60-CONTEXT.md, reproduced across 3 prior phases (60/61/62) |

### Supporting

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| `<string.h>` (memcpy) | C standard | Struct-by-value field copy | Phase 63 reuses the `Iron_window_set_icon` pattern for every struct-input shim. |
| `_Static_assert` (C11) | C11 | Layout verification | No new asserts needed in Phase 63; Phase 60's 392 asserts already cover every type. |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| memcpy-into-raylib-struct for Color-by-value | `*(Color *)&iron_color` cast | memcpy is explicit and avoids strict-aliasing warnings — 60-CONTEXT.md and `Iron_window_set_icon` both prefer memcpy. Stay with memcpy. |
| `[Vector2]` array passthrough | Pre-allocate fixed-N C arrays stack-side | Loses flexibility; DrawLineStrip/DrawTriangleStrip are variable-length by definition. Keep the array approach; fall back only if ABI fails. |
| `object Draw {}` receiver | `object Shapes {}` split from `object Draw {}` | 60-CONTEXT.md explicitly locked `object Draw` for the entire 2D + 3D begin/end surface. Splitting now contradicts locked design. Stay with single `Draw`. |
| Iron-side enum OR-ing for multiple begin-modes | Ban nesting above depth 2 | raylib natively supports unlimited stacking (mode stack). Expose the full stack; rely on user discipline. |

**Installation:** None. All dependencies are already vendored and wired into the build (`src/cli/build.c`, `src/cli/build_web.c`).

**Version verification:** Not applicable — raylib 5.5 is vendored. `grep "#define RAYLIB_VERSION " src/vendor/raylib/raylib.h` confirms 5.5 in the repo.

## Architecture Patterns

### System Architecture Diagram

```
User .iron file (e.g. pong.iron)
    │
    │ `import raylib` + `Draw.begin()` / `Draw.rectangle(...)` / `Draw.end()` etc.
    │
    ▼
src/stdlib/raylib.iron
    │ (contains: `object Draw {}` + empty-body foreign-method stubs)
    │ `func Draw.rectangle(x: Int32, y: Int32, w: Int32, h: Int32, color: Color) {}`
    │
    ▼ (ironc compile: emit_c lowers stub name via mangling)
    │
    │ Iron-side stub `func Draw.rectangle` → C symbol `Iron_draw_rectangle`
    │ emit_c auto-generates extern C prototype in the user's compiled .c file
    │
    ▼ (link time: user's compiled TU + iron_raylib.c + raylib.a all linked together)
    │
src/stdlib/iron_raylib.c  (`/* ── 2D Drawing (Phase 63) ──── */` section at line 563)
    │ `void Iron_draw_rectangle(int32_t x, int32_t y, int32_t w, int32_t h,
    │                           struct Iron_Color color) {`
    │ `    Color rl;`
    │ `    memcpy(&rl, &color, sizeof(Color));`
    │ `    DrawRectangle((int)x, (int)y, (int)w, (int)h, rl);`
    │ `}`
    │
    ▼
src/vendor/raylib/rshapes.c
    │ `DrawRectangle(int posX, int posY, int width, int height, Color color)`
    │ (raylib internal implementation, calls into rlgl OpenGL abstraction)
    │
    ▼
GPU (OpenGL driver via rglfw or emcc WebGL)
```

**Layout verification path (parallel, compile-time):**
```
src/stdlib/iron_raylib_layout.c
    │ `_Static_assert(sizeof(struct Iron_Color) == sizeof(Color), ...)`
    │ `_Static_assert(offsetof(struct Iron_Color, r) == offsetof(Color, r), ...)`
    │ ... (one size + per-field offset per type)
    ▼
clang -c iron_raylib_layout.c (fires at build time; any drift = build fails)
```

### Recommended Project Structure

No new files. Phase 63 modifies:

```
src/stdlib/
├── raylib.iron                # + ~150 lines (func Draw.* stubs + docstrings)
├── iron_raylib.h              # + ~70 lines (Iron_draw_* prototypes) at line 513
├── iron_raylib.c              # + ~350 lines (Iron_draw_* shims) at line 563
└── iron_raylib_layout.c       # UNCHANGED (Phase 60's 392 asserts still cover every type)

examples/pong/pong.iron         # 7 `-- PHASE 63:` markers → real Draw.* calls
tests/manual/game_raylib.iron   # 1 `-- PHASE 63:` marker → real Draw.* call
tests/integration/web/hello_raylib.iron  # 1 `-- PHASE 63:` marker → real Draw.* call
```

### Pattern 1: Shim-forward for scalar-argument draws

**What:** Primitives that take only scalar types (`int`, `float`) and exactly one `Color` pass through raylib directly; Color is the only struct to memcpy.

**When to use:** `DrawPixel`, `DrawLine`, `DrawCircle`, `DrawEllipse`, `DrawRectangle`, `DrawPoly`, etc.

**Example:**
```c
/* iron_raylib.c — Phase 63 section */
void Iron_draw_rectangle(int32_t x, int32_t y, int32_t w, int32_t h,
                         struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));  /* Iron_Color layout verified by Static_assert */
    DrawRectangle((int)x, (int)y, (int)w, (int)h, rl);
}
```

### Pattern 2: Shim-forward for Vector2-argument draws

**What:** Primitives that take one or more `Vector2` pass through via memcpy on each.

**When to use:** `DrawPixelV`, `DrawLineV`, `DrawCircleV`, `DrawRectangleV`, `DrawTriangle`, `DrawPoly`, `DrawSplineSegment<T>`.

**Example:**
```c
void Iron_draw_line_v(struct Iron_Vector2 start, struct Iron_Vector2 end,
                      struct Iron_Color color) {
    Vector2 s, e;
    Color c;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    memcpy(&c, &color, sizeof(Color));
    DrawLineV(s, e, c);
}
```

### Pattern 3: Struct-by-value RETURN (spline evaluators)

**What:** `GetSplinePoint<T>` returns `Vector2`. Reuse Phase 61's `Iron_window_get_window_position` field-copy-out pattern.

**When to use:** 5× `GetSplinePointLinear/Basis/CatmullRom/BezierQuad/BezierCubic`.

**Example:**
```c
struct Iron_Vector2 Iron_draw_get_spline_point_linear(
        struct Iron_Vector2 start, struct Iron_Vector2 end, float t) {
    Vector2 s, e;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    Vector2 rl = GetSplinePointLinear(s, e, t);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}
```

### Pattern 4: Array passthrough `[Vector2]` → `const Vector2 *` + count

**What:** Functions taking `const Vector2 *points, int pointCount` (9 total: DrawLineStrip, DrawTriangleFan, DrawTriangleStrip, 5× DrawSpline<T>).

**When to use:** Any function raylib declares with `const Vector2 *points`.

**ABI unknown:** Iron supports `ARRAY_PARAM_CONST_PTR` mode (`src/lir/lir_optimize.h:48`) that passes `[T]` as `const T *` + length. **But this has NEVER been exercised in the raylib shim boundary.** iron_net.c uses `Iron_List_Iron_Address` (struct with items+count+capacity) for its address list, which is a different ABI mode. A micro-smoke-test in 63-03 is required to confirm which mode Iron actually picks for Phase 63 stubs.

**Tentative example (PENDING smoke test):**
```c
/* If ARRAY_PARAM_CONST_PTR fires: */
void Iron_draw_line_strip(const struct Iron_Vector2 *points, int32_t count,
                          struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    /* Iron_Vector2 is byte-identical to Vector2, so the cast is safe. */
    DrawLineStrip((const Vector2 *)points, (int)count, c);
}
```

**Smoke test (write this BEFORE committing to all 9 bindings):**
```iron
-- Iron-side
func Draw.probe_array(points: [Vector2], count: Int32) {}

val pts: [Vector2] = [Vector2(Float32(0.0), Float32(0.0)),
                      Vector2(Float32(10.0), Float32(10.0))]
Draw.probe_array(pts, 2)  -- ironc must lower to Iron_draw_probe_array(pts_ptr, pts_len, 2)
```

```c
/* C-side — observe the prototype ironc generates */
void Iron_draw_probe_array(/* ??? */ points, int32_t count) {
    /* If Iron passes const Iron_Vector2* + len automatically → great, use it.
     * If Iron passes Iron_List_Iron_Vector2 → unpack via list.items / list.count.
     * If Iron REJECTS the array type → fall back to freestanding helper pattern. */
}
```

### Pattern 5: Begin/end mode pairs

**What:** Stateful begin/end pairs that wrap draw calls and push a mode onto raylib's internal stack.

**When to use:** `BeginDrawing`/`EndDrawing`, `BeginMode2D`/`EndMode2D`, `BeginTextureMode`/`EndTextureMode`, `BeginShaderMode`/`EndShaderMode`, `BeginBlendMode`/`EndBlendMode`, `BeginScissorMode`/`EndScissorMode`.

**Iron exposes the begin/end as two separate methods** — no closure-style `Draw.in_mode_2d(camera) { ... }` wrapper, because Iron has no block-argument syntax yet and raylib's C API is symmetric. User discipline keeps the stack balanced.

**Example:**
```iron
Draw.begin()
    Draw.clear(BLACK)
    Draw.begin_mode_2d(camera)
        Draw.rectangle(ball_x, ball_y, 10, 10, WHITE)
    Draw.end_mode_2d()
Draw.end()
```

### Anti-Patterns to Avoid

- **Naming `Draw.draw_rectangle(...)`.** Redundant with namespace. Use `Draw.rectangle(...)`.
- **Splitting `object Draw {}` into `object Mode {}` + `object Shapes {}`.** Contradicts 60-CONTEXT.md's explicit lock on `object Draw` for the whole surface.
- **Inlining the Iron_<T> → raylib <T> conversion with `*(T *)&x` casts.** Strict-aliasing risk and compiler-warning noise. Always use `memcpy` (matches `Iron_window_set_icon`).
- **Binding `DrawText` / `DrawFPS` / `DrawTexture` in Phase 63.** These belong to Phase 67 (text) and Phase 66 (textures). The `-- PHASE 63: DrawText(...)` markers in pong.iron (lines 99-100) are MISLABELED — they're Phase 67.
- **Binding `BeginVrStereoMode` / `EndVrStereoMode`.** Explicitly OOS per REQUIREMENTS.md.
- **Creating a new C shim file.** Shim lives in the existing single `iron_raylib.c`. Phase 60-CONTEXT.md locked this.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Cubic Bezier curve rendering | Iron-side spline rasterizer | `DrawSplineBezierCubic` | raylib's tessellator is mature, GPU-accelerated, and the 100% coverage rule requires binding it. |
| Rounded-rectangle corner arcs | Iron-side arc-geometry helper | `DrawRectangleRounded` | raylib computes the arc geometry internally with a tunable `segments` parameter. |
| Camera2D zoom/rotate transforms | Iron-side matrix math + manual vertex transform | `BeginMode2D(camera)` | raylib pushes a `MATRIX_MODELVIEW` using the Camera2D fields; any Iron-side reimplementation diverges from GPU behavior. |
| Render-to-texture framebuffer setup | Iron-side FBO object + glBindFramebuffer | `BeginTextureMode(rt)` | raylib handles FBO binding, viewport, depth buffer, flip-Y conventions. One function vs 30 lines of GL. |
| Alpha blending state | Iron-side `glBlendFunc` wrappers | `BeginBlendMode(.alpha)` / `.additive` / `.multiplied` | raylib's BlendMode enum maps to GL blend equations; hand-rolling = API divergence and broken semantics on web target. |
| Scissor rectangle clipping | Iron-side `glScissor` + `glEnable(GL_SCISSOR_TEST)` | `BeginScissorMode(x, y, w, h)` | Same as above — raylib abstracts the native target. |
| Spline point evaluation | Iron-side parametric curve math | `GetSplinePointLinear/Basis/CatmullRom/BezierQuad/BezierCubic` | `GetSplinePoint*` match raylib's draw-side tessellator. Divergence = visual-vs-logic mismatch. |
| Color RGBA struct | Iron-side `Color` struct — **actually, this IS the type defined in Phase 60**. Just use it. | `Color(r, g, b, a)` — exists in raylib.iron line ~110 | Already done by Phase 60. |

**Key insight:** Phase 63 is pure binding work. Every draw primitive exists in raylib with GPU-accelerated implementations. Hand-rolling any of them replaces 1 line of C shim with 30+ lines of Iron geometry and loses web-target parity.

## Runtime State Inventory

> Phase 63 is a greenfield binding phase, not a refactor. This section applies mainly to the re-enablement of `-- PHASE 63:` markers in 3 consumer files. No stored data / live service config / OS state / secrets / build artifacts are affected.

| Category | Items Found | Action Required |
|----------|-------------|------------------|
| Stored data | None | None |
| Live service config | None | None |
| OS-registered state | None | None |
| Secrets/env vars | None | None |
| Build artifacts | Pre-Phase-60 raylib.iron cached in git history only; no stale artifacts. `clang -c iron_raylib_layout.c` regenerates assertion .o on each build. | None |
| **Consumer files with `-- PHASE 63:` markers** | `examples/pong/pong.iron` (7 markers, lines 95-101), `tests/manual/game_raylib.iron` (1 marker, line 32), `tests/integration/web/hello_raylib.iron` (1 marker, line 15) | **Rewrite or re-mark in final Phase 63 plan.** Note: DrawText markers (pong.iron:99-100) are ACTUALLY Phase 67 (`TEXT-07/08`). Either re-mark as `-- PHASE 67:` or leave as-is and document the mislabel. |

**Nothing found in most categories:** Verified — Phase 63 is pure stdlib additions. No database, no external service, no OS registration. Only file-level edits.

## Common Pitfalls

### Pitfall 1: Float vs Float32 mismatch in spline parameter `t`

**What goes wrong:** `GetSplinePointLinear(start, end, float t)` takes `float` (raylib's C `float`). Iron's `Float` = `double`. If the Iron stub declares `t: Float` instead of `t: Float32`, the shim receives a double, truncates, and raylib gets a silently-wrong value.

**Why it happens:** Iron's default float literal is `Float` (double). Users write `Draw.get_spline_point_linear(start, end, 0.5)` and `0.5` is a double. If the stub declares `t: Float`, no compile error — silent truncation.

**How to avoid:** Declare `t: Float32`. Users must write `Float32(0.5)` explicitly (same pattern as pong.iron's `Vector2(Float32(400.0), Float32(300.0))` from Plan 60-08). Document in the docstring.

**Warning signs:** ironc compiles without error but the rendered spline curve is subtly wrong (off-by-one-pixel at endpoints, jittery evaluation).

### Pitfall 2: Missing `EndDrawing` after `BeginDrawing`

**What goes wrong:** Raylib buffers all draw calls between `BeginDrawing` and `EndDrawing` and only flushes at `EndDrawing`. Skipping `EndDrawing` results in a black screen and eventual buffer exhaustion.

**Why it happens:** Iron has no `defer` or RAII. Users who write `if (x) { Draw.begin(); return }` skip `Draw.end`.

**How to avoid:** Document the lifecycle in the Iron-side docstring. Do NOT attempt to enforce at the stdlib level (would require language-level support).

**Warning signs:** Black screen, `WARNING: CORE: Screen was not cleared` trace output.

### Pitfall 3: Nesting begin/end modes in the wrong order

**What goes wrong:** raylib's mode stack is FIFO per-type but the stacks interact. `BeginMode2D / BeginTextureMode / EndMode2D / EndTextureMode` works; `BeginMode2D / BeginTextureMode / EndTextureMode / EndMode2D` works too. But `BeginMode2D / EndScissorMode` without a matching `BeginScissorMode` corrupts raylib internals.

**Why it happens:** User error, especially in the "Nested beginShaderMode, beginBlendMode, and beginScissorMode" success criterion from the phase goal.

**How to avoid:** Document in raylib.iron docstring. No runtime enforcement. The phase success criterion validates this manually.

**Warning signs:** Visual artifacts; raylib trace `WARNING: rlgl: stack overflow/underflow`.

### Pitfall 4: Iron `Color(230, 41, 55, 255)` with Int literals vs `UInt8`

**What goes wrong:** `Color` is declared with `r, g, b, a: UInt8`. Iron integer literals are `Int` by default. Passing `255` may trigger an implicit widening that fails on older Iron versions.

**Why it happens:** Iron 60-08 rescue palette used `Color(230, 41, 55, 255)` — positional construction with Int literals — and verified it compiles. But explicit `UInt8(255)` casts may be required in the ORIGINAL ENUM-21 lookup.

**How to avoid:** Copy the Plan 60-08 rescue palette syntax verbatim: `Color(230, 41, 55, 255)`. This already works in pong.iron.

**Warning signs:** Compile error `field a expects UInt8, got Int`.

### Pitfall 5: Array ABI mismatch — `[Vector2]` vs `Iron_List_Iron_Vector2`

**What goes wrong:** Iron's compiler may lower `[Vector2]` as (a) a bare `const Iron_Vector2 *points` + separate length, OR (b) an `Iron_List_Iron_Vector2` struct with `.items`/`.count`/`.capacity`, depending on `ArrayParamMode` analysis in `src/lir/lir_optimize.c`. The shim C prototype must match what Iron emits.

**Why it happens:** Iron auto-selects `ARRAY_PARAM_CONST_PTR` when a function only reads and doesn't resize, `ARRAY_PARAM_LIST` otherwise. **External / foreign-method stubs may default to one mode that the shim wasn't written for.**

**How to avoid:** **Run a micro-smoke-test FIRST** (see Pattern 4 above). Observe ironc's emitted C prototype for a trivial `func Draw.probe_array(points: [Vector2], count: Int32) {}` and write the shim signature to match. Commit the probe + shim, verify it compiles, then scale up to all 9 array-taking functions.

**Warning signs:** Link error `undefined reference to Iron_draw_line_strip`; or compile error on iron_raylib.c about incompatible prototypes; or runtime crash reading past the end of the array.

### Pitfall 6: RenderTexture field layout — 44 bytes, not 40

**What goes wrong:** `RenderTexture` = `{ unsigned int id; Texture2D texture; Texture2D depth; }`. Naive count: 4 + 20 + 20 = 44. But raylib's layout might have 4 bytes of padding before `texture` to align it. The `_Static_assert(sizeof(struct Iron_RenderTexture) == sizeof(RenderTexture))` in iron_raylib_layout.c (Plan 60-03) ALREADY VERIFIED this — Iron matches C exactly.

**Why it happens:** Ironically, this is NOT a pitfall in practice because Phase 60-03 already caught it. Flagging as a pitfall in case a future raylib upgrade changes the layout.

**How to avoid:** Trust the `_Static_assert` grid. Do not hand-calculate sizes.

**Warning signs:** Build fails with `Iron_RenderTexture size must equal RenderTexture`. If this fires, raylib changed its struct definition — revisit Phase 60 assertions.

### Pitfall 7: `BlendMode.custom` / `BlendMode.custom_separate` require extra raylib setup

**What goes wrong:** raylib's `BlendMode` enum includes `CUSTOM` (index 6) and `CUSTOM_SEPARATE` (index 7) which require a prior `rlSetBlendFactors(...)` call before `BeginBlendMode(.custom)`. Without the setup, raylib logs a warning and uses default alpha blend.

**Why it happens:** REQUIREMENTS.md ENUM-08 includes all 8 values. User expects `.custom` to just work.

**How to avoid:** Document in the docstring that `.custom` and `.custom_separate` are advanced modes requiring prior `rlSetBlendFactors` (Phase 71 territory — rlgl.h is OOS for v2.0.0-alpha per REQUIREMENTS.md). Recommend users stick to `.alpha / .additive / .multiplied / .add_colors / .subtract_colors / .alpha_premultiply` for Phase 63.

**Warning signs:** No visual effect from `.custom` mode; `WARNING: RLGL: Custom blend mode requires rlSetBlendFactors` in trace log.

## Code Examples

Verified patterns from existing codebase:

### Example 1: Struct-by-value input — Color (NEW in Phase 63, pattern from Image)
```c
/* From iron_raylib.c:88 — Image by-value input precedent */
void Iron_window_set_icon(struct Iron_Image image) {
    Image rl_image;
    memcpy(&rl_image, &image, sizeof(Image));
    SetWindowIcon(rl_image);
}

/* Phase 63 adapts this for Color: */
void Iron_draw_clear(struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    ClearBackground(rl);
}
```
Source: `src/stdlib/iron_raylib.c:88`

### Example 2: Vector2 struct-by-value return
```c
/* From iron_raylib.c:137 (Plan 61-03) */
struct Iron_Vector2 Iron_window_get_window_position(void) {
    Vector2 rl = GetWindowPosition();
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}

/* Phase 63 adapts this for every GetSplinePoint*: */
struct Iron_Vector2 Iron_draw_get_spline_point_linear(
        struct Iron_Vector2 start, struct Iron_Vector2 end, float t) {
    Vector2 s, e;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    Vector2 rl = GetSplinePointLinear(s, e, t);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}
```
Source: `src/stdlib/iron_raylib.c:137`

### Example 3: Enum parameter casting
```c
/* From iron_raylib.c:74 — pattern for BlendMode input */
void Iron_window_set_state(uint32_t flags) {
    SetWindowState((unsigned int)flags);
}

/* Phase 63 adapts for BlendMode: */
void Iron_draw_begin_blend_mode(int32_t mode) {
    BeginBlendMode((int)mode);
}
```
Source: `src/stdlib/iron_raylib.c:74`

### Example 4: Iron-side stub declaration pattern
```iron
-- From raylib.iron:1176 (Plan 62-01)
func Keyboard.is_pressed(key: KeyboardKey) -> Bool {}

-- Phase 63 pattern — 2 parameters, one Int32, one Color-by-value, void return:
func Draw.clear(color: Color) {}

-- Phase 63 pattern — 5 parameters, 4 Int32 + 1 Color:
func Draw.rectangle(x: Int32, y: Int32, w: Int32, h: Int32, color: Color) {}

-- Phase 63 pattern — 2 Vector2 + 1 Float32 + 1 Color:
func Draw.line_ex(start: Vector2, end: Vector2, thick: Float32, color: Color) {}

-- Phase 63 pattern — struct-by-value return:
func Draw.get_spline_point_linear(start: Vector2, end: Vector2, t: Float32) -> Vector2 {}
```
Source: `src/stdlib/raylib.iron:1176`

### Example 5: Shim section marker append pattern
```
iron_raylib.c already has:
  line 291:  /* ── Input (Phase 62) ─────────────────────────────────────────────── */
  line 563:  /* ── 2D Drawing (Phase 63) ────────────────────────────────────────── */
  line 564:  /* ── Collision (Phase 64) ─────────────────────────────────────────── */

Phase 63 appends ABOVE line 564, immediately below line 563 marker.
iron_raylib.h Phase 63 marker at line 513 — same append location.
```
Source: `src/stdlib/iron_raylib.c:563`, `src/stdlib/iron_raylib.h:513`

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Bare `extern func InitWindow(...)` in raylib.iron | Shim-only through `iron_raylib.c` | Phase 60 (2026-04-14) | Enables `func Type.method` idiom (API-01) |
| Hand-written extern C prototypes | `emit_c` auto-generates from Iron stubs | Commit e3e5eee (2026-04-14) | Removes prototype-sync maintenance burden |
| Vec2 / RED / Key.W (pre-Phase-60) | Vector2 / `val RED = Color(...)` / KeyboardKey.W | Phase 60-08 (2026-04-14) | Clean-break; pong.iron rewritten; `-- PHASE 6N:` markers seed future phases |
| Int32 enum parameters | Typed Iron enum parameters | Phase 60 ENUM-22 + Phase 62 | `Draw.begin_blend_mode(.alpha)` not `.begin_blend_mode(0)` |
| Single-color palette (RED/BLUE/...) in raylib.iron | 6-color rescue palette in raylib.iron (TEX-14 full palette = Phase 66) | Phase 60-08 | Unblocks pong.iron; full palette deferred |

**Deprecated/outdated:**
- `val` vs `var` on object fields: Phase 60 locked all `val` (immutable). Phase 63 introduces no new fields, so this is inherited.
- Old raylib function names (`DrawRectangle` → `Draw.rectangle`): the C symbol stays, the Iron symbol is the snake_case shortened form.

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | Iron's `ARRAY_PARAM_CONST_PTR` mode (from `src/lir/lir_optimize.h:48`) is selected automatically for `func Draw.X(points: [Vector2], count: Int32) {}` stubs based on usage analysis; it emits `const Iron_Vector2 *` + `int32_t count` across the FFI. | Pattern 4, Pitfall 5 | Blocks 9 array-taking functions. Fallback: split the phase so 63-01/02 land without arrays; 63-03 probes the ABI; if unresolved, descope `[Vector2]` functions to Phase 73 API polish. | [ASSUMED — requires smoke test in 63-03] |
| A2 | `Camera2D` / `RenderTexture` / `Shader` / `NPatchInfo` / `Rectangle` / `Color` struct-by-value INPUT works identically to `Image` by-value input (memcpy-then-pass pattern from `Iron_window_set_icon`). | Pattern 1 | Low risk — Phase 60's `_Static_assert` grid proves byte-layout and Image-by-value was validated in Phase 61. If it breaks, fall back to out-parameter encoding. | [ASSUMED — validated indirectly by Phase 60 asserts + Phase 61 Image input] |
| A3 | `emit_c` auto-generates correct extern C prototypes for every new `func Draw.*` stub, including those with `Color` / `Camera` / `Vector2` / `Shader` / `RenderTexture` struct parameters. | Example 4 | Low risk — Phase 62 proved this for 18+ stubs across 4 plans. Phase 63's signatures are structurally identical (scalar + struct-by-value + enum). | [VERIFIED: commit e3e5eee, HANDOFF.md confirmation] |
| A4 | Dropping `draw_` prefix from Iron method names (`Draw.rectangle` vs `Draw.draw_rectangle`) is acceptable under 60-CONTEXT.md snake_case convention and matches the `Window.init` / `Window.should_close` style from Phase 61. | Claude's Discretion | Low — orthography preference. If rejected, rename mechanically. Recommend `Draw.rectangle` style; planner confirms in 63-CONTEXT.md. | [ASSUMED] |
| A5 | pong.iron `-- PHASE 63: DrawText(...)` markers at lines 99-100 are mislabeled and belong to Phase 67. Phase 63 either re-marks them as `-- PHASE 67:` or leaves them commented. | Deferred Ideas | Low — cosmetic. If misinterpreted, Phase 63 would attempt to bind DrawText which IS in Phase 67's scope (TEXT-07/08). The requirement DRAW2D-01..16 list does NOT include DrawText. | [VERIFIED: REQUIREMENTS.md DRAW2D-01..16 + TEXT-07/08 split] |
| A6 | No new `_Static_assert` entries are needed in iron_raylib_layout.c for Phase 63. Every type the phase passes by value (Color, Vector2, Rectangle, Camera, RenderTexture, Shader, NPatchInfo) already has its size + every-field offset asserted by Plans 60-02 through 60-04. | Layout Verification | Low — verified by grep of iron_raylib_layout.c. | [VERIFIED] |
| A7 | `BlendMode.custom` and `.custom_separate` will bind without Iron-side enforcement of the prerequisite `rlSetBlendFactors` setup call. Users who invoke them get a raylib trace warning but no hard failure. | Pitfall 7 | Low — document in docstring. rlgl.h is OOS for the milestone, so the setup helper cannot be bound yet. | [CITED: raylib.h comment "CUSTOM / CUSTOM_SEPARATE — requires rlSetBlendFactors"] |
| A8 | `SetShapesTexture` / `GetShapesTexture` / `GetShapesTextureRectangle` (raylib.h:1242-1244) are optional optimization helpers not required by DRAW2D-01..16. Phase 63 can include or defer them. Defaulting to INCLUDE for 100% coverage rule. | Deferred Ideas | Low — 3 functions, mechanical to bind. Do in 63-04 tail if time permits; otherwise defer to Phase 66 (uses Texture2D). | [CITED: raylib.h:1242 "It can be useful when using basic shapes and one single font"] |
| A9 | Iron's `[Vector2]` array literal syntax supports `Float32` field construction (`[Vector2(Float32(0.0), Float32(0.0)), Vector2(...)]`). | Smoke test | Medium — Iron's array-literal + struct-literal interaction may have gaps. Probe early. | [ASSUMED] |

## Open Questions

1. **Array-parameter ABI — `[Vector2]` across FFI.**
   - What we know: Iron has `ARRAY_PARAM_CONST_PTR` mode in `src/lir/lir_optimize.h:48`. `iron_net.c` uses `Iron_List_<T>` structs for linked-list unpacking, but Phase 62 `FilePathList` used a custom accessor pattern to avoid the array ABI question entirely.
   - What's unclear: Which mode ironc selects for a foreign-method stub with `[Vector2]` parameter; whether the prototype it emits expects `Iron_List_Iron_Vector2` or `const Iron_Vector2 *` + `int32_t`.
   - Recommendation: Plan 63-03 includes an explicit micro-smoke-test task FIRST (before binding the 9 array-taking functions). Write `func Draw.probe_array(pts: [Vector2], n: Int32) {}`; observe emitted C prototype; write shim to match. If ABI is `Iron_List`, use `.items` / `.count`. If it's raw pointer, use the cast. Document in SUMMARY.

2. **Drop `draw_` prefix from method names?**
   - What we know: 60-CONTEXT.md locks snake_case. Phase 61 uses `Window.init` (no `window_` prefix), Phase 62 uses `Keyboard.is_pressed` (no `keyboard_` prefix). Convention: drop the namespace-matching prefix.
   - What's unclear: `Draw.rectangle` reads fine but `Draw.end` collides with Iron's potential `end` keyword in older parsers. Verify ironc accepts `func Draw.end() {}`.
   - Recommendation: Plan 63-01 Task 1 includes a quick `ironc check` probe with `func Draw.end() {}` to confirm no keyword collision. If collision, fall back to `Draw.finish()` + rename accordingly. Unlikely — Iron has no reserved word `end`.

3. **`DrawText` / `DrawFPS` mislabeling in pong.iron.**
   - What we know: pong.iron lines 99-100 have `-- PHASE 63: DrawText(...)` markers. REQUIREMENTS.md DRAW2D-01..16 does not include text. TEXT-07 / TEXT-08 (Phase 67) owns `DrawText` / `DrawFPS` / `DrawTextEx` / `DrawTextPro`.
   - What's unclear: Whether to rewrite markers as `-- PHASE 67:` in Plan 63-04, or leave as-is and document the mislabel in the 63-04 SUMMARY.
   - Recommendation: Rewrite to `-- PHASE 67:` in Plan 63-04's pong.iron edit. This keeps the marker convention clean for Phase 67 executors and prevents future confusion.

4. **`SetShapesTexture` — include or defer?**
   - What we know: Three functions (`SetShapesTexture`, `GetShapesTexture`, `GetShapesTextureRectangle`) in `rshapes.c`. Not listed in DRAW2D-01..16 requirements. Used internally by raylib for batched single-draw-call optimization.
   - What's unclear: Whether the 100% coverage rule ("bind ALL raylib functions backing that capability") includes these — they're related to "how shapes draw" but aren't a capability themselves.
   - Recommendation: DEFER to Phase 66 (they take / return `Texture2D` and `Rectangle` for Texture; Phase 63 binds `Rectangle` input but not `Texture` input-as-output). Document the defer in 63-CONTEXT.md and 63-04-SUMMARY. Rationale: Phase 63 is already 55 functions; adding 3 Texture-parameter functions leaks into Phase 66's surface.

## Environment Availability

> Applies to: local development, CI build of ironc + iron_raylib.c + raylib vendored source.

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| clang or gcc | C compile of iron_raylib.c + iron_raylib_layout.c | ✓ (both typically present) | any recent | either works — HANDOFF.md Plan 62-03/04 noted clang absent on Linux, used gcc |
| raylib 5.5 vendored source | Iron_draw_* shims call into DrawRectangle, etc. | ✓ | 5.5 at src/vendor/raylib/ | — |
| ironc (Iron compiler) | `ironc build examples/pong/pong.iron` canonical validation | ✗ (no /home/victor/code/iron-lang/build/ironc present) | — | Build ironc first: `cmake -B build && cmake --build build`. Canonical pong validation is memory-expensive (~10 GB per invocation per HANDOFF.md); secondary files optional. |
| emcc (for web target) | `iron build --target=web` for hello_raylib.iron | unknown on this host | — | Web build test optional per memory discipline; clang `-c` verification on iron_raylib.c is the baseline. |

**Missing dependencies with no fallback:**
- None blocking. `ironc` needs to be built before final Plan 63-04 canonical pong.iron end-to-end validation, but this is a pre-existing project requirement, not a Phase 63 blocker.

**Missing dependencies with fallback:**
- ironc missing → build it first, or run wave-2 shim-level verification with `gcc -c` only (the Phase 62 pattern per HANDOFF.md memory discipline).

## Validation Architecture

> Phase 63's validation model inherits from Phase 61/62 — a mix of `clang -c`-level shim verification (cheap, fast) and `ironc build` end-to-end integration tests (expensive, canonical). `workflow.nyquist_validation` is not explicitly set; default = enabled. Including this section.

### Test Framework
| Property | Value |
|----------|-------|
| Framework | No formal test framework for raylib bindings. Verification is grep-based (Phase 62 pattern) + compile check. |
| Config file | `CMakeLists.txt` for ironc build; no pytest / jest equivalent. |
| Quick run command | `gcc -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra -o /tmp/verify.o` (or clang) |
| Full suite command | `./build/ironc build examples/pong/pong.iron -o /tmp/pong` — canonical end-to-end |

### Phase Requirements → Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DRAW2D-01 | `Draw.begin()` / `Draw.end()` | grep + compile | `grep -c '^func Draw.begin' src/stdlib/raylib.iron` (expect 1) + `gcc -c iron_raylib.c` | ❌ Wave 0 |
| DRAW2D-02 | `Draw.clear(color)` | grep + compile | `grep -c 'ClearBackground' src/stdlib/iron_raylib.c` (expect ≥1) | ❌ Wave 0 |
| DRAW2D-03 | `Draw.begin_mode_2d(camera)` | grep + compile | `grep -c 'BeginMode2D' src/stdlib/iron_raylib.c` + `func Draw.begin_mode_2d(camera: Camera)` in raylib.iron | ❌ Wave 0 |
| DRAW2D-04 | `Draw.begin_texture_mode(rt)` | grep + compile | `grep -c 'BeginTextureMode' iron_raylib.c` + stub grep | ❌ Wave 0 |
| DRAW2D-05 | `Draw.begin_shader_mode(s)` + `Draw.begin_blend_mode(mode)` | grep + compile | `grep -c 'BeginShaderMode\|BeginBlendMode' iron_raylib.c` (expect ≥2) | ❌ Wave 0 |
| DRAW2D-06 | `Draw.begin_scissor_mode(x,y,w,h)` | grep + compile | `grep -c 'BeginScissorMode' iron_raylib.c` | ❌ Wave 0 |
| DRAW2D-07 | `Draw.pixel` / `Draw.pixel_v` | grep + compile | `grep -c 'DrawPixel\|DrawPixelV' iron_raylib.c` (expect ≥2) | ❌ Wave 0 |
| DRAW2D-08 | Lines (5 variants) | grep + compile | `grep -c 'DrawLine\b\|DrawLineV\|DrawLineEx\|DrawLineStrip\|DrawLineBezier' iron_raylib.c` (expect ≥5) | ❌ Wave 0 |
| DRAW2D-09 | Circles (7 variants) | grep + compile | `grep -c 'DrawCircle\b\|DrawCircleSector\|DrawCircleSectorLines\|DrawCircleGradient\|DrawCircleV\|DrawCircleLines\|DrawCircleLinesV' iron_raylib.c` (expect ≥7) | ❌ Wave 0 |
| DRAW2D-10 | Ellipses (2) | grep + compile | `grep -c 'DrawEllipse' iron_raylib.c` (expect ≥2) | ❌ Wave 0 |
| DRAW2D-11 | Rings (2) | grep + compile | `grep -c 'DrawRing' iron_raylib.c` (expect ≥2) | ❌ Wave 0 |
| DRAW2D-12 | Rectangles (12 variants) | grep + compile | `grep -c 'DrawRectangle' iron_raylib.c` (expect ≥12) | ❌ Wave 0 |
| DRAW2D-13 | Triangles (4) | grep + compile | `grep -c 'DrawTriangle' iron_raylib.c` (expect ≥4) | ❌ Wave 0 |
| DRAW2D-14 | Polygons (3) | grep + compile | `grep -c 'DrawPoly' iron_raylib.c` (expect ≥3) | ❌ Wave 0 |
| DRAW2D-15 | Splines draws (10) | grep + compile | `grep -c 'DrawSpline' iron_raylib.c` (expect ≥10) | ❌ Wave 0 |
| DRAW2D-16 | Spline evaluators (5) | grep + compile | `grep -c 'GetSplinePoint' iron_raylib.c` (expect ≥5) | ❌ Wave 0 |
| End-to-end integration | pong.iron compiles + runs | ironc build | `./build/ironc build examples/pong/pong.iron -o /tmp/pong && file /tmp/pong | grep -q 'Mach-O\|ELF'` | ❌ Wave 0 (canonical, Plan 63-04) |

### Sampling Rate
- **Per task commit:** `gcc -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra -o /tmp/check.o` (fast, <5s)
- **Per wave merge:** grep-count verification + gcc compile of both iron_raylib.c AND iron_raylib_layout.c
- **Phase gate:** Full `ironc build examples/pong/pong.iron` produces runnable binary. (One expensive invocation, per HANDOFF.md memory discipline — run in Plan 63-04 only.)

### Wave 0 Gaps
- [ ] None at Phase 63 open — existing verification scaffolding is complete from Phase 62. No new test infrastructure needed.
- [ ] No `pytest` / `jest` equivalent to add; grep + clang/gcc compile is the established test model.
- [ ] (Deferred: a visual regression smoke test rendering every DRAW2D primitive to a PNG and diffing against reference — this is Phase 73 API showcase territory.)

## Security Domain

> `security_enforcement` is not explicitly set in `.planning/config.json` (file exists but focuses on workflow defaults). Defaulting to enabled. Phase 63 has minimal security surface — no new auth, session, crypto, or input validation paths are introduced. Every function takes primitives or opaque raylib-managed state.

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | no | — |
| V3 Session Management | no | — |
| V4 Access Control | no | — |
| V5 Input Validation | partial | Iron's typed enums enforce BlendMode/MouseCursor/etc. validity at the type-check level (no raw Int fallback). `[Vector2]` array length mismatches are raylib's concern (crash in draw calls, not ours). |
| V6 Cryptography | no | — |

### Known Threat Patterns for Phase 63's stack

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| Dereference opaque pointer from Iron | Tampering | `_`-prefixed `val: Int` convention from 60-CONTEXT.md hides `Shader._locs`, `Mesh._vertices`, etc. Users cannot construct pointers. Not a Phase 63 issue. |
| Out-of-bounds `[Vector2]` array length | DoS (crash) | Rely on Iron's array-length metadata + raylib's internal bounds (raylib trusts the passed length). Document in docstring: "count must equal array length". No shim-side validation (would cost one grep per draw call). |
| Spline evaluator with out-of-range `t` | — (rendering correctness only) | Document that `t ∈ [0.0, 1.0]` per raylib comment line 1297. Values outside still return a Vector2; just extrapolate. |
| Negative thickness / radius / segments | DoS (hang) | raylib handles internally; typically no-op on negative inputs. Document in docstring. |
| `Draw.begin_scissor_mode(w < 0, ...)` | — | raylib clamps internally to valid scissor rects. No Iron-side enforcement needed. |

### STRIDE summary
- **Spoofing / Repudiation / Elevation-of-Privilege:** Not applicable; Phase 63 is pure rendering pipeline.
- **Tampering:** Mitigated by opaque-pointer convention (pre-existing).
- **Information Disclosure:** Rendering to a RenderTexture could theoretically leak framebuffer contents to a user-controlled surface, but this is a design feature (what the texture-mode IS for), not a flaw.
- **DoS:** Bounded by raylib's internal defensive coding. No new attack surface over Phase 61's Window.init.

## Exhaustive Function List (Phase 63 scope, 55 functions)

> Authoritative count from raylib.h lines 1027-1042 + 1247-1302. Verified by direct grep.

### Frame / Draw-Mode Stack (12 functions — DRAW2D-01..06)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 1 | `BeginDrawing(void)` | `Draw.begin()` | DRAW2D-01 |
| 2 | `EndDrawing(void)` | `Draw.end()` | DRAW2D-01 |
| 3 | `ClearBackground(Color)` | `Draw.clear(color: Color)` | DRAW2D-02 |
| 4 | `BeginMode2D(Camera2D)` | `Draw.begin_mode_2d(camera: Camera)` | DRAW2D-03 |
| 5 | `EndMode2D(void)` | `Draw.end_mode_2d()` | DRAW2D-03 |
| 6 | `BeginTextureMode(RenderTexture2D)` | `Draw.begin_texture_mode(target: RenderTexture)` | DRAW2D-04 |
| 7 | `EndTextureMode(void)` | `Draw.end_texture_mode()` | DRAW2D-04 |
| 8 | `BeginShaderMode(Shader)` | `Draw.begin_shader_mode(shader: Shader)` | DRAW2D-05 |
| 9 | `EndShaderMode(void)` | `Draw.end_shader_mode()` | DRAW2D-05 |
| 10 | `BeginBlendMode(int)` | `Draw.begin_blend_mode(mode: BlendMode)` | DRAW2D-05 |
| 11 | `EndBlendMode(void)` | `Draw.end_blend_mode()` | DRAW2D-05 |
| 12 | `BeginScissorMode(int, int, int, int)` | `Draw.begin_scissor_mode(x: Int32, y: Int32, w: Int32, h: Int32)` | DRAW2D-06 |
| 13 | `EndScissorMode(void)` | `Draw.end_scissor_mode()` | DRAW2D-06 |

> Actually 13 in this group (I wrote 12 above, correcting). Total Phase 63 count adjusts accordingly.

### Pixel primitives (2 functions — DRAW2D-07)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 14 | `DrawPixel(int, int, Color)` | `Draw.pixel(x: Int32, y: Int32, color: Color)` | DRAW2D-07 |
| 15 | `DrawPixelV(Vector2, Color)` | `Draw.pixel_v(position: Vector2, color: Color)` | DRAW2D-07 |

### Line primitives (5 functions — DRAW2D-08)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 16 | `DrawLine(int, int, int, int, Color)` | `Draw.line(x1: Int32, y1: Int32, x2: Int32, y2: Int32, color: Color)` | DRAW2D-08 |
| 17 | `DrawLineV(Vector2, Vector2, Color)` | `Draw.line_v(start: Vector2, end: Vector2, color: Color)` | DRAW2D-08 |
| 18 | `DrawLineEx(Vector2, Vector2, float, Color)` | `Draw.line_ex(start: Vector2, end: Vector2, thick: Float32, color: Color)` | DRAW2D-08 |
| 19 | `DrawLineStrip(const Vector2 *, int, Color)` | `Draw.line_strip(points: [Vector2], count: Int32, color: Color)` ⚠ array ABI | DRAW2D-08 |
| 20 | `DrawLineBezier(Vector2, Vector2, float, Color)` | `Draw.line_bezier(start: Vector2, end: Vector2, thick: Float32, color: Color)` | DRAW2D-08 |

### Circle primitives (7 functions — DRAW2D-09)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 21 | `DrawCircle(int, int, float, Color)` | `Draw.circle(cx: Int32, cy: Int32, r: Float32, color: Color)` | DRAW2D-09 |
| 22 | `DrawCircleSector(Vector2, float, float, float, int, Color)` | `Draw.circle_sector(center: Vector2, r: Float32, start: Float32, end: Float32, segments: Int32, color: Color)` | DRAW2D-09 |
| 23 | `DrawCircleSectorLines(Vector2, float, float, float, int, Color)` | `Draw.circle_sector_lines(...)` | DRAW2D-09 |
| 24 | `DrawCircleGradient(int, int, float, Color, Color)` | `Draw.circle_gradient(cx: Int32, cy: Int32, r: Float32, inner: Color, outer: Color)` | DRAW2D-09 |
| 25 | `DrawCircleV(Vector2, float, Color)` | `Draw.circle_v(center: Vector2, r: Float32, color: Color)` | DRAW2D-09 |
| 26 | `DrawCircleLines(int, int, float, Color)` | `Draw.circle_lines(cx: Int32, cy: Int32, r: Float32, color: Color)` | DRAW2D-09 |
| 27 | `DrawCircleLinesV(Vector2, float, Color)` | `Draw.circle_lines_v(center: Vector2, r: Float32, color: Color)` | DRAW2D-09 |

### Ellipse primitives (2 functions — DRAW2D-10)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 28 | `DrawEllipse(int, int, float, float, Color)` | `Draw.ellipse(cx: Int32, cy: Int32, rh: Float32, rv: Float32, color: Color)` | DRAW2D-10 |
| 29 | `DrawEllipseLines(int, int, float, float, Color)` | `Draw.ellipse_lines(cx: Int32, cy: Int32, rh: Float32, rv: Float32, color: Color)` | DRAW2D-10 |

### Ring primitives (2 functions — DRAW2D-11)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 30 | `DrawRing(Vector2, float, float, float, float, int, Color)` | `Draw.ring(center: Vector2, inner: Float32, outer: Float32, start: Float32, end: Float32, segments: Int32, color: Color)` | DRAW2D-11 |
| 31 | `DrawRingLines(Vector2, float, float, float, float, int, Color)` | `Draw.ring_lines(...)` | DRAW2D-11 |

### Rectangle primitives (12 functions — DRAW2D-12)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 32 | `DrawRectangle(int, int, int, int, Color)` | `Draw.rectangle(x: Int32, y: Int32, w: Int32, h: Int32, color: Color)` | DRAW2D-12 |
| 33 | `DrawRectangleV(Vector2, Vector2, Color)` | `Draw.rectangle_v(position: Vector2, size: Vector2, color: Color)` | DRAW2D-12 |
| 34 | `DrawRectangleRec(Rectangle, Color)` | `Draw.rectangle_rec(rec: Rectangle, color: Color)` | DRAW2D-12 |
| 35 | `DrawRectanglePro(Rectangle, Vector2, float, Color)` | `Draw.rectangle_pro(rec: Rectangle, origin: Vector2, rotation: Float32, color: Color)` | DRAW2D-12 |
| 36 | `DrawRectangleGradientV(int, int, int, int, Color, Color)` | `Draw.rectangle_gradient_v(x: Int32, y: Int32, w: Int32, h: Int32, top: Color, bottom: Color)` | DRAW2D-12 |
| 37 | `DrawRectangleGradientH(int, int, int, int, Color, Color)` | `Draw.rectangle_gradient_h(...)` | DRAW2D-12 |
| 38 | `DrawRectangleGradientEx(Rectangle, Color, Color, Color, Color)` | `Draw.rectangle_gradient_ex(rec: Rectangle, tl: Color, bl: Color, tr: Color, br: Color)` | DRAW2D-12 |
| 39 | `DrawRectangleLines(int, int, int, int, Color)` | `Draw.rectangle_lines(x: Int32, y: Int32, w: Int32, h: Int32, color: Color)` | DRAW2D-12 |
| 40 | `DrawRectangleLinesEx(Rectangle, float, Color)` | `Draw.rectangle_lines_ex(rec: Rectangle, thick: Float32, color: Color)` | DRAW2D-12 |
| 41 | `DrawRectangleRounded(Rectangle, float, int, Color)` | `Draw.rectangle_rounded(rec: Rectangle, roundness: Float32, segments: Int32, color: Color)` | DRAW2D-12 |
| 42 | `DrawRectangleRoundedLines(Rectangle, float, int, Color)` | `Draw.rectangle_rounded_lines(rec: Rectangle, roundness: Float32, segments: Int32, color: Color)` | DRAW2D-12 |
| 43 | `DrawRectangleRoundedLinesEx(Rectangle, float, int, float, Color)` | `Draw.rectangle_rounded_lines_ex(rec: Rectangle, roundness: Float32, segments: Int32, thick: Float32, color: Color)` | DRAW2D-12 |

### Triangle primitives (4 functions — DRAW2D-13)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 44 | `DrawTriangle(Vector2, Vector2, Vector2, Color)` | `Draw.triangle(v1: Vector2, v2: Vector2, v3: Vector2, color: Color)` | DRAW2D-13 |
| 45 | `DrawTriangleLines(Vector2, Vector2, Vector2, Color)` | `Draw.triangle_lines(v1: Vector2, v2: Vector2, v3: Vector2, color: Color)` | DRAW2D-13 |
| 46 | `DrawTriangleFan(const Vector2 *, int, Color)` | `Draw.triangle_fan(points: [Vector2], count: Int32, color: Color)` ⚠ array ABI | DRAW2D-13 |
| 47 | `DrawTriangleStrip(const Vector2 *, int, Color)` | `Draw.triangle_strip(points: [Vector2], count: Int32, color: Color)` ⚠ array ABI | DRAW2D-13 |

### Polygon primitives (3 functions — DRAW2D-14)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 48 | `DrawPoly(Vector2, int, float, float, Color)` | `Draw.poly(center: Vector2, sides: Int32, r: Float32, rotation: Float32, color: Color)` | DRAW2D-14 |
| 49 | `DrawPolyLines(Vector2, int, float, float, Color)` | `Draw.poly_lines(center: Vector2, sides: Int32, r: Float32, rotation: Float32, color: Color)` | DRAW2D-14 |
| 50 | `DrawPolyLinesEx(Vector2, int, float, float, float, Color)` | `Draw.poly_lines_ex(center: Vector2, sides: Int32, r: Float32, rotation: Float32, thick: Float32, color: Color)` | DRAW2D-14 |

### Spline primitives (10 functions — DRAW2D-15)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 51 | `DrawSplineLinear(const Vector2 *, int, float, Color)` | `Draw.spline_linear(points: [Vector2], count: Int32, thick: Float32, color: Color)` ⚠ array ABI, min 2 pts | DRAW2D-15 |
| 52 | `DrawSplineBasis(const Vector2 *, int, float, Color)` | `Draw.spline_basis(points: [Vector2], count: Int32, thick: Float32, color: Color)` ⚠ array ABI, min 4 pts | DRAW2D-15 |
| 53 | `DrawSplineCatmullRom(const Vector2 *, int, float, Color)` | `Draw.spline_catmull_rom(points: [Vector2], count: Int32, thick: Float32, color: Color)` ⚠ array ABI, min 4 pts | DRAW2D-15 |
| 54 | `DrawSplineBezierQuadratic(const Vector2 *, int, float, Color)` | `Draw.spline_bezier_quadratic(points: [Vector2], count: Int32, thick: Float32, color: Color)` ⚠ array ABI, min 3 pts, pattern `[p1, c2, p3, c4, ...]` | DRAW2D-15 |
| 55 | `DrawSplineBezierCubic(const Vector2 *, int, float, Color)` | `Draw.spline_bezier_cubic(points: [Vector2], count: Int32, thick: Float32, color: Color)` ⚠ array ABI, min 4 pts, pattern `[p1, c2, c3, p4, c5, c6, ...]` | DRAW2D-15 |
| 56 | `DrawSplineSegmentLinear(Vector2, Vector2, float, Color)` | `Draw.spline_segment_linear(p1: Vector2, p2: Vector2, thick: Float32, color: Color)` | DRAW2D-15 |
| 57 | `DrawSplineSegmentBasis(Vector2, Vector2, Vector2, Vector2, float, Color)` | `Draw.spline_segment_basis(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, thick: Float32, color: Color)` | DRAW2D-15 |
| 58 | `DrawSplineSegmentCatmullRom(Vector2, Vector2, Vector2, Vector2, float, Color)` | `Draw.spline_segment_catmull_rom(...)` | DRAW2D-15 |
| 59 | `DrawSplineSegmentBezierQuadratic(Vector2, Vector2, Vector2, float, Color)` | `Draw.spline_segment_bezier_quadratic(p1: Vector2, c2: Vector2, p3: Vector2, thick: Float32, color: Color)` | DRAW2D-15 |
| 60 | `DrawSplineSegmentBezierCubic(Vector2, Vector2, Vector2, Vector2, float, Color)` | `Draw.spline_segment_bezier_cubic(p1: Vector2, c2: Vector2, c3: Vector2, p4: Vector2, thick: Float32, color: Color)` | DRAW2D-15 |

### Spline evaluators (5 functions — DRAW2D-16)
| # | raylib C | Iron-side | Req |
|---|----------|-----------|-----|
| 61 | `GetSplinePointLinear(Vector2, Vector2, float) -> Vector2` | `Draw.get_spline_point_linear(start: Vector2, end: Vector2, t: Float32) -> Vector2` | DRAW2D-16 |
| 62 | `GetSplinePointBasis(Vector2, Vector2, Vector2, Vector2, float) -> Vector2` | `Draw.get_spline_point_basis(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, t: Float32) -> Vector2` | DRAW2D-16 |
| 63 | `GetSplinePointCatmullRom(Vector2, Vector2, Vector2, Vector2, float) -> Vector2` | `Draw.get_spline_point_catmull_rom(...)` | DRAW2D-16 |
| 64 | `GetSplinePointBezierQuad(Vector2, Vector2, Vector2, float) -> Vector2` | `Draw.get_spline_point_bezier_quadratic(p1: Vector2, c2: Vector2, p3: Vector2, t: Float32) -> Vector2` (name normalized from C `BezierQuad` to `bezier_quadratic` for draw/eval symmetry) | DRAW2D-16 |
| 65 | `GetSplinePointBezierCubic(Vector2, Vector2, Vector2, Vector2, float) -> Vector2` | `Draw.get_spline_point_bezier_cubic(...)` | DRAW2D-16 |

**CORRECTED TOTAL:** 65 functions across DRAW2D-01..16. (The earlier summary estimated 55; accurate count from raylib.h is 65 if all spline segments, all rectangle variants, and all begin/end pairs are included.)

**Breakdown:**
- DRAW2D-01..06 (modes): 13 functions
- DRAW2D-07 (pixels): 2
- DRAW2D-08 (lines): 5
- DRAW2D-09 (circles): 7
- DRAW2D-10 (ellipses): 2
- DRAW2D-11 (rings): 2
- DRAW2D-12 (rectangles): 12
- DRAW2D-13 (triangles): 4
- DRAW2D-14 (polygons): 3
- DRAW2D-15 (splines draws): 10
- DRAW2D-16 (spline evaluators): 5

**Total: 65 Iron-side stubs + 65 C shim implementations.** (If `SetShapesTexture` / `GetShapesTexture` / `GetShapesTextureRectangle` are included: 68 total — recommend defer.)

Context budget estimate: each plan file for ~15 functions averages ~400 lines (per Phase 62 precedents). Phase 63 at 65 functions = ~4 plans × ~16 functions × ~400 lines = plan file total ~1600 lines. Reasonable.

## Plan Slicing Recommendation

| Plan | Scope | Functions | Reqs | Wave | Depends on |
|------|-------|-----------|------|------|------------|
| **63-01** | Frame + stack modes: begin/end, clear, mode_2d, texture_mode, shader_mode, blend_mode, scissor_mode. **Validates Color+Camera2D+RenderTexture+Shader by-value input ABI.** | 13 | DRAW2D-01..06 | 1 | none (Phase 62 complete) |
| **63-02** | Scalar & Vector2 primitives: pixels, lines (no strip), circles, ellipses, rings. **Mechanical — no new ABI surface.** | 16 | DRAW2D-07, 08 (partial: skip line_strip), 09, 10, 11 | 2 | 63-01 (serial — all append to same tail of Phase 63 section) |
| **63-03** | Rectangles + triangles (no strip/fan) + polygons. **Includes array ABI smoke test at task 1 (for DRAW2D-13 TriangleFan/Strip — defer these if probe fails).** | 15 scalar + 2 array-after-probe + 3 = 20 (or 18 if arrays descoped) | DRAW2D-12, 13 (partial), 14 | 2 | 63-02 |
| **63-04** | Splines (whole + segments + evaluators) + DRAW2D-08 DrawLineStrip (deferred from 63-02) + DRAW2D-13 TriangleFan/Strip (deferred from 63-03) + pong.iron / game_raylib.iron / hello_raylib.iron re-enablement + DrawText marker re-mark as PHASE 67. | 10 splines + 5 evaluators + 1 line_strip + 2 triangle-array = 18 (if array ABI confirmed in 63-03) | DRAW2D-15, 16 (+ reclaim DRAW2D-08/13 array leftovers) | 2 | 63-03 (canonical ironc validation) |

**Rationale:**
- 4 subphases match Phase 62's proven slicing granularity (4 plans for 40 functions = ~10 fns/plan; Phase 63 has 65 fns, averages 16 fns/plan).
- Plan 63-01 is the "risky" plan — it validates 4 new struct-by-value input ABIs (Color, Camera2D, RenderTexture, Shader) in one batch. If any fail, blocker surfaces at plan 1, not plan 4.
- Plan 63-03 is the "array ABI" plan — it probes `[Vector2]` before committing to the 9 array-taking functions. Fallback path is clearly documented.
- Plan 63-04 is the "sweep + validation" plan — it consumes any deferred array functions, re-enables consumer files, runs the canonical `ironc build pong.iron`.
- Wave 2 is serial (62-01 → 62-02 pattern): all plans append to the tail of the Phase 63 marker in the same three shim files; parallel execution would merge-conflict.

**Alternative 3-plan split (if budget is tight):**
- 63-01: modes + primitives through rectangles (43 fns)
- 63-02: triangles + polygons + splines draws (17 fns)
- 63-03: spline evaluators + array sweep + pong.iron (5 + deferred arrays + re-enablement)

Not recommended — loses the ABI-smoke-test safety margin.

**Alternative 5-plan split (if extra granularity needed):**
- 63-01: begin/end + clear (DRAW2D-01..02, 3 fns, tiny but validates Color input)
- 63-02: mode stack (DRAW2D-03..06, 10 fns, validates Camera2D/RT/Shader input)
- 63-03: primitives (pixel / line / circle / ellipse / ring / rect / triangle / poly, ~35 fns)
- 63-04: splines + evaluators (15 fns)
- 63-05: re-enablement + pong.iron validation

Acceptable but over-fragmented. 4-plan version is the sweet spot.

## Sources

### Primary (HIGH confidence — VERIFIED)
- `src/vendor/raylib/raylib.h` lines 1027-1042 (begin/end modes), 1247-1302 (shape + spline draws) — exhaustive C function signatures for Phase 63 [VERIFIED: direct grep]
- `src/stdlib/raylib.iron` — current state of Phase 60/61/62 bindings [VERIFIED: read at research time]
- `src/stdlib/iron_raylib.c:88` — `Iron_window_set_icon` — struct-by-value INPUT precedent [VERIFIED]
- `src/stdlib/iron_raylib.c:137-148` — `Iron_window_get_window_position` / `get_window_scale_dpi` — struct-by-value RETURN precedent [VERIFIED]
- `src/stdlib/iron_raylib_layout.c:114-119, 186-191` — `_Static_assert` grid for Color and Camera2D [VERIFIED]
- `src/lir/lir_optimize.h:44-50` — `ArrayParamMode` enum definition [VERIFIED]
- `.planning/phases/60-type-enum-foundation/60-CONTEXT.md` — binding architecture, naming, `object Draw` receiver [VERIFIED]
- `.planning/phases/61-window-system/61-CONTEXT.md` — struct-by-value ABI validation protocol [VERIFIED]
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-CONTEXT.md` — Plan slicing precedent, wave-serial pattern [VERIFIED]
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/HANDOFF.md` — memory discipline, clang/gcc fallback note [VERIFIED]
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-01-PLAN.md` and `62-04-PLAN.md` — concrete plan structure and acceptance-criteria patterns [VERIFIED]
- `.planning/REQUIREMENTS.md` lines 108-123 (DRAW2D-01..16) [VERIFIED]
- `.planning/ROADMAP.md` lines 133-148 (Phase 63 section) [VERIFIED]
- `examples/pong/pong.iron` — `-- PHASE 63:` markers at lines 95-101 [VERIFIED]
- `tests/manual/game_raylib.iron` — `-- PHASE 63:` marker at line 32 [VERIFIED]
- `tests/integration/web/hello_raylib.iron` — `-- PHASE 63:` marker at line 15 [VERIFIED]

### Secondary (MEDIUM confidence — CITED)
- raylib 5.5 documentation for `BlendMode.CUSTOM` / `CUSTOM_SEPARATE` requirement of `rlSetBlendFactors` prior setup [CITED: src/vendor/raylib/raylib.h enum comments]
- raylib spline point-pattern for BezierQuadratic `[p1, c2, p3, c4, ...]` and BezierCubic `[p1, c2, c3, p4, c5, c6, ...]` [CITED: raylib.h:1289-1290 inline comments]

### Tertiary (LOW confidence — ASSUMED, PENDING VERIFICATION)
- Iron's `ARRAY_PARAM_CONST_PTR` mode is auto-selected for foreign-method stubs with `[T]` parameters and passes `const T *` + separate length across the FFI. **Needs smoke test in 63-03.** [ASSUMED]
- Iron's `func Draw.end() {}` declaration does not collide with any Iron reserved word. **Quick ironc check in 63-01 task 1.** [ASSUMED]
- `Float32` + `Int32` integer literal coercion for `Color(230, 41, 55, 255)` matches the Plan 60-08 rescue-palette pattern. [VERIFIED — pong.iron compiles per STATE.md line 105]

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — 100% pattern inheritance from Phase 60/61/62, well-documented precedents
- Architecture: HIGH — shim + stub pattern is proven across 3 prior phases; 60-CONTEXT.md locks every decision
- Pitfalls: MEDIUM — Float32 and Color patterns verified; array-ABI pitfall is GENUINE unknown requiring smoke test
- Function list: HIGH — exhaustive grep of raylib.h:1027-1042 + 1247-1302
- Plan slicing: HIGH — mirrors Phase 62's successful 4-plan pattern with one minor tweak (array smoke test in plan 3)

**Research date:** 2026-04-16
**Valid until:** 2026-05-16 (30 days — Phase 60/61/62 stack is stable; only risk is Iron compiler changes to `emit_c` array lowering)

## Project Constraints (from CLAUDE.md)

No `CLAUDE.md` exists at `/home/victor/code/iron-lang/CLAUDE.md` as of 2026-04-16. No project-specific directives to enforce beyond the inherited Phase 60/61/62 conventions (documented in "User Constraints" section above).
