# Phase 63: 2D Drawing - Pattern Map

**Mapped:** 2026-04-16
**Files analyzed:** 6 (3 stdlib files modified, 3 consumer files modified)
**Analogs found:** 65 / 65 functions have exact precedent shims (only NEW ABI surface = struct-by-value *input* for Color/Camera/RenderTexture/Shader + `[Vector2]` array passthrough)

## File Classification

| New/Modified File | Role | Data Flow | Closest Analog | Match Quality |
|-------------------|------|-----------|----------------|---------------|
| `src/stdlib/raylib.iron` (append) | stdlib — Iron-side foreign-method stubs | FFI binding declaration | `src/stdlib/raylib.iron` Phase 62 Input section (lines 1176-1310) | exact |
| `src/stdlib/iron_raylib.h` (append) | stdlib — C prototypes | FFI header | `src/stdlib/iron_raylib.h` Phase 62 Input section (lines 435-511) | exact |
| `src/stdlib/iron_raylib.c` (append) | stdlib — C shim implementations | FFI forward to raylib | `src/stdlib/iron_raylib.c` Phase 62 Input section (lines 291-561) | exact |
| `examples/pong/pong.iron` (edit) | consumer — integration test | game loop | Phase 62 re-enablement pattern (pong.iron:89-93) | exact |
| `tests/manual/game_raylib.iron` (edit) | consumer — manual smoke test | placeholder exerciser | Phase 62 rewrite pattern (game_raylib.iron:33-34) | exact |
| `tests/integration/web/hello_raylib.iron` (edit) | consumer — web smoke test | hello-world | Phase 62 pattern (hello_raylib.iron:15) | exact |

## Pattern Assignments

### `src/stdlib/iron_raylib.c` — 65 shim implementations (controller, request-response)

Analogs live in `src/stdlib/iron_raylib.c` Phase 61 (lines 36-290) and Phase 62 (lines 291-561). All 65 Phase 63 shims map to one of **7 mechanical templates** derived from the analogs below.

---

#### Template A: No-arg void shim (begin/end stack wrappers)

**Applies to (8 functions):** `Draw.begin`, `Draw.end`, `Draw.end_mode_2d`, `Draw.end_texture_mode`, `Draw.end_shader_mode`, `Draw.end_blend_mode`, `Draw.end_scissor_mode` (+ `BeginDrawing` itself as begin).

**Analog:** `src/stdlib/iron_raylib.c:48-50` — `Iron_window_close`; and the dense block at `src/stdlib/iron_raylib.c:68-72` (5 one-liners).

Excerpt — no-arg void shim:
```c
/* src/stdlib/iron_raylib.c:48-54 */
void Iron_window_close(void) {
    CloseWindow();
}

/* src/stdlib/iron_raylib.c:68-72 — dense one-liner style */
void Iron_window_toggle_fullscreen(void)          { ToggleFullscreen();         }
void Iron_window_toggle_borderless_windowed(void) { ToggleBorderlessWindowed(); }
void Iron_window_maximize(void)                   { MaximizeWindow();           }
void Iron_window_minimize(void)                   { MinimizeWindow();           }
void Iron_window_restore(void)                    { RestoreWindow();            }
```

Adapt for Phase 63:
```c
/* iron_raylib.c — Phase 63 section after line 563 */
void Iron_draw_begin(void)             { BeginDrawing();     }
void Iron_draw_end(void)               { EndDrawing();       }
void Iron_draw_end_mode_2d(void)       { EndMode2D();        }
void Iron_draw_end_texture_mode(void)  { EndTextureMode();   }
void Iron_draw_end_shader_mode(void)   { EndShaderMode();    }
void Iron_draw_end_blend_mode(void)    { EndBlendMode();     }
void Iron_draw_end_scissor_mode(void)  { EndScissorMode();   }
```

---

#### Template B: Struct-by-value INPUT — single struct arg (Color / Camera / RenderTexture / Shader)

**Applies to (4 functions + re-used for every DRAW2D function that takes a Color):** `Draw.clear(color)`, `Draw.begin_mode_2d(camera)`, `Draw.begin_texture_mode(target)`, `Draw.begin_shader_mode(shader)`.

**Analog:** `src/stdlib/iron_raylib.c:88-96` — `Iron_window_set_icon` (the **only** existing struct-by-value INPUT shim; every Phase 63 struct-input binding follows this memcpy pattern).

