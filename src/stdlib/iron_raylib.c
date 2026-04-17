/* iron_raylib.c — Phase 60+ raylib stdlib shim implementation.
 *
 * This file is the single C translation unit for every raylib wrapper
 * function exposed to Iron code. Every `func Texture.draw(t: Texture,
 * ...) {}` empty-body stub in src/stdlib/raylib.iron lowers to a call
 * to `Iron_texture_draw(...)` which is implemented here.
 *
 * The file is populated incrementally:
 *   Phase 60: this scaffold (no wrappers, section markers only).
 *   Phase 61: Window & System wrappers (~35 functions).
 *   Phase 62: Input wrappers.
 *   ... through Phase 72.
 *
 * All wrappers forward to raylib's native functions declared in
 * src/vendor/raylib/raylib.h. Iron-side struct mirrors (Iron_Vector3
 * etc.) are layout-compatible with raylib's types, so pass-by-value
 * crosses the FFI boundary with zero marshalling — the shim simply
 * reinterpret-casts (or type-punned-copies) between the two layouts.
 *
 * Build integration:
 *   - src/cli/build.c compiles this file alongside iron_net.c and the
 *     other stdlib TUs when opts.use_raylib is true.
 *   - src/cli/build_web.c does the same for the emcc path.
 *   - src/stdlib/iron_raylib_layout.c is the sibling TU that proves
 *     byte-for-byte layout compatibility at build time via _Static_assert.
 */

#include <string.h>
#include "iron_raylib.h"
#include "raylib.h"
/* raymath.h reuses raylib's Vector2/3/4/Matrix typedefs via the
 * RL_VECTOR2_TYPE guard family. raylib.h defines those guards BEFORE
 * declaring the typedefs (line 167), so raylib.h must be included
 * first; raymath.h then skips its duplicate typedefs cleanly.
 * RAYMATH_STATIC_INLINE picks the `static inline` RMAPI flavor so each
 * TU owns its own copies without link-time collision against rcore.c
 * (which owns RAYMATH_IMPLEMENTATION at its line 116). */
#define RAYMATH_STATIC_INLINE
#include "raymath.h"

/* ════════════════════════════════════════════════════════════════════
 * Section markers — wrapper functions land here in later phases.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Window & System (Phase 61) ───────────────────────────────────── */

/* Window lifecycle (WIN-01) */

void Iron_window_init(int32_t w, int32_t h, Iron_String title) {
    /* iron_string_cstr yields a NUL-terminated const char * into the
     * Iron-owned string buffer. raylib copies the title into its own
     * storage via strdup internally, so we do not need to keep the
     * pointer alive past this call. */
    InitWindow((int)w, (int)h, iron_string_cstr(&title));
}

void Iron_window_close(void) {
    CloseWindow();
}

bool Iron_window_should_close(void) {
    return WindowShouldClose();
}

/* Window state queries (WIN-02) */

bool Iron_window_is_ready(void)      { return IsWindowReady();      }
bool Iron_window_is_fullscreen(void) { return IsWindowFullscreen(); }
bool Iron_window_is_hidden(void)     { return IsWindowHidden();     }
bool Iron_window_is_minimized(void)  { return IsWindowMinimized();  }
bool Iron_window_is_maximized(void)  { return IsWindowMaximized();  }
bool Iron_window_is_focused(void)    { return IsWindowFocused();    }
bool Iron_window_is_resized(void)    { return IsWindowResized();    }

/* Window state toggles (WIN-03) */

void Iron_window_toggle_fullscreen(void)          { ToggleFullscreen();         }
void Iron_window_toggle_borderless_windowed(void) { ToggleBorderlessWindowed(); }
void Iron_window_maximize(void)                   { MaximizeWindow();           }
void Iron_window_minimize(void)                   { MinimizeWindow();           }
void Iron_window_restore(void)                    { RestoreWindow();            }

void Iron_window_set_state(uint32_t flags) {
    SetWindowState((unsigned int)flags);
}

void Iron_window_clear_state(uint32_t flags) {
    ClearWindowState((unsigned int)flags);
}

bool Iron_window_is_state(uint32_t flag) {
    return IsWindowState((unsigned int)flag);
}

/* Window runtime properties (WIN-04) */

void Iron_window_set_icon(struct Iron_Image image) {
    /* Iron_Image is byte-compatible with raylib Image (verified by
     * iron_raylib_layout.c _Static_assert grid, Plan 60-03). Copy
     * via memcpy to avoid any strict-aliasing warnings; the compiler
     * will elide it. */
    Image rl_image;
    memcpy(&rl_image, &image, sizeof(Image));
    SetWindowIcon(rl_image);
}

void Iron_window_set_title(Iron_String title) {
    SetWindowTitle(iron_string_cstr(&title));
}

void Iron_window_set_position(int32_t x, int32_t y) {
    SetWindowPosition((int)x, (int)y);
}

void Iron_window_set_monitor(int32_t monitor) {
    SetWindowMonitor((int)monitor);
}

void Iron_window_set_min_size(int32_t w, int32_t h) {
    SetWindowMinSize((int)w, (int)h);
}

void Iron_window_set_max_size(int32_t w, int32_t h) {
    SetWindowMaxSize((int)w, (int)h);
}

void Iron_window_set_size(int32_t w, int32_t h) {
    SetWindowSize((int)w, (int)h);
}

void Iron_window_set_opacity(float opacity) {
    SetWindowOpacity(opacity);
}

void Iron_window_set_focused(void) {
    SetWindowFocused();
}

/* Screen and window geometry (WIN-05) */

int32_t Iron_window_get_screen_width(void)  { return (int32_t)GetScreenWidth();  }
int32_t Iron_window_get_screen_height(void) { return (int32_t)GetScreenHeight(); }
int32_t Iron_window_get_render_width(void)  { return (int32_t)GetRenderWidth();  }
int32_t Iron_window_get_render_height(void) { return (int32_t)GetRenderHeight(); }

struct Iron_Vector2 Iron_window_get_window_position(void) {
    Vector2 rl = GetWindowPosition();
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(out));
    return out;
}

struct Iron_Vector2 Iron_window_get_window_scale_dpi(void) {
    Vector2 rl = GetWindowScaleDPI();
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(out));
    return out;
}

/* Monitor enumeration (WIN-06) */

int32_t Iron_window_get_monitor_count(void)   { return (int32_t)GetMonitorCount();   }
int32_t Iron_window_get_current_monitor(void) { return (int32_t)GetCurrentMonitor(); }

struct Iron_Vector2 Iron_window_get_monitor_position(int32_t monitor) {
    Vector2 rl = GetMonitorPosition((int)monitor);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(out));
    return out;
}

int32_t Iron_window_get_monitor_width(int32_t monitor) {
    return (int32_t)GetMonitorWidth((int)monitor);
}
int32_t Iron_window_get_monitor_height(int32_t monitor) {
    return (int32_t)GetMonitorHeight((int)monitor);
}
int32_t Iron_window_get_monitor_physical_width(int32_t monitor) {
    return (int32_t)GetMonitorPhysicalWidth((int)monitor);
}
int32_t Iron_window_get_monitor_physical_height(int32_t monitor) {
    return (int32_t)GetMonitorPhysicalHeight((int)monitor);
}
int32_t Iron_window_get_monitor_refresh_rate(int32_t monitor) {
    return (int32_t)GetMonitorRefreshRate((int)monitor);
}

/* GetMonitorName returns a GLFW-internal buffer pointer that may be
 * clobbered on subsequent calls. Copy into Iron-managed string via
 * iron_string_from_cstr for stable lifetime. */
Iron_String Iron_window_get_monitor_name(int32_t monitor) {
    const char *name = GetMonitorName((int)monitor);
    if (name == NULL) {
        return iron_string_from_literal("", 0);
    }
    return iron_string_from_cstr(name, strlen(name));
}

/* Clipboard (WIN-07) */

/* GetClipboardText returns a GLFW-internal pointer that may be
 * overwritten on the next clipboard call. Copy into Iron-managed
 * string. Same safety pattern as GetMonitorName. */
Iron_String Iron_window_get_clipboard_text(void) {
    const char *text = GetClipboardText();
    if (text == NULL) {
        return iron_string_from_literal("", 0);
    }
    return iron_string_from_cstr(text, strlen(text));
}

void Iron_window_set_clipboard_text(Iron_String text) {
    SetClipboardText(iron_string_cstr(&text));
}

/* GetClipboardImage — first >16-byte struct-by-value return in
 * Phase 61. Iron_Image is byte-compatible with raylib Image
 * (Plan 60-03 _Static_assert grid). memcpy into Iron_Image and
 * return by value. */
struct Iron_Image Iron_window_get_clipboard_image(void) {
    Image rl = GetClipboardImage();
    struct Iron_Image out;
    memcpy(&out, &rl, sizeof(out));
    return out;
}

/* Screenshot (WIN-09) */

void Iron_window_take_screenshot(Iron_String filename) {
    TakeScreenshot(iron_string_cstr(&filename));
}

/* Screen-to-Image (WIN-09 second clause) — captures the current
 * framebuffer into a CPU-side Image by value. Must be called
 * between Draw.begin() and Draw.end() in the frame loop once
 * Phase 63 lands those; Phase 61's FFI wrapper just proves the
 * binding works. Caller owns the returned Image and is responsible
 * for unloading it when done. */
struct Iron_Image Iron_window_load_image_from_screen(void) {
    Image rl = LoadImageFromScreen();
    struct Iron_Image out;
    memcpy(&out, &rl, sizeof(out));
    return out;
}

/* Config flags + trace log + event waiting (WIN-08, WIN-10) */

void Iron_window_set_config_flags(uint32_t flags) {
    SetConfigFlags((unsigned int)flags);
}

void Iron_window_set_trace_log_level(int32_t level) {
    SetTraceLogLevel((int)level);
}

void Iron_window_enable_event_waiting(void)  { EnableEventWaiting();  }
void Iron_window_disable_event_waiting(void) { DisableEventWaiting(); }

/* Cursor (WIN-11) */

void Iron_window_show_cursor(void)         { ShowCursor();              }
void Iron_window_hide_cursor(void)         { HideCursor();              }
bool Iron_window_is_cursor_hidden(void)    { return IsCursorHidden();   }
void Iron_window_enable_cursor(void)       { EnableCursor();            }
void Iron_window_disable_cursor(void)      { DisableCursor();           }
bool Iron_window_is_cursor_on_screen(void) { return IsCursorOnScreen(); }

void Iron_window_set_mouse_cursor(int32_t cursor) {
    SetMouseCursor((int)cursor);
}

/* Frame loop (WIN-12) */