Excerpt — struct-by-value input via memcpy:
```c
/* src/stdlib/iron_raylib.c:88-96 */
void Iron_window_set_icon(struct Iron_Image image) {
    /* Iron_Image is byte-compatible with raylib Image (verified by
     * iron_raylib_layout.c _Static_assert grid, Plan 60-03). Copy
     * via memcpy to avoid any strict-aliasing warnings; the compiler
     * will elide it. */
    Image rl_image;
    memcpy(&rl_image, &image, sizeof(Image));
    SetWindowIcon(rl_image);
}
```

Adapt for Phase 63 `Draw.clear`:
```c
void Iron_draw_clear(struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    ClearBackground(rl);
}
```

Adapt for Phase 63 `Draw.begin_mode_2d`:
```c
void Iron_draw_begin_mode_2d(struct Iron_Camera camera) {
    Camera2D rl;
    memcpy(&rl, &camera, sizeof(Camera2D));
    BeginMode2D(rl);
}
```

Adapt for Phase 63 `Draw.begin_texture_mode`:
```c
void Iron_draw_begin_texture_mode(struct Iron_RenderTexture target) {
    RenderTexture2D rl;
    memcpy(&rl, &target, sizeof(RenderTexture2D));
    BeginTextureMode(rl);
}
```

Adapt for Phase 63 `Draw.begin_shader_mode`:
```c
void Iron_draw_begin_shader_mode(struct Iron_Shader shader) {
    Shader rl;
    memcpy(&rl, &shader, sizeof(Shader));
    BeginShaderMode(rl);
}
```

**Notes on layout:** Phase 60 `_Static_assert` grid in `src/stdlib/iron_raylib_layout.c` already pins byte-compat for `Iron_Color` / `Iron_Camera` / `Iron_RenderTexture` / `Iron_Shader`. No new asserts needed (Assumption A6).

---

#### Template C: Enum INPUT — single enum parameter (BlendMode)

**Applies to (1 function):** `Draw.begin_blend_mode(mode: BlendMode)`.

**Analog for enum-cast-to-int:** `src/stdlib/iron_raylib.c:74-76` — `Iron_window_set_state` (`uint32_t flags` → `(unsigned int)flags`) for bitmask flags, and `src/stdlib/iron_raylib.c:319-321` — `Iron_keyboard_set_exit_key` (`int32_t key` → `(int)key`) for typed enum ordinals.

Excerpt — enum to int cast:
```c
/* src/stdlib/iron_raylib.c:319-321 — closest precedent (typed enum → int) */
void Iron_keyboard_set_exit_key(int32_t key) {
    SetExitKey((int)key);
}
```

Adapt for `Draw.begin_blend_mode`:
```c
void Iron_draw_begin_blend_mode(int32_t mode) {
    BeginBlendMode((int)mode);
}
```

**Iron-side stub uses typed enum:** `func Draw.begin_blend_mode(mode: BlendMode) {}` — Iron lowers `BlendMode` to `int32_t` at FFI boundary (same lowering as every other enum in Phase 62).

---

#### Template D: Scalar-only shim (4 Int32 scissor args)

**Applies to (1 function):** `Draw.begin_scissor_mode(x, y, w, h)`.

**Analog:** `src/stdlib/iron_raylib.c:102-104` — `Iron_window_set_position` (two Int32 args with `(int)` cast).

Excerpt — scalar passthrough with int cast:
```c
/* src/stdlib/iron_raylib.c:102-104 */
void Iron_window_set_position(int32_t x, int32_t y) {
    SetWindowPosition((int)x, (int)y);
}
```

Adapt for `Draw.begin_scissor_mode`:
```c
void Iron_draw_begin_scissor_mode(int32_t x, int32_t y, int32_t w, int32_t h) {
    BeginScissorMode((int)x, (int)y, (int)w, (int)h);
}
```

---

#### Template E: Scalar + Color INPUT (dominant pattern, ~35 of 65 functions)

**Applies to:** `Draw.pixel`, `Draw.line`, `Draw.circle`, `Draw.circle_gradient`, `Draw.circle_lines`, `Draw.ellipse`, `Draw.ellipse_lines`, `Draw.rectangle`, `Draw.rectangle_gradient_v/h`, `Draw.rectangle_lines` — and every variant that mixes `int`/`float` scalars with a trailing `Color`.

**Analog (scalars + cast):** `src/stdlib/iron_raylib.c:361-363` — `Iron_mouse_set_scale` (two Float32 passthroughs, no cast). Plus the memcpy precedent from Template B for the Color.