void Iron_window_set_target_fps(int32_t fps) {
    SetTargetFPS((int)fps);
}

int32_t Iron_window_get_fps(void) {
    return (int32_t)GetFPS();
}

float Iron_window_get_frame_time(void) {
    return GetFrameTime();
}

double Iron_window_get_time(void) {
    return GetTime();
}

/* URL (WIN-13) */
void Iron_window_open_url(Iron_String url) {
    OpenURL(iron_string_cstr(&url));
}

/* Wait (FILE-07) */
void Iron_window_wait_time(double seconds) {
    WaitTime(seconds);
}

/* ── Input (Phase 62) ─────────────────────────────────────────────── */

/* Keyboard (INPUT-01, INPUT-02, INPUT-03) */

bool Iron_keyboard_is_pressed(int32_t key)        { return IsKeyPressed((int)key);        }
bool Iron_keyboard_is_pressed_repeat(int32_t key) { return IsKeyPressedRepeat((int)key);  }
bool Iron_keyboard_is_down(int32_t key)           { return IsKeyDown((int)key);           }
bool Iron_keyboard_is_released(int32_t key)       { return IsKeyReleased((int)key);       }
bool Iron_keyboard_is_up(int32_t key)             { return IsKeyUp((int)key);             }

/* GetKeyPressed returns 0 when the queue is empty, or a KEY_* ordinal
 * (32..348) otherwise. Iron's KeyboardKey enum defines NULL=0 and every
 * KEY_* in that range, so a direct (int32_t) cast at the FFI boundary
 * is safe — Iron-side, the user receives a typed KeyboardKey value
 * because the `func Keyboard.get_pressed() -> KeyboardKey` stub
 * declares the return type. This shim returns int32_t and Iron's
 * emit_c layer treats KeyboardKey as its underlying integer type. */
int32_t Iron_keyboard_get_pressed(void) {
    return (int32_t)GetKeyPressed();
}

/* GetCharPressed returns a Unicode codepoint (e.g. 'A' = 65, 'é' = 233)
 * or 0 when the queue is empty. NOT a KeyboardKey enum value — keep
 * as Int32 on the Iron side. */
int32_t Iron_keyboard_get_char_pressed(void) {
    return (int32_t)GetCharPressed();
}

void Iron_keyboard_set_exit_key(int32_t key) {
    SetExitKey((int)key);
}

/* Mouse (INPUT-04, INPUT-05, INPUT-06, INPUT-07) */

bool Iron_mouse_is_button_pressed(int32_t button)  { return IsMouseButtonPressed((int)button);  }
bool Iron_mouse_is_button_down(int32_t button)     { return IsMouseButtonDown((int)button);     }
bool Iron_mouse_is_button_released(int32_t button) { return IsMouseButtonReleased((int)button); }
bool Iron_mouse_is_button_up(int32_t button)       { return IsMouseButtonUp((int)button);       }

int32_t Iron_mouse_get_x(void) { return (int32_t)GetMouseX(); }
int32_t Iron_mouse_get_y(void) { return (int32_t)GetMouseY(); }

/* Struct-by-value Vector2 returns — Phase 61 pattern. The
 * _Static_assert grid in iron_raylib_layout.c guarantees
 * Iron_Vector2 is byte-identical to raylib's Vector2, so the
 * field-copy is only a style convention. */
struct Iron_Vector2 Iron_mouse_get_position(void) {
    Vector2 v = GetMousePosition();
    struct Iron_Vector2 out;
    out.x = v.x;
    out.y = v.y;
    return out;
}

struct Iron_Vector2 Iron_mouse_get_delta(void) {
    Vector2 v = GetMouseDelta();
    struct Iron_Vector2 out;
    out.x = v.x;
    out.y = v.y;
    return out;
}

void Iron_mouse_set_position(int32_t x, int32_t y) {
    SetMousePosition((int)x, (int)y);
}

void Iron_mouse_set_offset(int32_t offset_x, int32_t offset_y) {
    SetMouseOffset((int)offset_x, (int)offset_y);
}

void Iron_mouse_set_scale(float scale_x, float scale_y) {
    SetMouseScale(scale_x, scale_y);
}

float Iron_mouse_get_wheel_move(void) {
    return GetMouseWheelMove();
}

struct Iron_Vector2 Iron_mouse_get_wheel_move_v(void) {
    Vector2 v = GetMouseWheelMoveV();
    struct Iron_Vector2 out;
    out.x = v.x;
    out.y = v.y;
    return out;
}

void Iron_mouse_set_cursor(int32_t cursor) {
    SetMouseCursor((int)cursor);
}

/* Gamepad (INPUT-08, INPUT-09, INPUT-10) */

bool Iron_gamepad_is_available(int32_t gamepad) {
    return IsGamepadAvailable((int)gamepad);
}

/* Returns raylib's internal gamepad-name string. The pointer is
 * stable for the lifetime of the raylib session; Iron's runtime
 * copies on the receiving side via the same helper used by
 * Iron_window_get_monitor_name. Returns NULL / empty string when
 * the gamepad index is out of range (raylib returns a literal
 * "UNKNOWN" string in practice — pass it through unmodified). */
const char * Iron_gamepad_get_name(int32_t gamepad) {
    return GetGamepadName((int)gamepad);
}

bool Iron_gamepad_is_button_pressed(int32_t gamepad, int32_t button) {
    return IsGamepadButtonPressed((int)gamepad, (int)button);
}

bool Iron_gamepad_is_button_down(int32_t gamepad, int32_t button) {
    return IsGamepadButtonDown((int)gamepad, (int)button);
}

bool Iron_gamepad_is_button_released(int32_t gamepad, int32_t button) {
    return IsGamepadButtonReleased((int)gamepad, (int)button);
}

bool Iron_gamepad_is_button_up(int32_t gamepad, int32_t button) {
    return IsGamepadButtonUp((int)gamepad, (int)button);
}

/* GetGamepadButtonPressed returns 0 (UNKNOWN) when the queue is
 * empty, or a GamepadButton ordinal 1..17 otherwise. Every ordinal
 * in that range is defined in Iron's GamepadButton enum (Plan
 * 60-07). The Iron-side stub declares the return type as
 * GamepadButton; this shim returns int32_t and Iron's codegen
 * applies the typed cast at the call site. */
int32_t Iron_gamepad_get_button_pressed(void) {
    return (int32_t)GetGamepadButtonPressed();
}

int32_t Iron_gamepad_get_axis_count(int32_t gamepad) {
    return (int32_t)GetGamepadAxisCount((int)gamepad);
}

float Iron_gamepad_get_axis_movement(int32_t gamepad, int32_t axis) {
    return GetGamepadAxisMovement((int)gamepad, (int)axis);
}

/* Returns the count of newly registered mappings (raylib's own
 * SDL_GameControllerDB parser tallies them). Iron-side stub keeps
 * this as Int32; users may ignore the return. */
int32_t Iron_gamepad_set_mappings(const char * mappings) {
    return (int32_t)SetGamepadMappings(mappings);
}

void Iron_gamepad_set_vibration(int32_t gamepad, float left_motor, float right_motor, float duration) {
    SetGamepadVibration((int)gamepad, left_motor, right_motor, duration);
}

/* Touch (INPUT-11) */

int32_t Iron_touch_get_x(void) { return (int32_t)GetTouchX(); }
int32_t Iron_touch_get_y(void) { return (int32_t)GetTouchY(); }

struct Iron_Vector2 Iron_touch_get_position(int32_t index) {
    Vector2 v = GetTouchPosition((int)index);
    struct Iron_Vector2 out;
    out.x = v.x;
    out.y = v.y;
    return out;
}

int32_t Iron_touch_get_point_id(int32_t index) {
    return (int32_t)GetTouchPointId((int)index);
}

int32_t Iron_touch_get_point_count(void) {
    return (int32_t)GetTouchPointCount();
}

/* Gestures (INPUT-12) */

/* raylib's SetGesturesEnabled / IsGestureDetected take `unsigned int`
 * bitmasks. Iron's Gesture enum values are powers of two (1, 2, 4,
 * 8, 16, 32, 64, 128, 256, 512); passing a single enum value or an
 * OR-ed combination both work. Iron does not yet expose bitwise OR
 * on enum values ergonomically (Phase 73 polish), so users cast to
 * UInt32 for multi-gesture masks. */
void Iron_gestures_set_enabled(int32_t flags) {
    SetGesturesEnabled((unsigned int)flags);
}

bool Iron_gestures_is_detected(int32_t gesture) {
    return IsGestureDetected((unsigned int)gesture);
}

/* GetGestureDetected returns NONE (0) or one power-of-two in [1, 512].
 * Iron's Gesture enum covers all 11 values; Iron-side stub declares
 * the return type as `Gesture` and Iron's codegen treats this int32_t
 * as the typed enum. */
int32_t Iron_gestures_get_detected(void) {
    return (int32_t)GetGestureDetected();
}

float Iron_gestures_get_hold_duration(void) {
    return GetGestureHoldDuration();
}

struct Iron_Vector2 Iron_gestures_get_drag_vector(void) {
    Vector2 v = GetGestureDragVector();
    struct Iron_Vector2 out;
    out.x = v.x;
    out.y = v.y;
    return out;
}

float Iron_gestures_get_drag_angle(void) {
    return GetGestureDragAngle();
}

struct Iron_Vector2 Iron_gestures_get_pinch_vector(void) {
    Vector2 v = GetGesturePinchVector();
    struct Iron_Vector2 out;
    out.x = v.x;
    out.y = v.y;
    return out;
}

float Iron_gestures_get_pinch_angle(void) {
    return GetGesturePinchAngle();
}

/* File drop (INPUT-13) */

bool Iron_files_is_dropped(void) {
    return IsFileDropped();
}

/* Struct-by-value FilePathList return — Phase 60 `_Static_assert`
 * grid proves Iron_FilePathList is byte-compatible with raylib's
 * FilePathList (capacity/count/paths at offsets 0/4/8, total 16 bytes). */
struct Iron_FilePathList Iron_files_load_dropped(void) {
    FilePathList src = LoadDroppedFiles();
    struct Iron_FilePathList out;
    out.capacity = src.capacity;
    out.count = src.count;
    out._paths = (void *)src.paths;
    return out;
}

void Iron_files_unload_dropped(struct Iron_FilePathList list) {
    FilePathList fl;
    fl.capacity = list.capacity;
    fl.count = list.count;
    fl.paths = (char **)list._paths;
    UnloadDroppedFiles(fl);
}

/* FilePathList accessors (INPUT-13 iteration clause).
 *
 * `count` is a plain field read — exposed as a method for API
 * symmetry with other namespaces.
 *
 * `get(i)` returns the i-th path as a `const char *`; Iron's
 * runtime copies into an Iron-owned String on the receiving side
 * (same helper used by Iron_window_get_clipboard_text /
 * Iron_window_get_monitor_name). Bounds check against `count`
 * defensively — OOB returns an empty string literal instead of
 * segfaulting. */
int32_t Iron_filepathlist_count(struct Iron_FilePathList list) {
    return (int32_t)list.count;
}

const char * Iron_filepathlist_get(struct Iron_FilePathList list, int32_t index) {
    if (index < 0 || (uint32_t)index >= list.count || list._paths == NULL) {
        return "";
    }
    return ((const char **)list._paths)[index];
}

/* ── 2D Drawing (Phase 63) ────────────────────────────────────────── */

/* Frame (DRAW2D-01) — dense one-liner style matches iron_raylib.c:68-72. */

void Iron_draw_begin(void) { BeginDrawing(); }
void Iron_draw_end(void)   { EndDrawing();   }

/* Clear (DRAW2D-02) — first Color struct-by-value input.
 * Iron_Color is byte-compatible with raylib Color (Phase 60-02
 * _Static_assert grid verified size + per-field offsets). memcpy
 * avoids any strict-aliasing warnings; the compiler elides it.
 * Pattern copied from iron_raylib.c:88-96 (Iron_window_set_icon). */
void Iron_draw_clear(struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    ClearBackground(rl);
}

/* Camera2D mode (DRAW2D-03) — first Camera2D struct-by-value input.
 * Iron type is named `Camera` (no 2D suffix); raylib C type is
 * `Camera2D`. Phase 60-04 `_Static_assert(sizeof(struct Iron_Camera)
 * == sizeof(Camera2D))` locks this mapping. */
void Iron_draw_begin_mode_2d(struct Iron_Camera camera) {
    Camera2D rl;
    memcpy(&rl, &camera, sizeof(Camera2D));
    BeginMode2D(rl);
}

void Iron_draw_end_mode_2d(void) { EndMode2D(); }

/* Texture mode (DRAW2D-04) — first RenderTexture struct-by-value
 * input. raylib's RenderTexture2D is a typedef alias for
 * RenderTexture; both names produce identical layout. */
void Iron_draw_begin_texture_mode(struct Iron_RenderTexture target) {
    RenderTexture2D rl;
    memcpy(&rl, &target, sizeof(RenderTexture2D));
    BeginTextureMode(rl);
}

void Iron_draw_end_texture_mode(void) { EndTextureMode(); }

/* Shader mode (DRAW2D-05, part 1) — first Shader struct-by-value input.
 * Iron_Shader is 16 bytes (id:4 + 4 pad + _locs:8); raylib Shader is
 * 16 bytes (id:4 + 4 pad + int *locs:8). memcpy is safe. */
void Iron_draw_begin_shader_mode(struct Iron_Shader shader) {
    Shader rl;
    memcpy(&rl, &shader, sizeof(Shader));
    BeginShaderMode(rl);
}

void Iron_draw_end_shader_mode(void) { EndShaderMode(); }

/* Blend mode (DRAW2D-05, part 2) — typed BlendMode enum input.
 * Iron lowers BlendMode to int32_t at the FFI boundary (same as
 * every other enum in Phase 62). Cast to raylib's `int`. */
void Iron_draw_begin_blend_mode(int32_t mode) {
    BeginBlendMode((int)mode);
}

void Iron_draw_end_blend_mode(void) { EndBlendMode(); }

/* Scissor mode (DRAW2D-06) — four Int32 scalars. Pattern copied
 * from iron_raylib.c:102-104 (Iron_window_set_position). */
void Iron_draw_begin_scissor_mode(int32_t x, int32_t y, int32_t w, int32_t h) {
    BeginScissorMode((int)x, (int)y, (int)w, (int)h);
}

void Iron_draw_end_scissor_mode(void) { EndScissorMode(); }

/* Pixel primitives (DRAW2D-07) */

void Iron_draw_pixel(int32_t x, int32_t y, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawPixel((int)x, (int)y, rl);
}

void Iron_draw_pixel_v(struct Iron_Vector2 position, struct Iron_Color color) {
    Vector2 p;
    Color c;
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&c, &color,    sizeof(Color));
    DrawPixelV(p, c);
}

/* Line primitives (DRAW2D-08) — DrawLineStrip deferred to Plan 63-04 */

void Iron_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawLine((int)x1, (int)y1, (int)x2, (int)y2, rl);
}

void Iron_draw_line_v(struct Iron_Vector2 start, struct Iron_Vector2 end, struct Iron_Color color) {
    Vector2 s, e;
    Color c;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    memcpy(&c, &color, sizeof(Color));
    DrawLineV(s, e, c);
}

void Iron_draw_line_ex(struct Iron_Vector2 start, struct Iron_Vector2 end, float thick, struct Iron_Color color) {
    Vector2 s, e;
    Color c;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    memcpy(&c, &color, sizeof(Color));
    DrawLineEx(s, e, thick, c);
}

void Iron_draw_line_bezier(struct Iron_Vector2 start, struct Iron_Vector2 end, float thick, struct Iron_Color color) {
    Vector2 s, e;
    Color c;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    memcpy(&c, &color, sizeof(Color));
    DrawLineBezier(s, e, thick, c);
}

/* Circle primitives (DRAW2D-09) */

void Iron_draw_circle(int32_t cx, int32_t cy, float r, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawCircle((int)cx, (int)cy, r, rl);
}

void Iron_draw_circle_sector(struct Iron_Vector2 center, float r, float start, float end, int32_t segments, struct Iron_Color color) {
    Vector2 ct;
    Color cl;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&cl, &color,  sizeof(Color));
    DrawCircleSector(ct, r, start, end, (int)segments, cl);
}

void Iron_draw_circle_sector_lines(struct Iron_Vector2 center, float r, float start, float end, int32_t segments, struct Iron_Color color) {
    Vector2 ct;
    Color cl;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&cl, &color,  sizeof(Color));
    DrawCircleSectorLines(ct, r, start, end, (int)segments, cl);
}

/* Two-color variant — memcpy twice for the inner/outer gradient stops. */
void Iron_draw_circle_gradient(int32_t cx, int32_t cy, float r,
                               struct Iron_Color inner, struct Iron_Color outer) {
    Color i, o;
    memcpy(&i, &inner, sizeof(Color));
    memcpy(&o, &outer, sizeof(Color));
    DrawCircleGradient((int)cx, (int)cy, r, i, o);
}

void Iron_draw_circle_v(struct Iron_Vector2 center, float r, struct Iron_Color color) {
    Vector2 ct;
    Color cl;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&cl, &color,  sizeof(Color));
    DrawCircleV(ct, r, cl);
}

void Iron_draw_circle_lines(int32_t cx, int32_t cy, float r, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawCircleLines((int)cx, (int)cy, r, rl);
}

void Iron_draw_circle_lines_v(struct Iron_Vector2 center, float r, struct Iron_Color color) {
    Vector2 ct;
    Color cl;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&cl, &color,  sizeof(Color));
    DrawCircleLinesV(ct, r, cl);
}

/* Ellipse primitives (DRAW2D-10) */

void Iron_draw_ellipse(int32_t cx, int32_t cy, float rh, float rv, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawEllipse((int)cx, (int)cy, rh, rv, rl);
}

void Iron_draw_ellipse_lines(int32_t cx, int32_t cy, float rh, float rv, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawEllipseLines((int)cx, (int)cy, rh, rv, rl);
}

/* Ring primitives (DRAW2D-11) */

void Iron_draw_ring(struct Iron_Vector2 center, float inner_r, float outer_r, float start, float end, int32_t segments, struct Iron_Color color) {
    Vector2 ct;
    Color cl;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&cl, &color,  sizeof(Color));
    DrawRing(ct, inner_r, outer_r, start, end, (int)segments, cl);
}

void Iron_draw_ring_lines(struct Iron_Vector2 center, float inner_r, float outer_r, float start, float end, int32_t segments, struct Iron_Color color) {
    Vector2 ct;
    Color cl;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&cl, &color,  sizeof(Color));
    DrawRingLines(ct, inner_r, outer_r, start, end, (int)segments, cl);
}

/* Rectangle primitives (DRAW2D-12) */

void Iron_draw_rectangle(int32_t x, int32_t y, int32_t w, int32_t h, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawRectangle((int)x, (int)y, (int)w, (int)h, rl);
}

void Iron_draw_rectangle_v(struct Iron_Vector2 position, struct Iron_Vector2 size, struct Iron_Color color) {
    Vector2 p, s;
    Color c;
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&s, &size,     sizeof(Vector2));
    memcpy(&c, &color,    sizeof(Color));
    DrawRectangleV(p, s, c);
}

/* Rectangle struct-by-value INPUT — first use of this type. Phase 60-02
 * _Static_assert(sizeof(struct Iron_Rectangle) == sizeof(Rectangle)) pins layout. */
void Iron_draw_rectangle_rec(struct Iron_Rectangle rec, struct Iron_Color color) {
    Rectangle r;
    Color c;
    memcpy(&r, &rec,   sizeof(Rectangle));
    memcpy(&c, &color, sizeof(Color));
    DrawRectangleRec(r, c);
}

void Iron_draw_rectangle_pro(struct Iron_Rectangle rec, struct Iron_Vector2 origin, float rotation, struct Iron_Color color) {
    Rectangle r;
    Vector2 o;
    Color c;
    memcpy(&r, &rec,    sizeof(Rectangle));
    memcpy(&o, &origin, sizeof(Vector2));
    memcpy(&c, &color,  sizeof(Color));
    DrawRectanglePro(r, o, rotation, c);
}

void Iron_draw_rectangle_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
                                    struct Iron_Color top, struct Iron_Color bottom) {
    Color t, b;
    memcpy(&t, &top,    sizeof(Color));
    memcpy(&b, &bottom, sizeof(Color));
    DrawRectangleGradientV((int)x, (int)y, (int)w, (int)h, t, b);
}

void Iron_draw_rectangle_gradient_h(int32_t x, int32_t y, int32_t w, int32_t h,
                                    struct Iron_Color left, struct Iron_Color right) {
    Color l, r;
    memcpy(&l, &left,  sizeof(Color));
    memcpy(&r, &right, sizeof(Color));
    DrawRectangleGradientH((int)x, (int)y, (int)w, (int)h, l, r);
}

/* 4-color variant — quadruple memcpy. Argument order in raylib is
 * topLeft / bottomLeft / topRight / bottomRight (per raylib.h:104). */