Excerpt — combined scalars + Color memcpy (Phase 63 NEW pattern):
```c
/* Composite of iron_raylib.c:102-104 (scalar int casts) + iron_raylib.c:88-96 (memcpy Color) */
void Iron_draw_rectangle(int32_t x, int32_t y, int32_t w, int32_t h,
                         struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawRectangle((int)x, (int)y, (int)w, (int)h, rl);
}

void Iron_draw_pixel(int32_t x, int32_t y, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawPixel((int)x, (int)y, rl);
}

void Iron_draw_circle(int32_t cx, int32_t cy, float r, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawCircle((int)cx, (int)cy, r, rl);
}
```

**Two-color variants** (CircleGradient / RectangleGradientV / RectangleGradientH) — memcpy twice:
```c
void Iron_draw_rectangle_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
                                    struct Iron_Color top, struct Iron_Color bottom) {
    Color t, b;
    memcpy(&t, &top,    sizeof(Color));
    memcpy(&b, &bottom, sizeof(Color));
    DrawRectangleGradientV((int)x, (int)y, (int)w, (int)h, t, b);
}
```

**Four-color variant** (`DrawRectangleGradientEx`): memcpy 4 Colors + 1 Rectangle (Template F).

---

#### Template F: Vector2 INPUT by value + Color (common draw-primitive shape)

**Applies to (20+ functions):** `Draw.pixel_v`, `Draw.line_v/ex/bezier`, `Draw.circle_sector/_lines`, `Draw.circle_v/_lines_v`, `Draw.ring/_lines`, `Draw.rectangle_v`, `Draw.triangle/_lines`, `Draw.poly/_lines/_lines_ex`, all 5 `Draw.spline_segment_*`.

**Analog for Vector2 input by value:** no existing pure "Vector2-in" shim in Phase 61/62 (all Vector2 shims so far have been *returns*). The pattern is the same memcpy-to-local as Template B. The Phase 62 **outputs** at `src/stdlib/iron_raylib.c:337-343` establish the inverse direction; Phase 63 adapts it to the input direction.

Excerpt — combining precedents (memcpy input + scalars + Color memcpy):
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

void Iron_draw_line_ex(struct Iron_Vector2 start, struct Iron_Vector2 end,
                       float thick, struct Iron_Color color) {
    Vector2 s, e;
    Color c;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    memcpy(&c, &color, sizeof(Color));
    DrawLineEx(s, e, thick, c);
}

void Iron_draw_triangle(struct Iron_Vector2 v1, struct Iron_Vector2 v2,
                        struct Iron_Vector2 v3, struct Iron_Color color) {
    Vector2 a, b, cv;
    Color   col;
    memcpy(&a,   &v1,    sizeof(Vector2));
    memcpy(&b,   &v2,    sizeof(Vector2));
    memcpy(&cv,  &v3,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    DrawTriangle(a, b, cv, col);
}
```

**Rectangle-input variants** (`DrawRectangleRec`, `DrawRectanglePro`, `DrawRectangleLinesEx`, `DrawRectangleRounded`, `DrawRectangleRoundedLines`, `DrawRectangleRoundedLinesEx`, `DrawRectangleGradientEx`): same memcpy treatment for `Iron_Rectangle → Rectangle`:
```c
void Iron_draw_rectangle_rec(struct Iron_Rectangle rec, struct Iron_Color color) {
    Rectangle r; Color c;
    memcpy(&r, &rec,   sizeof(Rectangle));
    memcpy(&c, &color, sizeof(Color));
    DrawRectangleRec(r, c);
}
```

---

#### Template G: Struct-by-value Vector2 RETURN (spline evaluators, 5 functions)

**Applies to:** `Draw.get_spline_point_linear`, `Draw.get_spline_point_basis`, `Draw.get_spline_point_catmull_rom`, `Draw.get_spline_point_bezier_quadratic`, `Draw.get_spline_point_bezier_cubic`.

**Analog:** `src/stdlib/iron_raylib.c:137-142` — `Iron_window_get_window_position` (the memcpy-out form); and `src/stdlib/iron_raylib.c:337-343` — `Iron_mouse_get_position` (the field-assign form). Both work; Phase 62 preferred the field-assign style for simple x/y Vector2s.

Excerpt — memcpy-out form (Phase 61):
```c
/* src/stdlib/iron_raylib.c:137-142 */
struct Iron_Vector2 Iron_window_get_window_position(void) {
    Vector2 rl = GetWindowPosition();
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(out));
    return out;
}
```

Excerpt — field-assign form (Phase 62):
```c
/* src/stdlib/iron_raylib.c:337-343 */
struct Iron_Vector2 Iron_mouse_get_position(void) {
    Vector2 v = GetMousePosition();
    struct Iron_Vector2 out;
    out.x = v.x;
    out.y = v.y;
    return out;
}
```

Adapt for Phase 63 (combine Vector2 inputs + Vector2 output):
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

---

#### Template H: `[Vector2]` array INPUT — UNKNOWN ABI (9 functions)

**Applies to:** `Draw.line_strip`, `Draw.triangle_fan`, `Draw.triangle_strip`, `Draw.spline_linear`, `Draw.spline_basis`, `Draw.spline_catmull_rom`, `Draw.spline_bezier_quadratic`, `Draw.spline_bezier_cubic`.

**Analog:** **NONE EXISTS in iron_raylib.c.** This is the genuine unknown flagged by RESEARCH.md Pattern 4 + Pitfall 5 + Open Question 1. Phase 62 **sidestepped** the array ABI by building a FilePathList accessor pattern (`Iron_filepathlist_get(list, i) -> const char *`) — see `src/stdlib/iron_raylib.c:556-561`:

```c
/* src/stdlib/iron_raylib.c:556-561 — NOT an array param, but the closest
 * precedent for iterating a collection across the FFI. Phase 62 chose
 * accessor-indexed reads over passing an array wholesale. */