void Iron_draw_rectangle_gradient_ex(struct Iron_Rectangle rec,
                                     struct Iron_Color tl, struct Iron_Color bl,
                                     struct Iron_Color tr, struct Iron_Color br) {
    Rectangle r;
    Color a, b, c, d;
    memcpy(&r, &rec, sizeof(Rectangle));
    memcpy(&a, &tl,  sizeof(Color));
    memcpy(&b, &bl,  sizeof(Color));
    memcpy(&c, &tr,  sizeof(Color));
    memcpy(&d, &br,  sizeof(Color));
    DrawRectangleGradientEx(r, a, b, c, d);
}

void Iron_draw_rectangle_lines(int32_t x, int32_t y, int32_t w, int32_t h, struct Iron_Color color) {
    Color rl;
    memcpy(&rl, &color, sizeof(Color));
    DrawRectangleLines((int)x, (int)y, (int)w, (int)h, rl);
}

void Iron_draw_rectangle_lines_ex(struct Iron_Rectangle rec, float thick, struct Iron_Color color) {
    Rectangle r;
    Color c;
    memcpy(&r, &rec,   sizeof(Rectangle));
    memcpy(&c, &color, sizeof(Color));
    DrawRectangleLinesEx(r, thick, c);
}

void Iron_draw_rectangle_rounded(struct Iron_Rectangle rec, float roundness, int32_t segments, struct Iron_Color color) {
    Rectangle r;
    Color c;
    memcpy(&r, &rec,   sizeof(Rectangle));
    memcpy(&c, &color, sizeof(Color));
    DrawRectangleRounded(r, roundness, (int)segments, c);
}

void Iron_draw_rectangle_rounded_lines(struct Iron_Rectangle rec, float roundness, int32_t segments, struct Iron_Color color) {
    Rectangle r;
    Color c;
    memcpy(&r, &rec,   sizeof(Rectangle));
    memcpy(&c, &color, sizeof(Color));
    DrawRectangleRoundedLines(r, roundness, (int)segments, c);
}

void Iron_draw_rectangle_rounded_lines_ex(struct Iron_Rectangle rec, float roundness, int32_t segments, float thick, struct Iron_Color color) {
    Rectangle r;
    Color c;
    memcpy(&r, &rec,   sizeof(Rectangle));
    memcpy(&c, &color, sizeof(Color));
    DrawRectangleRoundedLinesEx(r, roundness, (int)segments, thick, c);
}

/* Triangle primitives (DRAW2D-13) — 2 fixed-point variants */

void Iron_draw_triangle(struct Iron_Vector2 v1, struct Iron_Vector2 v2, struct Iron_Vector2 v3, struct Iron_Color color) {
    Vector2 a, b, c;
    Color col;
    memcpy(&a,   &v1,    sizeof(Vector2));
    memcpy(&b,   &v2,    sizeof(Vector2));
    memcpy(&c,   &v3,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    DrawTriangle(a, b, c, col);
}

void Iron_draw_triangle_lines(struct Iron_Vector2 v1, struct Iron_Vector2 v2, struct Iron_Vector2 v3, struct Iron_Color color) {
    Vector2 a, b, c;
    Color col;
    memcpy(&a,   &v1,    sizeof(Vector2));
    memcpy(&b,   &v2,    sizeof(Vector2));
    memcpy(&c,   &v3,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    DrawTriangleLines(a, b, c, col);
}

/* Polygon primitives (DRAW2D-14) */

void Iron_draw_poly(struct Iron_Vector2 center, int32_t sides, float r, float rotation, struct Iron_Color color) {
    Vector2 ct;
    Color c;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&c,  &color,  sizeof(Color));
    DrawPoly(ct, (int)sides, r, rotation, c);
}

void Iron_draw_poly_lines(struct Iron_Vector2 center, int32_t sides, float r, float rotation, struct Iron_Color color) {
    Vector2 ct;
    Color c;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&c,  &color,  sizeof(Color));
    DrawPolyLines(ct, (int)sides, r, rotation, c);
}

void Iron_draw_poly_lines_ex(struct Iron_Vector2 center, int32_t sides, float r, float rotation, float thick, struct Iron_Color color) {
    Vector2 ct;
    Color c;
    memcpy(&ct, &center, sizeof(Vector2));
    memcpy(&c,  &color,  sizeof(Color));
    DrawPolyLinesEx(ct, (int)sides, r, rotation, thick, c);
}

/* Triangle array variants (DRAW2D-13 — array ABI confirmed by Task 1 probe).
 * ironc passes Iron_List_Iron_Vector2 BY VALUE (items/count/capacity wrapper,
 * 24 bytes). We forward points.items as `const Vector2 *` — Phase 60-02's
 * _Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2)) pins the
 * element layout byte-identical, so the reinterpret is safe. The shim trusts
 * the caller-supplied `count` rather than points.count per the plan's
 * explicit-count convention (mirrors raylib's C signature). */
void Iron_draw_triangle_fan(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawTriangleFan((const Vector2 *)points.items, (int)count, c);
}

void Iron_draw_triangle_strip(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawTriangleStrip((const Vector2 *)points.items, (int)count, c);
}

/* Spline segment primitives (DRAW2D-15 — fixed-point-count variants) */

void Iron_draw_spline_segment_linear(struct Iron_Vector2 p1, struct Iron_Vector2 p2, float thick, struct Iron_Color color) {
    Vector2 a, b;
    Color c;
    memcpy(&a, &p1,    sizeof(Vector2));
    memcpy(&b, &p2,    sizeof(Vector2));
    memcpy(&c, &color, sizeof(Color));
    DrawSplineSegmentLinear(a, b, thick, c);
}

void Iron_draw_spline_segment_basis(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float thick, struct Iron_Color color) {
    Vector2 a, b, cc, d;
    Color col;
    memcpy(&a,   &p1,    sizeof(Vector2));
    memcpy(&b,   &p2,    sizeof(Vector2));
    memcpy(&cc,  &p3,    sizeof(Vector2));
    memcpy(&d,   &p4,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    DrawSplineSegmentBasis(a, b, cc, d, thick, col);
}

void Iron_draw_spline_segment_catmull_rom(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float thick, struct Iron_Color color) {
    Vector2 a, b, cc, d;
    Color col;
    memcpy(&a,   &p1,    sizeof(Vector2));
    memcpy(&b,   &p2,    sizeof(Vector2));
    memcpy(&cc,  &p3,    sizeof(Vector2));
    memcpy(&d,   &p4,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    DrawSplineSegmentCatmullRom(a, b, cc, d, thick, col);
}

void Iron_draw_spline_segment_bezier_quadratic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 p3, float thick, struct Iron_Color color) {
    Vector2 a, b, cc;
    Color col;
    memcpy(&a,   &p1,    sizeof(Vector2));
    memcpy(&b,   &c2,    sizeof(Vector2));
    memcpy(&cc,  &p3,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    DrawSplineSegmentBezierQuadratic(a, b, cc, thick, col);
}

void Iron_draw_spline_segment_bezier_cubic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 c3, struct Iron_Vector2 p4, float thick, struct Iron_Color color) {
    Vector2 a, b, cc, d;
    Color col;
    memcpy(&a,   &p1,    sizeof(Vector2));
    memcpy(&b,   &c2,    sizeof(Vector2));
    memcpy(&cc,  &c3,    sizeof(Vector2));
    memcpy(&d,   &p4,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    DrawSplineSegmentBezierCubic(a, b, cc, d, thick, col);
}

/* Spline evaluators (DRAW2D-16) — Vector2 RETURN via memcpy-out.
 * Pattern matches iron_raylib.c:137-142 (Iron_window_get_window_position).
 * Iron-side method name normalizes raylib's `BezierQuad` to
 * `bezier_quadratic` for symmetry with the draw side. */

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

struct Iron_Vector2 Iron_draw_get_spline_point_basis(
        struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float t) {
    Vector2 a, b, c, d;
    memcpy(&a, &p1, sizeof(Vector2));
    memcpy(&b, &p2, sizeof(Vector2));
    memcpy(&c, &p3, sizeof(Vector2));
    memcpy(&d, &p4, sizeof(Vector2));
    Vector2 rl = GetSplinePointBasis(a, b, c, d, t);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_draw_get_spline_point_catmull_rom(
        struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float t) {
    Vector2 a, b, c, d;
    memcpy(&a, &p1, sizeof(Vector2));
    memcpy(&b, &p2, sizeof(Vector2));
    memcpy(&c, &p3, sizeof(Vector2));
    memcpy(&d, &p4, sizeof(Vector2));
    Vector2 rl = GetSplinePointCatmullRom(a, b, c, d, t);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_draw_get_spline_point_bezier_quadratic(
        struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 p3, float t) {
    Vector2 a, b, c;
    memcpy(&a, &p1, sizeof(Vector2));
    memcpy(&b, &c2, sizeof(Vector2));
    memcpy(&c, &p3, sizeof(Vector2));
    /* raylib's C symbol is GetSplinePointBezierQuad (short name); Iron
     * normalizes the method to bezier_quadratic for symmetry. */
    Vector2 rl = GetSplinePointBezierQuad(a, b, c, t);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_draw_get_spline_point_bezier_cubic(
        struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 c3, struct Iron_Vector2 p4, float t) {
    Vector2 a, b, c, d;
    memcpy(&a, &p1, sizeof(Vector2));
    memcpy(&b, &c2, sizeof(Vector2));
    memcpy(&c, &c3, sizeof(Vector2));
    memcpy(&d, &p4, sizeof(Vector2));
    Vector2 rl = GetSplinePointBezierCubic(a, b, c, d, t);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}

/* Deferred / whole-spline array shims (DRAW2D-08 last, DRAW2D-15 whole-spline)
 * — ARRAY ABI confirmed by Plan 63-03 Task 1 probe. ironc passes
 * Iron_List_Iron_Vector2 BY VALUE (items/count/capacity wrapper, 24
 * bytes). Forward points.items as `const Vector2 *`; the
 * _Static_assert grid in Phase 60-02 makes the reinterpret safe. The
 * shim trusts the caller-supplied `count` rather than points.count
 * per the plan's explicit-count convention (mirrors raylib's C
 * signature). */

void Iron_draw_line_strip(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawLineStrip((const Vector2 *)points.items, (int)count, c);
}

void Iron_draw_spline_linear(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawSplineLinear((const Vector2 *)points.items, (int)count, thick, c);
}

void Iron_draw_spline_basis(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawSplineBasis((const Vector2 *)points.items, (int)count, thick, c);
}

void Iron_draw_spline_catmull_rom(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawSplineCatmullRom((const Vector2 *)points.items, (int)count, thick, c);
}

void Iron_draw_spline_bezier_quadratic(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawSplineBezierQuadratic((const Vector2 *)points.items, (int)count, thick, c);
}

void Iron_draw_spline_bezier_cubic(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawSplineBezierCubic((const Vector2 *)points.items, (int)count, thick, c);
}

/* ── Collision (Phase 64) ─────────────────────────────────────────── */
/* 2D collision (COLL-01) — 11 functions. */

bool Iron_rectangle_collides(struct Iron_Rectangle self, struct Iron_Rectangle other) {
    Rectangle a, b;
    memcpy(&a, &self,  sizeof(Rectangle));
    memcpy(&b, &other, sizeof(Rectangle));
    return CheckCollisionRecs(a, b);
}

struct Iron_Rectangle Iron_rectangle_intersection(struct Iron_Rectangle self, struct Iron_Rectangle other) {
    Rectangle a, b;
    memcpy(&a, &self,  sizeof(Rectangle));
    memcpy(&b, &other, sizeof(Rectangle));
    Rectangle rl = GetCollisionRec(a, b);
    struct Iron_Rectangle out;
    memcpy(&out, &rl, sizeof(Rectangle));
    return out;
}

bool Iron_rectangle_contains_point(struct Iron_Rectangle self, struct Iron_Vector2 point) {
    Rectangle r;
    Vector2 p;
    memcpy(&r, &self,  sizeof(Rectangle));
    memcpy(&p, &point, sizeof(Vector2));
    return CheckCollisionPointRec(p, r);
}

bool Iron_rectangle_collides_circle(struct Iron_Rectangle self, struct Iron_Vector2 center, float radius) {
    Rectangle r;
    Vector2 c;
    memcpy(&r, &self,   sizeof(Rectangle));
    memcpy(&c, &center, sizeof(Vector2));
    return CheckCollisionCircleRec(c, radius, r);
}

bool Iron_vector2_inside_triangle(struct Iron_Vector2 self, struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3) {
    Vector2 p, a, b, c;
    memcpy(&p, &self, sizeof(Vector2));
    memcpy(&a, &p1,   sizeof(Vector2));
    memcpy(&b, &p2,   sizeof(Vector2));
    memcpy(&c, &p3,   sizeof(Vector2));
    return CheckCollisionPointTriangle(p, a, b, c);
}

bool Iron_vector2_inside_polygon(struct Iron_Vector2 self, Iron_List_Iron_Vector2 points) {
    Vector2 p;
    memcpy(&p, &self, sizeof(Vector2));
    return CheckCollisionPointPoly(p, (const Vector2 *)points.items, (int)points.count);
}

bool Iron_vector2_on_line(struct Iron_Vector2 self, struct Iron_Vector2 p1, struct Iron_Vector2 p2, int32_t threshold) {
    Vector2 p, a, b;
    memcpy(&p, &self, sizeof(Vector2));
    memcpy(&a, &p1,   sizeof(Vector2));
    memcpy(&b, &p2,   sizeof(Vector2));
    return CheckCollisionPointLine(p, a, b, (int)threshold);
}

bool Iron_collision_circles(struct Iron_Vector2 c1, float r1, struct Iron_Vector2 c2, float r2) {
    Vector2 a, b;
    memcpy(&a, &c1, sizeof(Vector2));
    memcpy(&b, &c2, sizeof(Vector2));
    return CheckCollisionCircles(a, r1, b, r2);
}

bool Iron_collision_circle_line(struct Iron_Vector2 center, float radius, struct Iron_Vector2 p1, struct Iron_Vector2 p2) {
    Vector2 c, a, b;
    memcpy(&c, &center, sizeof(Vector2));
    memcpy(&a, &p1,     sizeof(Vector2));
    memcpy(&b, &p2,     sizeof(Vector2));
    return CheckCollisionCircleLine(c, radius, a, b);
}

bool Iron_collision_point_circle(struct Iron_Vector2 point, struct Iron_Vector2 center, float radius) {
    Vector2 p, c;
    memcpy(&p, &point,  sizeof(Vector2));
    memcpy(&c, &center, sizeof(Vector2));
    return CheckCollisionPointCircle(p, c, radius);
}

Iron_Tuple_Bool_Vector2 Iron_collision_lines(
        struct Iron_Vector2 start_a, struct Iron_Vector2 end_a,
        struct Iron_Vector2 start_b, struct Iron_Vector2 end_b) {
    Vector2 s1, e1, s2, e2, pt;
    memcpy(&s1, &start_a, sizeof(Vector2));
    memcpy(&e1, &end_a,   sizeof(Vector2));
    memcpy(&s2, &start_b, sizeof(Vector2));
    memcpy(&e2, &end_b,   sizeof(Vector2));
    bool hit = CheckCollisionLines(s1, e1, s2, e2, &pt);
    Iron_Tuple_Bool_Vector2 out;
    out.v0 = hit;
    memcpy(&out.v1, &pt, sizeof(Vector2));
    return out;
}

/* 3D collision (COLL-02) — 8 functions. */

bool Iron_boundingbox_collides(struct Iron_BoundingBox self, struct Iron_BoundingBox other) {
    BoundingBox a, b;
    memcpy(&a, &self,  sizeof(BoundingBox));
    memcpy(&b, &other, sizeof(BoundingBox));
    return CheckCollisionBoxes(a, b);
}

bool Iron_boundingbox_collides_sphere(struct Iron_BoundingBox self, struct Iron_Vector3 center, float radius) {
    BoundingBox b;
    Vector3 c;
    memcpy(&b, &self,   sizeof(BoundingBox));
    memcpy(&c, &center, sizeof(Vector3));
    return CheckCollisionBoxSphere(b, c, radius);
}

struct Iron_RayCollision Iron_ray_hit_sphere(struct Iron_Ray self, struct Iron_Vector3 center, float radius) {
    Ray r;
    Vector3 c;
    memcpy(&r, &self,   sizeof(Ray));
    memcpy(&c, &center, sizeof(Vector3));
    RayCollision rl = GetRayCollisionSphere(r, c, radius);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));  /* 32 bytes */
    return out;
}

struct Iron_RayCollision Iron_ray_hit_box(struct Iron_Ray self, struct Iron_BoundingBox box) {
    Ray r;
    BoundingBox bb;
    memcpy(&r,  &self, sizeof(Ray));
    memcpy(&bb, &box,  sizeof(BoundingBox));
    RayCollision rl = GetRayCollisionBox(r, bb);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}

struct Iron_RayCollision Iron_ray_hit_mesh(struct Iron_Ray self, struct Iron_Mesh mesh, struct Iron_Matrix transform) {
    Ray r;
    Mesh m;
    Matrix tx;
    memcpy(&r, &self, sizeof(Ray));
    memcpy(&m, &mesh, sizeof(Mesh));      /* 120 bytes */
    memcpy(&tx, &transform, sizeof(Matrix));    /* 64 bytes */
    RayCollision rl = GetRayCollisionMesh(r, m, tx);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}

struct Iron_RayCollision Iron_ray_hit_triangle(struct Iron_Ray self, struct Iron_Vector3 p1, struct Iron_Vector3 p2, struct Iron_Vector3 p3) {
    Ray r;
    Vector3 a, b, c;
    memcpy(&r, &self, sizeof(Ray));
    memcpy(&a, &p1,   sizeof(Vector3));
    memcpy(&b, &p2,   sizeof(Vector3));
    memcpy(&c, &p3,   sizeof(Vector3));
    RayCollision rl = GetRayCollisionTriangle(r, a, b, c);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}

struct Iron_RayCollision Iron_ray_hit_quad(struct Iron_Ray self, struct Iron_Vector3 p1, struct Iron_Vector3 p2, struct Iron_Vector3 p3, struct Iron_Vector3 p4) {
    Ray r;
    Vector3 a, b, c, d;
    memcpy(&r, &self, sizeof(Ray));
    memcpy(&a, &p1,   sizeof(Vector3));
    memcpy(&b, &p2,   sizeof(Vector3));
    memcpy(&c, &p3,   sizeof(Vector3));
    memcpy(&d, &p4,   sizeof(Vector3));
    RayCollision rl = GetRayCollisionQuad(r, a, b, c, d);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}

bool Iron_collision_spheres(struct Iron_Vector3 c1, float r1, struct Iron_Vector3 c2, float r2) {
    Vector3 a, b;
    memcpy(&a, &c1, sizeof(Vector3));
    memcpy(&b, &c2, sizeof(Vector3));
    return CheckCollisionSpheres(a, r1, b, r2);
}

/* ── raymath (Phase 65) ───────────────────────────────────────────── */

/* RMath namespace — scalar helpers (MATH-01, raymath.h lines 178-228).
 *
 * Each shim forwards to raymath's RMAPI function. With
 * RAYMATH_STATIC_INLINE defined above, the raymath symbols are
 * `static inline` — each TU owns its own copy; linker deduplicates.
 * FloatEquals returns int (1/0); we coerce to stdbool.h bool so the
 * Iron-side signature is a clean Bool.
 *
 * Symbol prefix is Iron_rmath_ (NOT Iron_math_) to avoid colliding
 * with iron_math.h / math.iron's existing Math namespace surface.
 * Iron stubs use `object RMath`. */

float Iron_rmath_clamp(float value, float min, float max) {
    return Clamp(value, min, max);
}

float Iron_rmath_lerp(float start, float end, float amount) {
    return Lerp(start, end, amount);
}

float Iron_rmath_normalize(float value, float start, float end) {
    return Normalize(value, start, end);
}

float Iron_rmath_wrap(float value, float min, float max) {
    return Wrap(value, min, max);
}

float Iron_rmath_remap(float value, float in_start, float in_end, float out_start, float out_end) {
    return Remap(value, in_start, in_end, out_start, out_end);
}

bool Iron_rmath_float_equals(float x, float y) {
    return (bool)(FloatEquals(x, y) != 0);
}

/* Vector2 methods — 30 functions (MATH-02, raymath.h lines 236-620).
 *
 * Each shim memcpys the Iron_Vector2 argument into a stack-local
 * raymath `Vector2`, invokes the raymath function, then memcpys the
 * result (when it's a Vector2) back into an Iron_Vector2 return
 * value. Vector2 is 8 bytes (two contiguous floats) — the Phase 60-02
 * _Static_assert grid pins this layout permanently.
 *
 * Vector2Equals returns int (1/0); coerced to stdbool.h bool.
 * Vector2Transform takes a raymath Matrix (64 B) as second arg —
 * passed by value; two separate memcpys marshal the Vector2 and
 * Matrix independently. */

struct Iron_Vector2 Iron_vector2_zero(void) {
    Vector2 r = Vector2Zero();
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_one(void) {
    Vector2 r = Vector2One();
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_add(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Add(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_add_value(struct Iron_Vector2 self, float add) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2AddValue(v, add);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_subtract(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Subtract(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_subtract_value(struct Iron_Vector2 self, float sub) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2SubtractValue(v, sub);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

float Iron_vector2_length(struct Iron_Vector2 self) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    return Vector2Length(v);
}

float Iron_vector2_length_sqr(struct Iron_Vector2 self) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    return Vector2LengthSqr(v);
}

float Iron_vector2_dot_product(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    return Vector2DotProduct(a, b);
}

float Iron_vector2_distance(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    return Vector2Distance(a, b);
}

float Iron_vector2_distance_sqr(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    return Vector2DistanceSqr(a, b);
}

float Iron_vector2_angle(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    return Vector2Angle(a, b);
}

float Iron_vector2_line_angle(struct Iron_Vector2 start, struct Iron_Vector2 end) {
    Vector2 a, b;
    memcpy(&a, &start, sizeof(Vector2));
    memcpy(&b, &end,   sizeof(Vector2));
    return Vector2LineAngle(a, b);
}

struct Iron_Vector2 Iron_vector2_scale(struct Iron_Vector2 self, float scale) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2Scale(v, scale);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_multiply(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Multiply(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_negate(struct Iron_Vector2 self) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2Negate(v);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_divide(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Divide(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_normalize(struct Iron_Vector2 self) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2Normalize(v);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_transform(struct Iron_Vector2 self, struct Iron_Matrix mat) {
    Vector2 v;
    Matrix  m;
    memcpy(&v, &self, sizeof(Vector2));
    memcpy(&m, &mat,  sizeof(Matrix));
    Vector2 r = Vector2Transform(v, m);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_lerp(struct Iron_Vector2 self, struct Iron_Vector2 other, float amount) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Lerp(a, b, amount);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_reflect(struct Iron_Vector2 self, struct Iron_Vector2 normal) {
    Vector2 v, n;
    memcpy(&v, &self,   sizeof(Vector2));
    memcpy(&n, &normal, sizeof(Vector2));
    Vector2 r = Vector2Reflect(v, n);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_min(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Min(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_max(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    Vector2 r = Vector2Max(a, b);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_rotate(struct Iron_Vector2 self, float angle) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2Rotate(v, angle);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_move_towards(struct Iron_Vector2 self, struct Iron_Vector2 target, float max_distance) {
    Vector2 v, t;
    memcpy(&v, &self,   sizeof(Vector2));
    memcpy(&t, &target, sizeof(Vector2));
    Vector2 r = Vector2MoveTowards(v, t, max_distance);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_invert(struct Iron_Vector2 self) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2Invert(v);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_clamp(struct Iron_Vector2 self, struct Iron_Vector2 min, struct Iron_Vector2 max) {
    Vector2 v, lo, hi;
    memcpy(&v,  &self, sizeof(Vector2));
    memcpy(&lo, &min,  sizeof(Vector2));
    memcpy(&hi, &max,  sizeof(Vector2));
    Vector2 r = Vector2Clamp(v, lo, hi);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_vector2_clamp_value(struct Iron_Vector2 self, float min, float max) {
    Vector2 v;
    memcpy(&v, &self, sizeof(Vector2));
    Vector2 r = Vector2ClampValue(v, min, max);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

bool Iron_vector2_equals(struct Iron_Vector2 self, struct Iron_Vector2 other) {
    Vector2 a, b;
    memcpy(&a, &self,  sizeof(Vector2));
    memcpy(&b, &other, sizeof(Vector2));
    return (bool)(Vector2Equals(a, b) != 0);
}

struct Iron_Vector2 Iron_vector2_refract(struct Iron_Vector2 self, struct Iron_Vector2 n, float r) {
    Vector2 v, nrm;
    memcpy(&v,   &self, sizeof(Vector2));
    memcpy(&nrm, &n,    sizeof(Vector2));
    Vector2 out_v = Vector2Refract(v, nrm, r);
    struct Iron_Vector2 out;
    memcpy(&out, &out_v, sizeof(Vector2));
    return out;
}

/* Vector3 methods — 38 functions (MATH-03, raymath.h lines 621-1140).
 * Each shim memcpys struct-by-value Iron_Vector3 (12 B) into a raymath
 * Vector3, invokes the raymath function, then memcpys the result back
 * into Iron_Vector3 for return. Cross-type args (Quaternion 16 B,
 * Matrix 64 B) use one independent memcpy per struct-kind arg per the
 * Iron_ray_hit_mesh precedent (Phase 64-02). Matrix-64 B pass-by-value
 * is exactly at the -Wlarge-by-value-copy threshold (fires at strictly
 * > 64), so unproject/transform pass clean under -Wall -Wextra.
 *
 * 2 functions deferred to later plans:
 *   - Iron_vector3_to_float_v → Plan 65-03 (Float3 return type)
 *   - Iron_vector3_ortho_normalize → Plan 65-04 (tuple return) */

struct Iron_Vector3 Iron_vector3_zero(void) {
    Vector3 r = Vector3Zero();
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_one(void) {
    Vector3 r = Vector3One();
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_add(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Add(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_add_value(struct Iron_Vector3 self, float add) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3AddValue(v, add);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_subtract(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Subtract(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_subtract_value(struct Iron_Vector3 self, float sub) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3SubtractValue(v, sub);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_scale(struct Iron_Vector3 self, float scalar) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3Scale(v, scalar);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_multiply(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Multiply(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_cross_product(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3CrossProduct(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_perpendicular(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3Perpendicular(v);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

float Iron_vector3_length(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    return Vector3Length(v);
}

float Iron_vector3_length_sqr(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    return Vector3LengthSqr(v);
}

float Iron_vector3_dot_product(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    return Vector3DotProduct(a, b);
}

float Iron_vector3_distance(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    return Vector3Distance(a, b);
}

float Iron_vector3_distance_sqr(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    return Vector3DistanceSqr(a, b);
}

float Iron_vector3_angle(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    return Vector3Angle(a, b);
}

struct Iron_Vector3 Iron_vector3_negate(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3Negate(v);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_divide(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Divide(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_normalize(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3Normalize(v);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_project(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Project(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_reject(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Reject(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_transform(struct Iron_Vector3 self, struct Iron_Matrix mat) {
    Vector3 v;
    Matrix  m;
    memcpy(&v, &self, sizeof(Vector3));
    memcpy(&m, &mat,  sizeof(Matrix));
    Vector3 r = Vector3Transform(v, m);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_rotate_by_quaternion(struct Iron_Vector3 self, struct Iron_Quaternion q) {
    Vector3    v;
    Quaternion quat;
    memcpy(&v,    &self, sizeof(Vector3));
    memcpy(&quat, &q,    sizeof(Quaternion));
    Vector3 r = Vector3RotateByQuaternion(v, quat);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_rotate_by_axis_angle(struct Iron_Vector3 self, struct Iron_Vector3 axis, float angle) {
    Vector3 v, ax;
    memcpy(&v,  &self, sizeof(Vector3));
    memcpy(&ax, &axis, sizeof(Vector3));
    Vector3 r = Vector3RotateByAxisAngle(v, ax, angle);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_move_towards(struct Iron_Vector3 self, struct Iron_Vector3 target, float max_distance) {
    Vector3 v, t;
    memcpy(&v, &self,   sizeof(Vector3));
    memcpy(&t, &target, sizeof(Vector3));
    Vector3 r = Vector3MoveTowards(v, t, max_distance);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_lerp(struct Iron_Vector3 self, struct Iron_Vector3 other, float amount) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Lerp(a, b, amount);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_cubic_hermite(struct Iron_Vector3 self, struct Iron_Vector3 tangent1, struct Iron_Vector3 v2, struct Iron_Vector3 tangent2, float amount) {
    Vector3 v1, t1, vv2, t2;
    memcpy(&v1,  &self,     sizeof(Vector3));
    memcpy(&t1,  &tangent1, sizeof(Vector3));
    memcpy(&vv2, &v2,       sizeof(Vector3));
    memcpy(&t2,  &tangent2, sizeof(Vector3));
    Vector3 r = Vector3CubicHermite(v1, t1, vv2, t2, amount);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_reflect(struct Iron_Vector3 self, struct Iron_Vector3 normal) {
    Vector3 v, n;
    memcpy(&v, &self,   sizeof(Vector3));
    memcpy(&n, &normal, sizeof(Vector3));
    Vector3 r = Vector3Reflect(v, n);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_min(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Min(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_max(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    Vector3 r = Vector3Max(a, b);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_barycenter(struct Iron_Vector3 self, struct Iron_Vector3 a, struct Iron_Vector3 b, struct Iron_Vector3 c) {
    Vector3 pp, aa, bb, cc;
    memcpy(&pp, &self, sizeof(Vector3));
    memcpy(&aa, &a,    sizeof(Vector3));
    memcpy(&bb, &b,    sizeof(Vector3));
    memcpy(&cc, &c,    sizeof(Vector3));
    Vector3 r = Vector3Barycenter(pp, aa, bb, cc);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_unproject(struct Iron_Vector3 self, struct Iron_Matrix projection, struct Iron_Matrix view) {
    Vector3 src;
    Matrix  proj, vw;
    memcpy(&src,  &self,       sizeof(Vector3));
    memcpy(&proj, &projection, sizeof(Matrix));
    memcpy(&vw,   &view,       sizeof(Matrix));
    Vector3 r = Vector3Unproject(src, proj, vw);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_invert(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3Invert(v);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_clamp(struct Iron_Vector3 self, struct Iron_Vector3 min, struct Iron_Vector3 max) {
    Vector3 v, mn, mx;
    memcpy(&v,  &self, sizeof(Vector3));
    memcpy(&mn, &min,  sizeof(Vector3));
    memcpy(&mx, &max,  sizeof(Vector3));
    Vector3 r = Vector3Clamp(v, mn, mx);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Vector3 Iron_vector3_clamp_value(struct Iron_Vector3 self, float min, float max) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    Vector3 r = Vector3ClampValue(v, min, max);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

bool Iron_vector3_equals(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 a, b;
    memcpy(&a, &self,  sizeof(Vector3));
    memcpy(&b, &other, sizeof(Vector3));
    return (bool)(Vector3Equals(a, b) != 0);
}

struct Iron_Vector3 Iron_vector3_refract(struct Iron_Vector3 self, struct Iron_Vector3 n, float r) {
    Vector3 v, nrm;
    memcpy(&v,   &self, sizeof(Vector3));
    memcpy(&nrm, &n,    sizeof(Vector3));
    Vector3 out_v = Vector3Refract(v, nrm, r);
    struct Iron_Vector3 out;
    memcpy(&out, &out_v, sizeof(Vector3));
    return out;
}

/* Vector3.to_float_v (Plan 65-03, MATH-03 carried) — first Float3
 * (12 B) struct-by-value RETURN shim. Byte-copy from raymath's
 * `float3 { float v[3]; }` into `struct Iron_Float3 { float x, y, z; }` —
 * layout-identical by C contiguous-float guarantee + per-field
 * _Static_assert offsetof pins (iron_raylib_layout.c Phase 65 Plan 03). */
struct Iron_Float3 Iron_vector3_to_float_v(struct Iron_Vector3 self) {
    Vector3 v;
    memcpy(&v, &self, sizeof(Vector3));
    float3 r = Vector3ToFloatV(v);
    struct Iron_Float3 out;
    memcpy(&out, &r, sizeof(float3));
    return out;
}

/* ── Vector4 methods (Phase 65 Plan 03, MATH-04) ───────────────────
 * 22 shims mirroring raymath Vector4* (lines 1232-1440). 16 B struct
 * pass-by-value in/out reuses the Rectangle template from Phase 63.
 * Vector4Equals returns int → explicit (bool)(... != 0) coercion. */

struct Iron_Vector4 Iron_vector4_zero(void) {
    Vector4 r = Vector4Zero();
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_one(void) {
    Vector4 r = Vector4One();
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_add(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    Vector4 r = Vector4Add(a, b);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_add_value(struct Iron_Vector4 self, float add) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    Vector4 r = Vector4AddValue(v, add);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_subtract(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    Vector4 r = Vector4Subtract(a, b);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_subtract_value(struct Iron_Vector4 self, float sub) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    Vector4 r = Vector4SubtractValue(v, sub);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

float Iron_vector4_length(struct Iron_Vector4 self) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    return Vector4Length(v);
}

float Iron_vector4_length_sqr(struct Iron_Vector4 self) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    return Vector4LengthSqr(v);
}

float Iron_vector4_dot_product(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    return Vector4DotProduct(a, b);
}

float Iron_vector4_distance(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    return Vector4Distance(a, b);
}

float Iron_vector4_distance_sqr(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    return Vector4DistanceSqr(a, b);
}

struct Iron_Vector4 Iron_vector4_scale(struct Iron_Vector4 self, float scale) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    Vector4 r = Vector4Scale(v, scale);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_multiply(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    Vector4 r = Vector4Multiply(a, b);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_negate(struct Iron_Vector4 self) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    Vector4 r = Vector4Negate(v);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_divide(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    Vector4 r = Vector4Divide(a, b);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_normalize(struct Iron_Vector4 self) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    Vector4 r = Vector4Normalize(v);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_min(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    Vector4 r = Vector4Min(a, b);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_max(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    Vector4 r = Vector4Max(a, b);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_lerp(struct Iron_Vector4 self, struct Iron_Vector4 other, float amount) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    Vector4 r = Vector4Lerp(a, b, amount);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_move_towards(struct Iron_Vector4 self, struct Iron_Vector4 target, float max_distance) {
    Vector4 v, t;
    memcpy(&v, &self,   sizeof(Vector4));
    memcpy(&t, &target, sizeof(Vector4));
    Vector4 r = Vector4MoveTowards(v, t, max_distance);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

struct Iron_Vector4 Iron_vector4_invert(struct Iron_Vector4 self) {
    Vector4 v;
    memcpy(&v, &self, sizeof(Vector4));
    Vector4 r = Vector4Invert(v);
    struct Iron_Vector4 out;
    memcpy(&out, &r, sizeof(Vector4));
    return out;
}

bool Iron_vector4_equals(struct Iron_Vector4 self, struct Iron_Vector4 other) {
    Vector4 a, b;
    memcpy(&a, &self,  sizeof(Vector4));
    memcpy(&b, &other, sizeof(Vector4));
    return (bool)(Vector4Equals(a, b) != 0);
}

/* ── Matrix methods (Phase 65 Plan 03, MATH-05) ────────────────────
 * 21 of 22 shims. Decompose deferred to Plan 65-04 (3-tuple out-param).
 * First 64 B struct-by-value RETURN surface in the codebase — validated
 * zero-warning under -Wall -Wextra (clang threshold fires at strictly
 * >64, not >=64). Frustum/Perspective/Ortho widen Iron Float32 to
 * raymath double via (double) casts. */

float Iron_matrix_determinant(struct Iron_Matrix self) {
    Matrix m;
    memcpy(&m, &self, sizeof(Matrix));
    return MatrixDeterminant(m);
}

float Iron_matrix_trace(struct Iron_Matrix self) {
    Matrix m;
    memcpy(&m, &self, sizeof(Matrix));
    return MatrixTrace(m);
}

struct Iron_Matrix Iron_matrix_transpose(struct Iron_Matrix self) {
    Matrix m;
    memcpy(&m, &self, sizeof(Matrix));
    Matrix r = MatrixTranspose(m);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_invert(struct Iron_Matrix self) {
    Matrix m;
    memcpy(&m, &self, sizeof(Matrix));
    Matrix r = MatrixInvert(m);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_identity(void) {
    Matrix r = MatrixIdentity();
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_add(struct Iron_Matrix self, struct Iron_Matrix other) {
    Matrix a, b;
    memcpy(&a, &self,  sizeof(Matrix));
    memcpy(&b, &other, sizeof(Matrix));
    Matrix r = MatrixAdd(a, b);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_subtract(struct Iron_Matrix self, struct Iron_Matrix other) {
    Matrix a, b;
    memcpy(&a, &self,  sizeof(Matrix));
    memcpy(&b, &other, sizeof(Matrix));
    Matrix r = MatrixSubtract(a, b);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_multiply(struct Iron_Matrix self, struct Iron_Matrix other) {
    Matrix a, b;
    memcpy(&a, &self,  sizeof(Matrix));
    memcpy(&b, &other, sizeof(Matrix));
    Matrix r = MatrixMultiply(a, b);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_translate(float x, float y, float z) {
    Matrix r = MatrixTranslate(x, y, z);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_rotate(struct Iron_Vector3 axis, float angle) {
    Vector3 a;
    memcpy(&a, &axis, sizeof(Vector3));
    Matrix r = MatrixRotate(a, angle);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_rotate_x(float angle) {
    Matrix r = MatrixRotateX(angle);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_rotate_y(float angle) {
    Matrix r = MatrixRotateY(angle);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_rotate_z(float angle) {
    Matrix r = MatrixRotateZ(angle);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_rotate_xyz(struct Iron_Vector3 angle) {
    Vector3 a;
    memcpy(&a, &angle, sizeof(Vector3));
    Matrix r = MatrixRotateXYZ(a);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_rotate_zyx(struct Iron_Vector3 angle) {
    Vector3 a;
    memcpy(&a, &angle, sizeof(Vector3));
    Matrix r = MatrixRotateZYX(a);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_scale(float x, float y, float z) {
    Matrix r = MatrixScale(x, y, z);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_frustum(float left, float right, float bottom, float top, float near_plane, float far_plane) {
    /* raymath widens to double; Iron-side is Float32. */
    Matrix r = MatrixFrustum((double)left, (double)right, (double)bottom, (double)top, (double)near_plane, (double)far_plane);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_perspective(float fovy, float aspect, float near_plane, float far_plane) {
    Matrix r = MatrixPerspective((double)fovy, (double)aspect, (double)near_plane, (double)far_plane);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_ortho(float left, float right, float bottom, float top, float near_plane, float far_plane) {
    Matrix r = MatrixOrtho((double)left, (double)right, (double)bottom, (double)top, (double)near_plane, (double)far_plane);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Matrix Iron_matrix_look_at(struct Iron_Vector3 eye, struct Iron_Vector3 target, struct Iron_Vector3 up) {
    Vector3 e, t, u;
    memcpy(&e, &eye,    sizeof(Vector3));
    memcpy(&t, &target, sizeof(Vector3));
    memcpy(&u, &up,     sizeof(Vector3));
    Matrix r = MatrixLookAt(e, t, u);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Float16 Iron_matrix_to_float_v(struct Iron_Matrix self) {
    Matrix m;
    memcpy(&m, &self, sizeof(Matrix));
    float16 r = MatrixToFloatV(m);
    struct Iron_Float16 out;
    memcpy(&out, &r, sizeof(float16));
    return out;
}

/* ── Phase 65 Plan 04 — out-param tuple shims ──────────────────────
 * Three shims packaging raymath out-parameter results into tuple
 * returns: MatrixDecompose (3-tuple), Vector3OrthoNormalize (2-tuple
 * mutating both args), QuaternionToAxisAngle (2-tuple). Tuple
 * typedefs (Iron_Tuple_Vector3_Quaternion_Vector3, _Vector3_Vector3,
 * _Vector3_Float32) are guarded in iron_raylib.h and auto-emitted
 * by ironc into consumer C TUs via emit_helpers.c:260-294.
 *
 * MatrixDecompose: first 3-tuple return surface in the codebase.
 * Task 1 probe confirmed the auto-emit path is arity-agnostic. */
Iron_Tuple_Vector3_Quaternion_Vector3 Iron_matrix_decompose(struct Iron_Matrix self) {
    Matrix m;
    memcpy(&m, &self, sizeof(Matrix));
    Vector3    translation, scale;
    Quaternion rotation;
    MatrixDecompose(m, &translation, &rotation, &scale);
    Iron_Tuple_Vector3_Quaternion_Vector3 out;
    memcpy(&out.v0, &translation, sizeof(Vector3));
    memcpy(&out.v1, &rotation,    sizeof(Quaternion));
    memcpy(&out.v2, &scale,       sizeof(Vector3));
    return out;
}

Iron_Tuple_Vector3_Vector3 Iron_vector3_ortho_normalize(struct Iron_Vector3 self, struct Iron_Vector3 other) {
    Vector3 c1, c2;
    memcpy(&c1, &self,  sizeof(Vector3));
    memcpy(&c2, &other, sizeof(Vector3));
    Vector3OrthoNormalize(&c1, &c2);    /* mutates both locals */
    Iron_Tuple_Vector3_Vector3 out;
    memcpy(&out.v0, &c1, sizeof(Vector3));
    memcpy(&out.v1, &c2, sizeof(Vector3));
    return out;
}

/* ── Phase 65 Plan 04 — Quaternion methods (MATH-06, 24 shims) ─────
 * 24 of 24 raymath Quaternion RMAPI functions. Same 16 B struct-by-
 * value in/out template as Vector4. Cross-type shims (from_matrix /
 * to_matrix / transform / from_vector3_to_vector3 / to_euler /
 * from_axis_angle) use one memcpy per struct-kind arg per Phase 64
 * Iron_ray_hit_mesh precedent. QuaternionEquals returns int → shim
 * coerces to bool. */
struct Iron_Quaternion Iron_quaternion_add(struct Iron_Quaternion self, struct Iron_Quaternion other) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    Quaternion r = QuaternionAdd(a, b);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_add_value(struct Iron_Quaternion self, float add) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Quaternion r = QuaternionAddValue(q, add);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_subtract(struct Iron_Quaternion self, struct Iron_Quaternion other) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    Quaternion r = QuaternionSubtract(a, b);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_subtract_value(struct Iron_Quaternion self, float sub) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Quaternion r = QuaternionSubtractValue(q, sub);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_identity(void) {
    Quaternion r = QuaternionIdentity();
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

float Iron_quaternion_length(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    return QuaternionLength(q);
}

struct Iron_Quaternion Iron_quaternion_normalize(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Quaternion r = QuaternionNormalize(q);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_invert(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Quaternion r = QuaternionInvert(q);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_multiply(struct Iron_Quaternion self, struct Iron_Quaternion other) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    Quaternion r = QuaternionMultiply(a, b);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_scale(struct Iron_Quaternion self, float mul) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Quaternion r = QuaternionScale(q, mul);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_divide(struct Iron_Quaternion self, struct Iron_Quaternion other) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    Quaternion r = QuaternionDivide(a, b);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_lerp(struct Iron_Quaternion self, struct Iron_Quaternion other, float amount) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    Quaternion r = QuaternionLerp(a, b, amount);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_nlerp(struct Iron_Quaternion self, struct Iron_Quaternion other, float amount) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    Quaternion r = QuaternionNlerp(a, b, amount);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_slerp(struct Iron_Quaternion self, struct Iron_Quaternion other, float amount) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    Quaternion r = QuaternionSlerp(a, b, amount);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_cubic_hermite_spline(struct Iron_Quaternion self, struct Iron_Quaternion out_tangent1, struct Iron_Quaternion q2, struct Iron_Quaternion in_tangent2, float t) {
    Quaternion a, ot, b, it;
    memcpy(&a,  &self,         sizeof(Quaternion));
    memcpy(&ot, &out_tangent1, sizeof(Quaternion));
    memcpy(&b,  &q2,           sizeof(Quaternion));
    memcpy(&it, &in_tangent2,  sizeof(Quaternion));
    Quaternion r = QuaternionCubicHermiteSpline(a, ot, b, it, t);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_from_vector3_to_vector3(struct Iron_Vector3 from, struct Iron_Vector3 to) {
    Vector3 f, t;
    memcpy(&f, &from, sizeof(Vector3));
    memcpy(&t, &to,   sizeof(Vector3));
    Quaternion r = QuaternionFromVector3ToVector3(f, t);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Quaternion Iron_quaternion_from_matrix(struct Iron_Matrix mat) {
    Matrix m;
    memcpy(&m, &mat, sizeof(Matrix));
    Quaternion r = QuaternionFromMatrix(m);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Matrix Iron_quaternion_to_matrix(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Matrix r = QuaternionToMatrix(q);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

struct Iron_Quaternion Iron_quaternion_from_axis_angle(struct Iron_Vector3 axis, float angle) {
    Vector3 a;
    memcpy(&a, &axis, sizeof(Vector3));
    Quaternion r = QuaternionFromAxisAngle(a, angle);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

Iron_Tuple_Vector3_Float32 Iron_quaternion_to_axis_angle(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Vector3 axis = { 0 };
    float   angle = 0.0f;
    QuaternionToAxisAngle(q, &axis, &angle);
    Iron_Tuple_Vector3_Float32 out;
    memcpy(&out.v0, &axis, sizeof(Vector3));
    out.v1 = angle;
    return out;
}

struct Iron_Quaternion Iron_quaternion_from_euler(float pitch, float yaw, float roll) {
    Quaternion r = QuaternionFromEuler(pitch, yaw, roll);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

struct Iron_Vector3 Iron_quaternion_to_euler(struct Iron_Quaternion self) {
    Quaternion q;
    memcpy(&q, &self, sizeof(Quaternion));
    Vector3 r = QuaternionToEuler(q);
    struct Iron_Vector3 out;
    memcpy(&out, &r, sizeof(Vector3));
    return out;
}

struct Iron_Quaternion Iron_quaternion_transform(struct Iron_Quaternion self, struct Iron_Matrix mat) {
    Quaternion q;
    Matrix     m;
    memcpy(&q, &self, sizeof(Quaternion));
    memcpy(&m, &mat,  sizeof(Matrix));
    Quaternion r = QuaternionTransform(q, m);
    struct Iron_Quaternion out;
    memcpy(&out, &r, sizeof(Quaternion));
    return out;
}

bool Iron_quaternion_equals(struct Iron_Quaternion self, struct Iron_Quaternion other) {
    Quaternion a, b;
    memcpy(&a, &self,  sizeof(Quaternion));
    memcpy(&b, &other, sizeof(Quaternion));
    return (bool)(QuaternionEquals(a, b) != 0);
}

/* ── Textures & Images (Phase 66) ─────────────────────────────────── */

/* Color math — TEX-13 (Plan 66-01).
 *
 * Template for every Color-in / Color-out shim:
 *   1. memcpy Iron_Color args into local raylib Color values,
 *   2. call the corresponding raylib function,
 *   3. memcpy the returned raylib struct back into an Iron_Color out,
 *   4. return the out struct by value.
 *
 * ColorIsEqual returns a raylib C `bool` (int under the hood on some
 * toolchains) — coerced to Iron Bool via `(bool)(... != 0)` per the
 * Phase 62 precedent used for Vector2Equals/Vector3Equals/Vector4Equals.
 *
 * GetPixelColor / SetPixelColor take `void *` pixel buffers. The Iron
 * receiver is `data: Int` (int64_t); the shim casts via
 * `(void *)(intptr_t)data`. sizeof(int64_t) == sizeof(void *) on every
 * 64-bit target Iron supports (iron_raylib.h top-of-file contract).
 */

bool Iron_color_is_equal(struct Iron_Color c1, struct Iron_Color c2) {
    Color a, b;
    memcpy(&a, &c1, sizeof(Color));
    memcpy(&b, &c2, sizeof(Color));
    return (bool)(ColorIsEqual(a, b) != 0);
}

struct Iron_Color Iron_color_fade(struct Iron_Color c, float alpha) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    Color r = Fade(col, alpha);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

int32_t Iron_color_to_int(struct Iron_Color c) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    return (int32_t)ColorToInt(col);
}

struct Iron_Vector4 Iron_color_normalize(struct Iron_Color c) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    Vector4 v = ColorNormalize(col);
    struct Iron_Vector4 out;
    memcpy(&out, &v, sizeof(struct Iron_Vector4));
    return out;
}

struct Iron_Color Iron_color_from_normalized(struct Iron_Vector4 v) {
    Vector4 rv;
    memcpy(&rv, &v, sizeof(Vector4));
    Color r = ColorFromNormalized(rv);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Vector3 Iron_color_to_hsv(struct Iron_Color c) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    Vector3 v = ColorToHSV(col);
    struct Iron_Vector3 out;
    memcpy(&out, &v, sizeof(struct Iron_Vector3));
    return out;
}

struct Iron_Color Iron_color_from_hsv(float hue, float saturation, float value) {
    Color r = ColorFromHSV(hue, saturation, value);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Color Iron_color_tint(struct Iron_Color c, struct Iron_Color tint) {
    Color col, tn;
    memcpy(&col, &c,    sizeof(Color));
    memcpy(&tn,  &tint, sizeof(Color));
    Color r = ColorTint(col, tn);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Color Iron_color_brightness(struct Iron_Color c, float factor) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    Color r = ColorBrightness(col, factor);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Color Iron_color_contrast(struct Iron_Color c, float contrast) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    Color r = ColorContrast(col, contrast);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Color Iron_color_alpha(struct Iron_Color c, float alpha) {
    Color col;
    memcpy(&col, &c, sizeof(Color));
    Color r = ColorAlpha(col, alpha);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Color Iron_color_alpha_blend(struct Iron_Color dst, struct Iron_Color src, struct Iron_Color tint) {
    Color d, s, t;
    memcpy(&d, &dst,  sizeof(Color));
    memcpy(&s, &src,  sizeof(Color));
    memcpy(&t, &tint, sizeof(Color));
    Color r = ColorAlphaBlend(d, s, t);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Color Iron_color_lerp(struct Iron_Color c1, struct Iron_Color c2, float factor) {
    Color a, b;
    memcpy(&a, &c1, sizeof(Color));
    memcpy(&b, &c2, sizeof(Color));
    Color r = ColorLerp(a, b, factor);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

struct Iron_Color Iron_color_from_int(uint32_t hex_value) {
    Color r = GetColor(hex_value);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

/* Opaque void* arg probe — first in iron_raylib.c. sizeof(int64_t)
 * equals sizeof(void *) on every 64-bit target Iron supports. The
 * intptr_t hop is the canonical pointer-round-trip cast. */
struct Iron_Color Iron_color_from_pixel_data(int64_t data, int32_t format) {
    void *src_ptr = (void *)(intptr_t)data;
    Color r = GetPixelColor(src_ptr, (int)format);
    struct Iron_Color out;
    memcpy(&out, &r, sizeof(struct Iron_Color));
    return out;
}

void Iron_color_to_pixel_data(int64_t data, struct Iron_Color c, int32_t format) {
    void *dst_ptr = (void *)(intptr_t)data;
    Color col;
    memcpy(&col, &c, sizeof(Color));
    SetPixelColor(dst_ptr, col, (int)format);
}

int32_t Iron_color_pixel_data_size(int32_t width, int32_t height, int32_t format) {
    return (int32_t)GetPixelDataSize((int)width, (int)height, (int)format);
}

/* ── Text & Fonts (Phase 67) ──────────────────────────────────────── */
/* ── Audio (Phase 68) ─────────────────────────────────────────────── */
/* ── 3D Drawing (Phase 69) ────────────────────────────────────────── */
/* ── Models (Phase 70) ────────────────────────────────────────────── */
/* ── Shaders (Phase 71) ───────────────────────────────────────────── */
/* ── File I/O & Utils (Phase 72) ──────────────────────────────────── */

/* Phase 60 leaves this file intentionally empty of wrapper functions.
 * A later Phase 60 plan (60-08 clean-break) may rewrite pong.iron /
 * game_raylib.iron / hello_raylib.iron to only use TYPE/ENUM
 * definitions — no wrapper calls — so zero-function iron_raylib.c
 * still links. If a rewrite needs a specific wrapper earlier than
 * Phase 61, that plan will add a single section-scoped shim here. */