const char * Iron_filepathlist_get(struct Iron_FilePathList list, int32_t index) {
    if (index < 0 || (uint32_t)index >= list.count || list._paths == NULL) {
        return "";
    }
    return ((const char **)list._paths)[index];
}
```

**No other stdlib file uses a true `[T]` FFI parameter.** `iron_net.c` uses struct wrappers (`Iron_List_Iron_Address`), not raw array passthrough.

**Planner action:** Plan 63-03 Task 1 MUST run the micro-smoke-test from RESEARCH.md Pattern 4 before binding the 9 array-taking functions. Observe the C prototype ironc emits for:
```iron
func Draw.probe_array(points: [Vector2], count: Int32) {}
```

Tentative shim (IF `ARRAY_PARAM_CONST_PTR` fires — pending smoke test):
```c
void Iron_draw_line_strip(const struct Iron_Vector2 *points, int32_t count,
                          struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawLineStrip((const Vector2 *)points, (int)count, c);
}
```

**Fallback paths** (per RESEARCH.md Claude's Discretion):
1. If ironc emits `Iron_List_Iron_Vector2 *` struct wrapper → unpack `.items` / `.count` inside the shim.
2. If ironc rejects `[Vector2]` in foreign stubs → descope the 9 array draws to Phase 73, keep only the 5 `DrawSplineSegment<T>` + `DrawLine`/`DrawLineEx`/`DrawLineBezier` + `DrawTriangle`/`DrawTriangleLines`.

---

### `src/stdlib/iron_raylib.h` — 65 C prototypes (header, FFI declaration)

**Analog:** `src/stdlib/iron_raylib.h:435-511` — Phase 62 Input section (complete template for declaring each prototype category).

**Structure template** (copy-paste per template):
```c
/* src/stdlib/iron_raylib.h:476-481 — Touch (no-arg + Vector2 return + scalar args) */
int32_t              Iron_touch_get_x(void);
int32_t              Iron_touch_get_y(void);
struct Iron_Vector2  Iron_touch_get_position(int32_t index);
int32_t              Iron_touch_get_point_id(int32_t index);
int32_t              Iron_touch_get_point_count(void);
```

**Struct-by-value INPUT prototype format** — use `struct Iron_<T>` with the `struct` keyword prefix (Phase 61 pattern at `iron_raylib.h:370`):
```c
/* src/stdlib/iron_raylib.h:370 */
void Iron_window_set_icon(struct Iron_Image image);
```

**File append location:** `src/stdlib/iron_raylib.h:513` — the existing `/* ── 2D Drawing (Phase 63) ─────────── */` marker. Insert new prototypes BELOW the marker and ABOVE `/* ── Collision (Phase 64) ─── */` at line 514.

**Prototype block draft (65 lines, grouped by DRAW2D requirement)**:
```c
/* ── 2D Drawing (Phase 63) ────────────────────────────────────────── */

/* Frame + stack modes (DRAW2D-01..06) */
void Iron_draw_begin(void);
void Iron_draw_end(void);
void Iron_draw_clear(struct Iron_Color color);
void Iron_draw_begin_mode_2d(struct Iron_Camera camera);
void Iron_draw_end_mode_2d(void);
void Iron_draw_begin_texture_mode(struct Iron_RenderTexture target);
void Iron_draw_end_texture_mode(void);
void Iron_draw_begin_shader_mode(struct Iron_Shader shader);
void Iron_draw_end_shader_mode(void);
void Iron_draw_begin_blend_mode(int32_t mode);
void Iron_draw_end_blend_mode(void);
void Iron_draw_begin_scissor_mode(int32_t x, int32_t y, int32_t w, int32_t h);
void Iron_draw_end_scissor_mode(void);

/* Pixel primitives (DRAW2D-07) */
void Iron_draw_pixel(int32_t x, int32_t y, struct Iron_Color color);
void Iron_draw_pixel_v(struct Iron_Vector2 position, struct Iron_Color color);

/* Line primitives (DRAW2D-08) — array prototype for line_strip TBD post-smoke-test */

/* ... (full listing continues per RESEARCH.md function table) ... */

/* Spline evaluators (DRAW2D-16) */
struct Iron_Vector2 Iron_draw_get_spline_point_linear(struct Iron_Vector2 start, struct Iron_Vector2 end, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_basis(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_catmull_rom(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_bezier_quadratic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 p3, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_bezier_cubic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 c3, struct Iron_Vector2 p4, float t);
```

---

### `src/stdlib/raylib.iron` — 65 Iron-side stubs (model — empty-body foreign method stubs)

**Analog:** `src/stdlib/raylib.iron:1176-1310` — Phase 62 Input section. Mirror every surface shape.

#### Pattern I.1 — void no-arg method (Template A)

Analog — `src/stdlib/raylib.iron:1002-1003`:
```iron
func Window.close() {}
func Window.should_close() -> Bool {}
```

Phase 63 adaptation:
```iron
func Draw.begin() {}
func Draw.end() {}
func Draw.end_mode_2d() {}
func Draw.end_texture_mode() {}
func Draw.end_shader_mode() {}
func Draw.end_blend_mode() {}
func Draw.end_scissor_mode() {}
```

**Iron keyword collision check (Open Question 2):** `func Draw.end()` — Iron has no reserved word `end`. Plan 63-01 should run a quick `ironc check` probe as Task 1 verify; if parser rejects, rename to `Draw.finish()` (falls back to Claude's Discretion per RESEARCH.md).

#### Pattern I.2 — Single struct-input method (Template B)

Analog — `src/stdlib/raylib.iron:1038`:
```iron
func Window.set_icon(image: Image) {}
```

Phase 63 adaptation:
```iron
func Draw.clear(color: Color) {}
func Draw.begin_mode_2d(camera: Camera) {}
func Draw.begin_texture_mode(target: RenderTexture) {}
func Draw.begin_shader_mode(shader: Shader) {}
```

#### Pattern I.3 — Enum-input method (Template C)

Analog — `src/stdlib/raylib.iron:1028`:
```iron
func Window.set_state(flags: ConfigFlags) {}
```

Phase 63 adaptation:
```iron
func Draw.begin_blend_mode(mode: BlendMode) {}
```

#### Pattern I.4 — Int scalar args (Template D)

Analog — `src/stdlib/raylib.iron:1040`:
```iron
func Window.set_position(x: Int32, y: Int32) {}
```

Phase 63 adaptation:
```iron
func Draw.begin_scissor_mode(x: Int32, y: Int32, w: Int32, h: Int32) {}
```

#### Pattern I.5 — Scalars + Color tail (Template E, dominant shape)

Analog — combines two Phase 62 patterns. No exact precedent with `Color` param yet; closest is `Keyboard.set_exit_key(key: KeyboardKey)` at `raylib.iron:1183` plus `Window.set_icon(image: Image)` at `raylib.iron:1038`:
```iron
/* raylib.iron:1183 */
func Keyboard.set_exit_key(key: KeyboardKey) {}

/* raylib.iron:1038 */
func Window.set_icon(image: Image) {}
```

Phase 63 adaptation (the `Color` parameter is just another object-by-value argument):
```iron
func Draw.pixel(x: Int32, y: Int32, color: Color) {}
func Draw.line(x1: Int32, y1: Int32, x2: Int32, y2: Int32, color: Color) {}
func Draw.circle(cx: Int32, cy: Int32, r: Float32, color: Color) {}
func Draw.rectangle(x: Int32, y: Int32, w: Int32, h: Int32, color: Color) {}
func Draw.ellipse(cx: Int32, cy: Int32, rh: Float32, rv: Float32, color: Color) {}
```

#### Pattern I.6 — Vector2 input + Color tail (Template F)

Analog — no existing stub takes Vector2-by-value. Closest: `Window.set_icon(image: Image)` shows object-by-value input; the Iron-side method syntax is the same.

Phase 63 adaptation:
```iron
func Draw.pixel_v(position: Vector2, color: Color) {}
func Draw.line_v(start: Vector2, end: Vector2, color: Color) {}
func Draw.line_ex(start: Vector2, end: Vector2, thick: Float32, color: Color) {}
func Draw.triangle(v1: Vector2, v2: Vector2, v3: Vector2, color: Color) {}
func Draw.rectangle_rec(rec: Rectangle, color: Color) {}
func Draw.rectangle_pro(rec: Rectangle, origin: Vector2, rotation: Float32, color: Color) {}
```

#### Pattern I.7 — Vector2 RETURN (Template G)

Analog — `src/stdlib/raylib.iron:1205-1206`:
```iron
func Mouse.get_position() -> Vector2 {}
func Mouse.get_delta() -> Vector2 {}
```

Phase 63 adaptation:
```iron
func Draw.get_spline_point_linear(start: Vector2, end: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_basis(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_catmull_rom(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_bezier_quadratic(p1: Vector2, c2: Vector2, p3: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_bezier_cubic(p1: Vector2, c2: Vector2, c3: Vector2, p4: Vector2, t: Float32) -> Vector2 {}
```

**Float32 vs Float pitfall (RESEARCH.md Pitfall 1):** `t: Float32` — NOT `Float`. Iron's default `Float` is `double`; raylib expects `float`. Docstring must direct users to write `Float32(0.5)` at call site, matching the Plan 60-08 rescue palette `Vector2(Float32(0.0), Float32(0.0))` pattern from `tests/manual/game_raylib.iron:19`.

#### Pattern I.8 — `[Vector2]` array input (Template H) — PENDING SMOKE TEST

No precedent in raylib.iron. Tentative shape (per RESEARCH.md):
```iron
func Draw.line_strip(points: [Vector2], count: Int32, color: Color) {}
func Draw.spline_linear(points: [Vector2], count: Int32, thick: Float32, color: Color) {}
```

**File append location:** `src/stdlib/raylib.iron` tail (after line 1311's `func FilePathList.get(index: Int32) -> String {}`). Put the Phase 63 block under a new section banner:
```iron
-- ══════════════════════════════════════════════════════════════════════
-- 2D Drawing (Phase 63)
-- ══════════════════════════════════════════════════════════════════════
--
-- Phase 63 binds raylib's `rshapes.c` + `rcore.c` draw-mode functions
-- to the `Draw` namespace declared in Plan 60-06. Every stub below
-- lowers to `Iron_draw_<name>(...)` in C, implemented by
-- src/stdlib/iron_raylib.c. Shim-only pattern per 60-CONTEXT.md.
```

Matches the Phase 62 Input banner format at `raylib.iron:978-979`.

---

### `tests/manual/game_raylib.iron` — Phase 63 consumer file edit

**Analog:** `tests/manual/game_raylib.iron:32-34` (Phase 62 rewrite pattern just landed):
```iron
/* Before Plan 62-04 */
-- PHASE 62: if Input.is_key_down(down_key) { ... }

/* After Plan 62-04 */
-- PHASE 63: Draw.begin() / Draw.clear(bg) / Draw.end()
-- Phase 62: poll the keyboard once to exercise the down_key binding.
val _kb_down: Bool = Keyboard.is_down(down_key)
```

**Phase 63 rewrite:** the single `-- PHASE 63:` marker at line 32 becomes an actual Draw frame. Use the existing `bg` local (declared at line 22 as `val bg: Color = BLACK`) + the existing `fg`/`highlight`/`accent` Color locals:
```iron
Window.init(800, 600, title)
Window.set_target_fps(60)
val _should: Bool = Window.should_close()
-- Phase 62: poll the keyboard once to exercise the down_key binding.
val _kb_down: Bool = Keyboard.is_down(down_key)
-- Phase 63: render a single frame to exercise the Draw namespace.
Draw.begin()
Draw.clear(bg)
Draw.rectangle(100, 100, 50, 50, fg)
Draw.end()
Window.close()
```

---

### `examples/pong/pong.iron` — Phase 63 consumer file edit (canonical validation target)

**Analog:** `examples/pong/pong.iron:89-93` (Phase 62 re-enablement pattern):
```iron
/* Pattern — placeholder locals declared, keyboard calls consume them */
val _kb_start: Bool      = Keyboard.is_pressed(start_key)
val _kb_left_up: Bool    = Keyboard.is_down(left_up)
```

**Phase 63 rewrite:** the 7 `-- PHASE 63:` markers at lines 95-101 become real Draw calls:
```iron
/* Before Plan 63-04 — 7 markers */
-- PHASE 63: Draw.begin()
-- PHASE 63: Draw.clear(bg)
-- PHASE 63: DrawLine(SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H, divider)
-- PHASE 63: DrawRectangle(0, left_y, PADDLE_W, PADDLE_H, fg)
-- PHASE 63: DrawText(score_str, SCREEN_W / 4, 20, 40, fg)
-- PHASE 63: DrawText("GAME OVER", ..., accent)
-- PHASE 63: Draw.end()

/* After Plan 63-04 — lines 95-101 rewritten */
Draw.begin()
Draw.clear(bg)
Draw.line(400, 0, 400, 600, divider)
Draw.rectangle(0, 250, 10, 100, fg)
-- PHASE 67: DrawText(score_str, 200, 20, 40, fg)    -- re-marked from PHASE 63 (mislabeled)
-- PHASE 67: DrawText("GAME OVER", 300, 280, 40, accent)  -- re-marked from PHASE 63 (mislabeled)
Draw.end()
```

**Two lines stay commented:** the DrawText lines (99-100) are re-marked as `-- PHASE 67:` per RESEARCH.md Open Question 3 (mislabeled — text belongs to Phase 67, not Phase 63).

---

### `tests/integration/web/hello_raylib.iron` — Phase 63 consumer file edit (web integration)

**Analog:** same file line 14-15 already has the exact marker style.

Rewrite line 15:
```iron
/* Before */
-- PHASE 63: Draw.begin(); Draw.clear(bg); Draw.end()

/* After */
Draw.begin()
Draw.clear(bg)
Draw.end()
```

Keep `bg`, `fg`, `title` locals alive via existing `_unused_*` bindings at lines 18-20 (or add a `Draw.rectangle(10, 10, 20, 20, fg)` call to consume `fg`).

---

## Shared Patterns

### Shared Pattern 1 — memcpy-based struct-by-value passing

**Source:** `src/stdlib/iron_raylib.c:88-96` (`Iron_window_set_icon`) — the canonical memcpy pattern referenced throughout the file.

**Apply to:** EVERY Phase 63 shim that takes any `struct Iron_<T>` parameter. (Approximately 60 of 65 Phase 63 shims — every draw function takes a `Color` at minimum.)

```c
/* Template — copy verbatim into each shim body before the raylib call */
Color rl;
memcpy(&rl, &color, sizeof(Color));
/* ... then call: */
DrawWhatever(..., rl);
```

**Rationale (from file comment, `iron_raylib.c:88-94`):**
> "Iron_Image is byte-compatible with raylib Image (verified by iron_raylib_layout.c _Static_assert grid, Plan 60-03). Copy via memcpy to avoid any strict-aliasing warnings; the compiler will elide it."

The same rationale applies to `Iron_Color` (Plan 60-02), `Iron_Vector2` (Plan 60-02), `Iron_Rectangle` (Plan 60-02), `Iron_Camera` (Plan 60-04), `Iron_RenderTexture` (Plan 60-03), `Iron_Shader` (Plan 60-04). All layouts pinned by the Phase 60 `_Static_assert` grid (Assumption A6 in RESEARCH.md).

**Include at file top:** `#include <string.h>` already present at `iron_raylib.c:28`. No new includes needed.

### Shared Pattern 2 — Int32 argument cast to C int

**Source:** `src/stdlib/iron_raylib.c:102-104` (`Iron_window_set_position`) and repeated ~30 times throughout Phase 61/62.

**Apply to:** Every shim that forwards an `int32_t` arg to a raylib function expecting `int`:
```c
SetWindowPosition((int)x, (int)y);       /* iron_raylib.c:103 */
IsKeyPressed((int)key);                  /* iron_raylib.c:295 */
GetTouchPosition((int)index);            /* iron_raylib.c:448 */
```

Phase 63 usage: `DrawPixel((int)x, (int)y, rl)`, `DrawCircle((int)cx, (int)cy, r, rl)`, etc.

### Shared Pattern 3 — Section-marker append

**Source:** `src/stdlib/iron_raylib.c:563` and `src/stdlib/iron_raylib.h:513`.

**Apply to:** All new code in iron_raylib.c / iron_raylib.h.

Both files contain placeholder section markers. Append Phase 63 content immediately under `/* ── 2D Drawing (Phase 63) ─── */` and above `/* ── Collision (Phase 64) ─── */`. Never modify Phase 60/61/62 content above the marker — purely additive.

### Shared Pattern 4 — Struct-by-value return (5 instances in Phase 63)

**Source:** `src/stdlib/iron_raylib.c:137-142` (memcpy form) and `src/stdlib/iron_raylib.c:337-343` (field-assign form).

**Apply to:** All 5 `Iron_draw_get_spline_point_<variant>` shims. RESEARCH.md Pattern 3 recommends the memcpy form (matches Phase 61's choice); Phase 62 used field-assign for Vector2 specifically. Either is correct — Phase 63 planner may pick for consistency. **Recommendation: pick memcpy** to maintain parity with the Color/Camera/Shader *input* shims that all use memcpy. Single style across all struct-by-value paths reduces reader load.

### Shared Pattern 5 — Docstring format for the raylib.iron stub block

**Source:** `src/stdlib/raylib.iron:1184-1213` (Mouse block) and `raylib.iron:1245-1255` (Touch block).

**Pattern structure:**
```iron
-- <Category name> (<REQ-ID>)
--
-- <1-2 sentence high-level description of what raylib does here.>
-- <Any quirk or lifecycle constraint — e.g., "must be called between
-- BeginDrawing and EndDrawing", "takes typed enum — users cast if
-- combining bits", etc.>
func Draw.method_name(arg: Type) -> RetType {}
func Draw.next_method(...) {}
```

**Apply to:** Every sub-block of 2-8 related Phase 63 stubs (frame/modes, pixels, lines, circles, ellipses, rings, rectangles, triangles, polygons, splines, spline-evaluators).

---

## No Analog Found

| File | Role | Data Flow | Reason |
|------|------|-----------|--------|
| `[Vector2]` array-taking draw shims (9 functions) | stdlib C shim | array passthrough FFI | **No precedent anywhere in iron_raylib.c.** Phase 62's FilePathList used an accessor pattern (`get(i) -> String`) to sidestep the array ABI. The `iron_net.c` `Iron_List_Iron_Address` pattern is a struct wrapper, not a raw passthrough. Planner MUST run micro-smoke-test (Pattern H above) in Plan 63-03 before committing to shim signatures. If ironc rejects or emits an unexpected ABI, fall back to one of two paths documented in RESEARCH.md Claude's Discretion: (a) fixed-segment `DrawSpline<T>` pattern using chained `DrawSplineSegment<T>` calls, or (b) descope to Phase 73. |

## Metadata

**Analog search scope:** `/home/victor/code/iron-lang/src/stdlib/iron_raylib.{c,h}`, `/home/victor/code/iron-lang/src/stdlib/raylib.iron`, `/home/victor/code/iron-lang/src/stdlib/iron_net.c`, `/home/victor/code/iron-lang/examples/pong/pong.iron`, `/home/victor/code/iron-lang/tests/manual/game_raylib.iron`, `/home/victor/code/iron-lang/tests/integration/web/hello_raylib.iron`, `/home/victor/code/iron-lang/src/vendor/raylib/raylib.h`.

**Files scanned:** 9 primary + RESEARCH.md / 62-04-PLAN.md / 62-CONTEXT.md for upstream context.

**Pattern extraction date:** 2026-04-16.

**Key inheritance chains:**
- Template A (no-arg void): `iron_raylib.c:48` → `iron_raylib.c:68-72` → Phase 63 begin/end shims.
- Template B (struct-by-value input): `iron_raylib.c:88-96` (`Iron_window_set_icon`) → 4 Phase 63 begin-mode shims + ~60 Color-consuming draw shims.
- Template C (enum input): `iron_raylib.c:319-321` (`Iron_keyboard_set_exit_key`) → `Iron_draw_begin_blend_mode`.
- Template D (Int32 scalar): `iron_raylib.c:102-104` (`Iron_window_set_position`) → `Iron_draw_begin_scissor_mode`.
- Template E (scalar+Color): Templates B + D composed → ~35 Phase 63 draw shims.
- Template F (Vector2 input): Template B extended (Iron_Vector2 is just another Iron struct) → ~20 Phase 63 draw shims.
- Template G (Vector2 return): `iron_raylib.c:137-142` (`Iron_window_get_window_position`) → 5 Phase 63 spline evaluator shims.
- Template H (array input): **NO PRECEDENT** → probe required.

**Iron-side inheritance (raylib.iron):**
- `Window.set_icon(image: Image)` at `raylib.iron:1038` → Template I.2 model for `Draw.clear(color: Color)`.
- `Mouse.get_position() -> Vector2` at `raylib.iron:1205` → Template I.7 model for spline evaluators.
- `Window.set_state(flags: ConfigFlags)` at `raylib.iron:1028` → Template I.3 model for `Draw.begin_blend_mode(mode: BlendMode)`.
- `Window.set_position(x: Int32, y: Int32)` at `raylib.iron:1040` → Template I.4 model for `Draw.begin_scissor_mode(...)`.
