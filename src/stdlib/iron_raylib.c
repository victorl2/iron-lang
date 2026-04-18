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

#include <stdlib.h>
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

/* Phase 67 extension: default-font draws (TEXT-07, TEXT-08 default variant).
 *
 * Pitfall 1 (67-RESEARCH.md): both DrawFPS and DrawText read raylib's
 * embedded default font, which is populated by rtext.c:LoadFontDefault
 * during InitWindow. Calling either shim before Window.init() dereferences
 * a null atlas pointer. Flagged in raylib.iron above each method.
 *
 * Iron_String carries text as a length-prefixed payload;
 * iron_string_cstr() returns a NUL-terminated view (no allocation for
 * typical string literals).
 */

void Iron_draw_fps(int32_t pos_x, int32_t pos_y) {
    DrawFPS((int)pos_x, (int)pos_y);
}

void Iron_draw_text(Iron_String text, int32_t pos_x, int32_t pos_y,
                     int32_t font_size, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawText(iron_string_cstr(&text), (int)pos_x, (int)pos_y,
             (int)font_size, c);
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

/* ════════════════════════════════════════════════════════════════════
 * Image I/O + Generation + Extraction (Plan 66-02 — TEX-01, narrowed
 * TEX-02, TEX-03, narrowed TEX-04, TEX-06).
 *
 * DEFERRED to a later phase per RESEARCH Pitfall 7 ([UInt8] FFI gap):
 *   LoadImageRaw, LoadImageFromMemory, LoadImageAnimFromMemory,
 *   ExportImageToMemory — all four take or return a raw unsigned-char
 *   buffer that Iron cannot yet express as a function parameter.
 *
 * Shim template (every Image-out function follows this shape):
 *   1. memcpy Iron args (Image / Color / Rectangle) into raylib locals,
 *   2. call the raylib function,
 *   3. memcpy the returned raylib struct back into an Iron mirror out,
 *   4. return by value.
 *
 * String arguments cross the FFI via iron_string_cstr(&s) (Phase 61
 * precedent used ~10 times in this file).
 * ════════════════════════════════════════════════════════════════════ */

/* TEX-01 + narrowed TEX-02 (file-path load + screen/texture sources) */
struct Iron_Image Iron_image_load(Iron_String path) {
    const char *cpath = iron_string_cstr(&path);
    Image src = LoadImage(cpath ? cpath : "");
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

void Iron_image_unload(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    UnloadImage(src);
}

bool Iron_image_is_valid(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    return (bool)(IsImageValid(src) != 0);
}

/* LoadImageAnim writes the animation frame count into an `int *frames`
 * out-param. Iron has no out-ref mechanism; the frame count is
 * discarded here. Callers needing it must wait for [UInt8]-FFI phase
 * when an out-tuple variant can land alongside. */
struct Iron_Image Iron_image_load_anim(Iron_String path) {
    const char *cpath = iron_string_cstr(&path);
    int frames = 0;
    Image src = LoadImageAnim(cpath ? cpath : "", &frames);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_from_texture(struct Iron_Texture tex) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    Image src = LoadImageFromTexture(t);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_from_screen(void) {
    Image src = LoadImageFromScreen();
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

/* TEX-03 procedural generators (9 functions) */
struct Iron_Image Iron_image_color(int32_t width, int32_t height, struct Iron_Color color) {
    Color col;
    memcpy(&col, &color, sizeof(Color));
    Image src = GenImageColor((int)width, (int)height, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_gradient_linear(int32_t width, int32_t height, int32_t direction,
                                              struct Iron_Color start, struct Iron_Color finish) {
    Color a, b;
    memcpy(&a, &start,  sizeof(Color));
    memcpy(&b, &finish, sizeof(Color));
    Image src = GenImageGradientLinear((int)width, (int)height, (int)direction, a, b);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_gradient_radial(int32_t width, int32_t height, float density,
                                              struct Iron_Color inner, struct Iron_Color outer) {
    Color i, o;
    memcpy(&i, &inner, sizeof(Color));
    memcpy(&o, &outer, sizeof(Color));
    Image src = GenImageGradientRadial((int)width, (int)height, density, i, o);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_gradient_square(int32_t width, int32_t height, float density,
                                              struct Iron_Color inner, struct Iron_Color outer) {
    Color i, o;
    memcpy(&i, &inner, sizeof(Color));
    memcpy(&o, &outer, sizeof(Color));
    Image src = GenImageGradientSquare((int)width, (int)height, density, i, o);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_checked(int32_t width, int32_t height, int32_t checks_x, int32_t checks_y,
                                      struct Iron_Color c1, struct Iron_Color c2) {
    Color a, b;
    memcpy(&a, &c1, sizeof(Color));
    memcpy(&b, &c2, sizeof(Color));
    Image src = GenImageChecked((int)width, (int)height, (int)checks_x, (int)checks_y, a, b);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_white_noise(int32_t width, int32_t height, float factor) {
    Image src = GenImageWhiteNoise((int)width, (int)height, factor);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_perlin_noise(int32_t width, int32_t height, int32_t offset_x,
                                           int32_t offset_y, float scale) {
    Image src = GenImagePerlinNoise((int)width, (int)height,
                                     (int)offset_x, (int)offset_y, scale);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_cellular(int32_t width, int32_t height, int32_t tile_size) {
    Image src = GenImageCellular((int)width, (int)height, (int)tile_size);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

/* GenImageText(width, height, text) — grayscale bitmap from text using
 * the default font. Requires Window.init() first (default font isn't
 * loaded until the raylib context is up; RESEARCH Pitfall 4). */
struct Iron_Image Iron_image_text(int32_t width, int32_t height, Iron_String text) {
    const char *ctext = iron_string_cstr(&text);
    Image src = GenImageText((int)width, (int)height, ctext ? ctext : "");
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

/* narrowed TEX-04 (file export — ExportImageToMemory deferred) */
bool Iron_image_export(struct Iron_Image img, Iron_String path) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    const char *cpath = iron_string_cstr(&path);
    return (bool)(ExportImage(src, cpath ? cpath : "") != 0);
}

bool Iron_image_export_as_code(struct Iron_Image img, Iron_String path) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    const char *cpath = iron_string_cstr(&path);
    return (bool)(ExportImageAsCode(src, cpath ? cpath : "") != 0);
}

/* TEX-06 data extraction — `-> [Color]` reverse-direction Iron_List.
 *
 * Task 1 probe GREEN path: emit_structs.c:309-312 auto-emits the
 * Iron_List_Iron_Color typedef + IRON_LIST_DECL + IRON_LIST_IMPL into
 * the consumer TU via the foreign-method-stub return scan. These shims
 * malloc their own items buffer (direct calloc, same pattern as
 * iron_net.c:build_address_list_from_addrinfo) so standalone
 * `clang -c iron_raylib.c` compiles without depending on the
 * compiler-emitted _create_with_capacity helper. raylib's
 * LoadImageColors / LoadImagePalette allocate the returned Color*; we
 * memcpy-own the data then call UnloadImageColors / UnloadImagePalette
 * so Iron holds the single authoritative copy. */
Iron_List_Iron_Color Iron_image_load_colors(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    Color *colors = LoadImageColors(src);
    int64_t count = (int64_t)src.width * src.height;
    Iron_List_Iron_Color out;
    out.items    = NULL;
    out.count    = 0;
    out.capacity = 0;
    if (count > 0) {
        out.items    = (struct Iron_Color *)calloc((size_t)count, sizeof(struct Iron_Color));
        out.capacity = count;
        if (colors && out.items) {
            memcpy(out.items, colors, (size_t)count * sizeof(struct Iron_Color));
            out.count = count;
        }
    }
    UnloadImageColors(colors);
    return out;
}

Iron_List_Iron_Color Iron_image_load_palette(struct Iron_Image img, int32_t max_palette_size) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    int color_count = 0;
    Color *colors = LoadImagePalette(src, (int)max_palette_size, &color_count);
    Iron_List_Iron_Color out;
    out.items    = NULL;
    out.count    = 0;
    out.capacity = 0;
    if (color_count > 0) {
        out.items    = (struct Iron_Color *)calloc((size_t)color_count, sizeof(struct Iron_Color));
        out.capacity = color_count;
        if (colors && out.items) {
            memcpy(out.items, colors, (size_t)color_count * sizeof(struct Iron_Color));
            out.count = color_count;
        }
    }
    UnloadImagePalette(colors);
    return out;
}

struct Iron_Rectangle Iron_image_get_alpha_border(struct Iron_Image img, float threshold) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    Rectangle r = GetImageAlphaBorder(src, threshold);
    struct Iron_Rectangle out;
    memcpy(&out, &r, sizeof(struct Iron_Rectangle));
    return out;
}

struct Iron_Color Iron_image_get_color(struct Iron_Image img, int32_t x, int32_t y) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    Color c = GetImageColor(src, (int)x, (int)y);
    struct Iron_Color out;
    memcpy(&out, &c, sizeof(struct Iron_Color));
    return out;
}

/*
 * TEX-05 Image transforms (Plan 66-03 Task 1 — 27 shims).
 *
 * Pattern 2 (mutating transform, return by value): memcpy Iron_Image in,
 * call ImageFoo(&src, ...) so raylib mutates in place, memcpy mutated src
 * out as fresh Iron_Image return. Pattern 1 (copy/from_rectangle/from_channel):
 * raylib returns Image by value from ImageCopy / ImageFromImage /
 * ImageFromChannel — memcpy input, capture return, memcpy out.
 *
 * ImageKernelConvolution DEFERRED: takes `const float *kernel`, requires
 * [Float32] FFI runtime support (Iron_List_float / Iron_List_double are
 * not in iron_runtime.h:824-830 pre-declared primitive list types).
 * Rebind in the phase that lands the Float list primitive alongside
 * [UInt8] (same Pitfall 7 variant as the 4 memory-buffer Image functions
 * deferred in Plan 66-02).
 */

/* Group A — 3 by-value-return functions (Pattern 1). */

struct Iron_Image Iron_image_copy(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    Image dup = ImageCopy(src);
    struct Iron_Image out;
    memcpy(&out, &dup, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_from_rectangle(struct Iron_Image img, struct Iron_Rectangle rec) {
    Image src;
    Rectangle r;
    memcpy(&src, &img, sizeof(Image));
    memcpy(&r,   &rec, sizeof(Rectangle));
    Image dup = ImageFromImage(src, r);
    struct Iron_Image out;
    memcpy(&out, &dup, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_from_channel(struct Iron_Image img, int32_t selected_channel) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    Image dup = ImageFromChannel(src, (int)selected_channel);
    struct Iron_Image out;
    memcpy(&out, &dup, sizeof(struct Iron_Image));
    return out;
}

/* Group B — 24 mutating transforms (Pattern 2). */

struct Iron_Image Iron_image_format(struct Iron_Image img, int32_t new_format) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageFormat(&src, (int)new_format);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_to_pot(struct Iron_Image img, struct Iron_Color fill) {
    Image src;
    Color col;
    memcpy(&src, &img,  sizeof(Image));
    memcpy(&col, &fill, sizeof(Color));
    ImageToPOT(&src, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_crop(struct Iron_Image img, struct Iron_Rectangle crop) {
    Image src;
    Rectangle r;
    memcpy(&src, &img,  sizeof(Image));
    memcpy(&r,   &crop, sizeof(Rectangle));
    ImageCrop(&src, r);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_alpha_crop(struct Iron_Image img, float threshold) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageAlphaCrop(&src, threshold);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_alpha_clear(struct Iron_Image img, struct Iron_Color color, float threshold) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageAlphaClear(&src, col, threshold);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_alpha_mask(struct Iron_Image img, struct Iron_Image mask) {
    Image src, mk;
    memcpy(&src, &img,  sizeof(Image));
    memcpy(&mk,  &mask, sizeof(Image));
    ImageAlphaMask(&src, mk);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_alpha_premultiply(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageAlphaPremultiply(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_blur_gaussian(struct Iron_Image img, int32_t blur_size) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageBlurGaussian(&src, (int)blur_size);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_resize(struct Iron_Image img, int32_t new_width, int32_t new_height) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageResize(&src, (int)new_width, (int)new_height);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_resize_nn(struct Iron_Image img, int32_t new_width, int32_t new_height) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageResizeNN(&src, (int)new_width, (int)new_height);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_resize_canvas(struct Iron_Image img, int32_t new_width, int32_t new_height,
                                            int32_t offset_x, int32_t offset_y, struct Iron_Color fill) {
    Image src;
    Color col;
    memcpy(&src, &img,  sizeof(Image));
    memcpy(&col, &fill, sizeof(Color));
    ImageResizeCanvas(&src, (int)new_width, (int)new_height, (int)offset_x, (int)offset_y, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_mipmaps(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageMipmaps(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_dither(struct Iron_Image img, int32_t r_bpp, int32_t g_bpp,
                                     int32_t b_bpp, int32_t a_bpp) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageDither(&src, (int)r_bpp, (int)g_bpp, (int)b_bpp, (int)a_bpp);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_flip_vertical(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageFlipVertical(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_flip_horizontal(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageFlipHorizontal(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_rotate(struct Iron_Image img, int32_t degrees) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageRotate(&src, (int)degrees);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_rotate_cw(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageRotateCW(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_rotate_ccw(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageRotateCCW(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_color_tint(struct Iron_Image img, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageColorTint(&src, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_color_invert(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageColorInvert(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_color_grayscale(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageColorGrayscale(&src);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_color_contrast(struct Iron_Image img, float contrast) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageColorContrast(&src, contrast);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_color_brightness(struct Iron_Image img, int32_t brightness) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    ImageColorBrightness(&src, (int)brightness);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_color_replace(struct Iron_Image img, struct Iron_Color color,
                                            struct Iron_Color replace) {
    Image src;
    Color a, b;
    memcpy(&src, &img,     sizeof(Image));
    memcpy(&a,   &color,   sizeof(Color));
    memcpy(&b,   &replace, sizeof(Color));
    ImageColorReplace(&src, a, b);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

/*
 * TEX-07 Image CPU draw (Plan 66-03 Task 2 — 21 shims).
 *
 * Every shim uses Pattern 2: memcpy Iron_Image in, call
 * ImageDrawFoo(&dst, ...) so raylib mutates dst in place, memcpy
 * mutated dst back out. Vector2/Rectangle/Color parameters memcpy'd
 * alongside the Image. Array draws (triangle_fan/triangle_strip)
 * reuse the Iron_List_Iron_Vector2 by-value ABI from Phase 63-03.
 *
 * ImageDrawTextEx: Phase 66 deferral closed in Phase 67 Plan 01 Task 2
 * (see the Iron_image_text_ex / Iron_image_draw_text_ex shims below,
 * placed after the Pattern 2 mutating-transforms run).
 *
 * ImageDrawText uses the raylib default font — Pitfall 4 requires
 * Window.init() to have loaded the default font before the call.
 */

struct Iron_Image Iron_image_clear_background(struct Iron_Image img, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageClearBackground(&src, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_pixel(struct Iron_Image img, int32_t pos_x, int32_t pos_y,
                                         struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawPixel(&src, (int)pos_x, (int)pos_y, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_pixel_v(struct Iron_Image img, struct Iron_Vector2 position,
                                           struct Iron_Color color) {
    Image src;
    Vector2 p;
    Color col;
    memcpy(&src, &img,      sizeof(Image));
    memcpy(&p,   &position, sizeof(Vector2));
    memcpy(&col, &color,    sizeof(Color));
    ImageDrawPixelV(&src, p, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_line(struct Iron_Image img, int32_t start_x, int32_t start_y,
                                        int32_t end_x, int32_t end_y, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawLine(&src, (int)start_x, (int)start_y, (int)end_x, (int)end_y, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_line_v(struct Iron_Image img, struct Iron_Vector2 start,
                                          struct Iron_Vector2 finish, struct Iron_Color color) {
    Image src;
    Vector2 a, b;
    Color col;
    memcpy(&src, &img,    sizeof(Image));
    memcpy(&a,   &start,  sizeof(Vector2));
    memcpy(&b,   &finish, sizeof(Vector2));
    memcpy(&col, &color,  sizeof(Color));
    ImageDrawLineV(&src, a, b, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_line_ex(struct Iron_Image img, struct Iron_Vector2 start,
                                           struct Iron_Vector2 finish, int32_t thick,
                                           struct Iron_Color color) {
    Image src;
    Vector2 a, b;
    Color col;
    memcpy(&src, &img,    sizeof(Image));
    memcpy(&a,   &start,  sizeof(Vector2));
    memcpy(&b,   &finish, sizeof(Vector2));
    memcpy(&col, &color,  sizeof(Color));
    ImageDrawLineEx(&src, a, b, (int)thick, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_circle(struct Iron_Image img, int32_t center_x, int32_t center_y,
                                          int32_t radius, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawCircle(&src, (int)center_x, (int)center_y, (int)radius, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_circle_v(struct Iron_Image img, struct Iron_Vector2 center,
                                            int32_t radius, struct Iron_Color color) {
    Image src;
    Vector2 p;
    Color col;
    memcpy(&src, &img,    sizeof(Image));
    memcpy(&p,   &center, sizeof(Vector2));
    memcpy(&col, &color,  sizeof(Color));
    ImageDrawCircleV(&src, p, (int)radius, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_circle_lines(struct Iron_Image img, int32_t center_x, int32_t center_y,
                                                int32_t radius, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawCircleLines(&src, (int)center_x, (int)center_y, (int)radius, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_circle_lines_v(struct Iron_Image img, struct Iron_Vector2 center,
                                                  int32_t radius, struct Iron_Color color) {
    Image src;
    Vector2 p;
    Color col;
    memcpy(&src, &img,    sizeof(Image));
    memcpy(&p,   &center, sizeof(Vector2));
    memcpy(&col, &color,  sizeof(Color));
    ImageDrawCircleLinesV(&src, p, (int)radius, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_rectangle(struct Iron_Image img, int32_t pos_x, int32_t pos_y,
                                             int32_t width, int32_t height, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawRectangle(&src, (int)pos_x, (int)pos_y, (int)width, (int)height, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_rectangle_v(struct Iron_Image img, struct Iron_Vector2 position,
                                               struct Iron_Vector2 size, struct Iron_Color color) {
    Image src;
    Vector2 p, s;
    Color col;
    memcpy(&src, &img,      sizeof(Image));
    memcpy(&p,   &position, sizeof(Vector2));
    memcpy(&s,   &size,     sizeof(Vector2));
    memcpy(&col, &color,    sizeof(Color));
    ImageDrawRectangleV(&src, p, s, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_rectangle_rec(struct Iron_Image img, struct Iron_Rectangle rec,
                                                 struct Iron_Color color) {
    Image src;
    Rectangle r;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&r,   &rec,   sizeof(Rectangle));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawRectangleRec(&src, r, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_rectangle_lines(struct Iron_Image img, struct Iron_Rectangle rec,
                                                   int32_t thick, struct Iron_Color color) {
    Image src;
    Rectangle r;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&r,   &rec,   sizeof(Rectangle));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawRectangleLines(&src, r, (int)thick, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_triangle(struct Iron_Image img, struct Iron_Vector2 v1,
                                            struct Iron_Vector2 v2, struct Iron_Vector2 v3,
                                            struct Iron_Color color) {
    Image src;
    Vector2 a, b, c;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&a,   &v1,    sizeof(Vector2));
    memcpy(&b,   &v2,    sizeof(Vector2));
    memcpy(&c,   &v3,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawTriangle(&src, a, b, c, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_triangle_ex(struct Iron_Image img, struct Iron_Vector2 v1,
                                               struct Iron_Vector2 v2, struct Iron_Vector2 v3,
                                               struct Iron_Color c1, struct Iron_Color c2,
                                               struct Iron_Color c3) {
    Image src;
    Vector2 a, b, c;
    Color ca, cb, cc;
    memcpy(&src, &img, sizeof(Image));
    memcpy(&a,   &v1,  sizeof(Vector2));
    memcpy(&b,   &v2,  sizeof(Vector2));
    memcpy(&c,   &v3,  sizeof(Vector2));
    memcpy(&ca,  &c1,  sizeof(Color));
    memcpy(&cb,  &c2,  sizeof(Color));
    memcpy(&cc,  &c3,  sizeof(Color));
    ImageDrawTriangleEx(&src, a, b, c, ca, cb, cc);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_triangle_lines(struct Iron_Image img, struct Iron_Vector2 v1,
                                                  struct Iron_Vector2 v2, struct Iron_Vector2 v3,
                                                  struct Iron_Color color) {
    Image src;
    Vector2 a, b, c;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&a,   &v1,    sizeof(Vector2));
    memcpy(&b,   &v2,    sizeof(Vector2));
    memcpy(&c,   &v3,    sizeof(Vector2));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawTriangleLines(&src, a, b, c, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_triangle_fan(struct Iron_Image img, Iron_List_Iron_Vector2 points,
                                                int32_t point_count, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawTriangleFan(&src, (Vector2 *)points.items, (int)point_count, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_triangle_strip(struct Iron_Image img, Iron_List_Iron_Vector2 points,
                                                  int32_t point_count, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    ImageDrawTriangleStrip(&src, (Vector2 *)points.items, (int)point_count, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw(struct Iron_Image img, struct Iron_Image src_img,
                                   struct Iron_Rectangle src_rec, struct Iron_Rectangle dst_rec,
                                   struct Iron_Color tint) {
    Image dst, sc;
    Rectangle sr, dr;
    Color col;
    memcpy(&dst, &img,     sizeof(Image));
    memcpy(&sc,  &src_img, sizeof(Image));
    memcpy(&sr,  &src_rec, sizeof(Rectangle));
    memcpy(&dr,  &dst_rec, sizeof(Rectangle));
    memcpy(&col, &tint,    sizeof(Color));
    ImageDraw(&dst, sc, sr, dr, col);
    struct Iron_Image out;
    memcpy(&out, &dst, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_text(struct Iron_Image img, Iron_String text, int32_t pos_x,
                                        int32_t pos_y, int32_t font_size, struct Iron_Color color) {
    Image src;
    Color col;
    memcpy(&src, &img,   sizeof(Image));
    memcpy(&col, &color, sizeof(Color));
    const char *ctext = iron_string_cstr(&text);
    /* Pitfall 4: default font requires Window.init() before this runs. */
    ImageDrawText(&src, ctext ? ctext : "", (int)pos_x, (int)pos_y, (int)font_size, col);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

/*
 * TEX-05 + TEX-07 Phase 66 deferrals closed in Phase 67 Plan 01 Task 2.
 * Font type now exists — ImageTextEx (allocates fresh Image) and
 * ImageDrawTextEx (mutating-return-by-value per Pattern 2) land here,
 * placed alongside the other Image draw shims. Font lifetime must
 * outlive both calls (Pitfall 6 — raylib reads Font._recs / _glyphs
 * internally to look up glyphs).
 */

struct Iron_Image Iron_image_text_ex(struct Iron_Font font, Iron_String text,
                                      float font_size, float spacing,
                                      struct Iron_Color tint) {
    Font f;
    Color c;
    memcpy(&f, &font, sizeof(Font));
    memcpy(&c, &tint, sizeof(Color));
    const char *ctext = iron_string_cstr(&text);
    Image rl = ImageTextEx(f, ctext ? ctext : "", font_size, spacing, c);
    struct Iron_Image out;
    memcpy(&out, &rl, sizeof(struct Iron_Image));
    return out;
}

struct Iron_Image Iron_image_draw_text_ex(struct Iron_Image img, struct Iron_Font font,
                                           Iron_String text, struct Iron_Vector2 position,
                                           float font_size, float spacing,
                                           struct Iron_Color tint) {
    Image src;
    Font f;
    Vector2 p;
    Color c;
    memcpy(&src, &img,      sizeof(Image));
    memcpy(&f,   &font,     sizeof(Font));
    memcpy(&p,   &position, sizeof(Vector2));
    memcpy(&c,   &tint,     sizeof(Color));
    const char *ctext = iron_string_cstr(&text);
    ImageDrawTextEx(&src, f, ctext ? ctext : "", p, font_size, spacing, c);
    struct Iron_Image out;
    memcpy(&out, &src, sizeof(struct Iron_Image));
    return out;
}

/* ════════════════════════════════════════════════════════════════════
 * TEX-08/09/10/11 Texture + RenderTexture (Plan 66-04 Task 1 — 12 shims).
 *
 * First RenderTexture-by-value RETURN (44 B, under -Wlarge-by-value-copy
 * 64 B threshold). Opaque void* ARG for texture updates via int64_t →
 * (void *)(intptr_t) cast (Pattern 4 from Plan 66-01 probe).
 * TextureCubemap / RenderTexture2D are plain typedef aliases of
 * Texture / RenderTexture in raylib.h (Pitfall 8) — the memcpy shares
 * the same Iron mirror structs.
 * ════════════════════════════════════════════════════════════════════ */

/* TEX-08: Texture load/unload/valid (4 shims). */

struct Iron_Texture Iron_texture_load(Iron_String path) {
    const char *cpath = iron_string_cstr(&path);
    Texture2D t = LoadTexture(cpath ? cpath : "");
    struct Iron_Texture out;
    memcpy(&out, &t, sizeof(struct Iron_Texture));
    return out;
}

struct Iron_Texture Iron_image_to_texture(struct Iron_Image img) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    Texture2D t = LoadTextureFromImage(src);
    struct Iron_Texture out;
    memcpy(&out, &t, sizeof(struct Iron_Texture));
    return out;
}

void Iron_texture_unload(struct Iron_Texture tex) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    UnloadTexture(t);
}

bool Iron_texture_is_valid(struct Iron_Texture tex) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    return (bool)(IsTextureValid(t) != 0);
}

/* TEX-09: Cubemap + RenderTexture load/unload/valid (4 shims).
 * First RenderTexture-by-value RETURN at 44 B. */

struct Iron_Texture Iron_texture_load_cubemap(struct Iron_Image img, int32_t layout) {
    Image src;
    memcpy(&src, &img, sizeof(Image));
    TextureCubemap tc = LoadTextureCubemap(src, (int)layout);
    struct Iron_Texture out;
    /* TextureCubemap is typedef of Texture — Pitfall 8 */
    memcpy(&out, &tc, sizeof(struct Iron_Texture));
    return out;
}

struct Iron_RenderTexture Iron_rendertexture_load(int32_t width, int32_t height) {
    RenderTexture2D rt = LoadRenderTexture((int)width, (int)height);
    struct Iron_RenderTexture out;
    /* 44 B memcpy — first RenderTexture by-value return */
    memcpy(&out, &rt, sizeof(struct Iron_RenderTexture));
    return out;
}

void Iron_rendertexture_unload(struct Iron_RenderTexture rt) {
    RenderTexture2D t;
    memcpy(&t, &rt, sizeof(RenderTexture));
    UnloadRenderTexture(t);
}

bool Iron_rendertexture_is_valid(struct Iron_RenderTexture rt) {
    RenderTexture2D t;
    memcpy(&t, &rt, sizeof(RenderTexture));
    return (bool)(IsRenderTextureValid(t) != 0);
}

/* TEX-10: Texture update via opaque Int → void* cast (2 shims).
 * Extends Plan 66-01's Color.from_pixel_data probe. */

void Iron_texture_update(struct Iron_Texture tex, int64_t pixels) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    const void *px = (const void *)(intptr_t)pixels;
    UpdateTexture(t, px);
}

void Iron_texture_update_rec(struct Iron_Texture tex, struct Iron_Rectangle rec, int64_t pixels) {
    Texture t;
    Rectangle r;
    memcpy(&t, &tex, sizeof(Texture));
    memcpy(&r, &rec, sizeof(Rectangle));
    const void *px = (const void *)(intptr_t)pixels;
    UpdateTextureRec(t, r, px);
}

/* TEX-11: Config — filter / wrap / mipmaps (3 shims).
 * set_filter / set_wrap take Texture by value; raylib mutates GPU state
 * on the texture's `id` (handle) so Iron-side struct is not returned.
 * gen_mipmaps uses the mutating-pointer pattern: memcpy in, raylib
 * mutates &t in place, memcpy mutated struct out. */

void Iron_texture_set_filter(struct Iron_Texture tex, int32_t filter) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    SetTextureFilter(t, (int)filter);
}

void Iron_texture_set_wrap(struct Iron_Texture tex, int32_t wrap) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    SetTextureWrap(t, (int)wrap);
}

struct Iron_Texture Iron_texture_gen_mipmaps(struct Iron_Texture tex) {
    Texture t;
    memcpy(&t, &tex, sizeof(Texture));
    GenTextureMipmaps(&t);
    struct Iron_Texture out;
    memcpy(&out, &t, sizeof(struct Iron_Texture));
    return out;
}

/* ════════════════════════════════════════════════════════════════════
 * TEX-12 Texture draw variants (Plan 66-04 Task 2 — 6 shims).
 *
 * Texture / Vector2 / Rectangle / Color memcpy pattern from Phase 63
 * scaled to 6 draw entry points. draw_n_patch is the first
 * NPatchInfo-by-value INPUT across the FFI (36 B, under -Wlarge-by-
 * value-copy 64 B threshold).
 * ════════════════════════════════════════════════════════════════════ */

void Iron_texture_draw(struct Iron_Texture tex, int32_t pos_x, int32_t pos_y,
                        struct Iron_Color tint) {
    Texture t;
    Color c;
    memcpy(&t, &tex,  sizeof(Texture));
    memcpy(&c, &tint, sizeof(Color));
    DrawTexture(t, (int)pos_x, (int)pos_y, c);
}

void Iron_texture_draw_v(struct Iron_Texture tex, struct Iron_Vector2 position,
                          struct Iron_Color tint) {
    Texture t;
    Vector2 p;
    Color c;
    memcpy(&t, &tex,      sizeof(Texture));
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&c, &tint,     sizeof(Color));
    DrawTextureV(t, p, c);
}

void Iron_texture_draw_ex(struct Iron_Texture tex, struct Iron_Vector2 position,
                           float rotation, float scale, struct Iron_Color tint) {
    Texture t;
    Vector2 p;
    Color c;
    memcpy(&t, &tex,      sizeof(Texture));
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&c, &tint,     sizeof(Color));
    DrawTextureEx(t, p, rotation, scale, c);
}

void Iron_texture_draw_rec(struct Iron_Texture tex, struct Iron_Rectangle source,
                            struct Iron_Vector2 position, struct Iron_Color tint) {
    Texture t;
    Rectangle s;
    Vector2 p;
    Color c;
    memcpy(&t, &tex,      sizeof(Texture));
    memcpy(&s, &source,   sizeof(Rectangle));
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&c, &tint,     sizeof(Color));
    DrawTextureRec(t, s, p, c);
}

void Iron_texture_draw_pro(struct Iron_Texture tex, struct Iron_Rectangle source,
                            struct Iron_Rectangle dest, struct Iron_Vector2 origin,
                            float rotation, struct Iron_Color tint) {
    Texture t;
    Rectangle s, d;
    Vector2 o;
    Color c;
    memcpy(&t, &tex,    sizeof(Texture));
    memcpy(&s, &source, sizeof(Rectangle));
    memcpy(&d, &dest,   sizeof(Rectangle));
    memcpy(&o, &origin, sizeof(Vector2));
    memcpy(&c, &tint,   sizeof(Color));
    DrawTexturePro(t, s, d, o, rotation, c);
}

void Iron_texture_draw_n_patch(struct Iron_Texture tex, struct Iron_NPatchInfo n_patch_info,
                                struct Iron_Rectangle dest, struct Iron_Vector2 origin,
                                float rotation, struct Iron_Color tint) {
    Texture t;
    NPatchInfo n;
    Rectangle d;
    Vector2 o;
    Color c;
    memcpy(&t, &tex,          sizeof(Texture));
    /* First NPatchInfo-by-value INPUT (36 B) */
    memcpy(&n, &n_patch_info, sizeof(NPatchInfo));
    memcpy(&d, &dest,         sizeof(Rectangle));
    memcpy(&o, &origin,       sizeof(Vector2));
    memcpy(&c, &tint,         sizeof(Color));
    DrawTextureNPatch(t, n, d, o, rotation, c);
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Text & Fonts (Phase 67) ──────────────────────────────────────────
 *
 * Font loading + lifecycle (TEXT-01..04, TEXT-06). See 67-RESEARCH.md.
 *
 * Pattern 1 (RETURN by value): LoadFont / LoadFontEx / LoadFontFromImage /
 *   GetFontDefault all return raylib Font (48 B). Shim memcpys into
 *   Iron_Font. Byte-identity enforced by Phase 60-03 _Static_assert grid.
 * Pattern 2 (INPUT by value): IsFontValid / UnloadFont / ExportFontAsCode
 *   take raylib Font by value. Shim memcpys Iron_Font into raylib Font
 *   local.
 * Pattern 3 (Iron_List_int32_t INPUT): Font.load_ex takes [Int32]
 *   codepoints. Iron_List_int32_t is pre-declared in iron_runtime.h:826 —
 *   pass (int *)items to raylib.
 *
 * Pitfall 1: GetFontDefault requires Window.init() to have loaded the
 *   default atlas via rtext.c:LoadFontDefault. Documented in raylib.iron.
 * Pitfall 5: LoadFont returns Font with baseSize=0 on failure — users
 *   must call font.is_valid() before drawing.
 *
 * Font.from_memory DEFERRED: [UInt8] FFI blocker — Iron_List_uint8_t not
 *   pre-declared. Matches 5 existing Phase 66 [UInt8] deferrals.
 * Font.load_data DEFERRED: same [UInt8] FFI blocker (raw fileData input).
 *   Unblocks when Iron_List_uint8_t lands in iron_runtime.h.
 * ══════════════════════════════════════════════════════════════════════ */

struct Iron_Font Iron_font_default(void) {
    Font rl = GetFontDefault();
    struct Iron_Font out;
    memcpy(&out, &rl, sizeof(struct Iron_Font));
    return out;
}

struct Iron_Font Iron_font_load(Iron_String file_name) {
    const char *cpath = iron_string_cstr(&file_name);
    Font rl = LoadFont(cpath ? cpath : "");
    struct Iron_Font out;
    memcpy(&out, &rl, sizeof(struct Iron_Font));
    return out;
}

struct Iron_Font Iron_font_load_ex(Iron_String file_name, int32_t font_size,
                                    Iron_List_int32_t codepoints) {
    const char *cpath = iron_string_cstr(&file_name);
    /* codepoints.items is int32_t * (pre-declared in iron_runtime.h:826);
     * raylib's LoadFontEx expects int *. Same type on all 64-bit targets
     * Iron supports. */
    Font rl = LoadFontEx(cpath ? cpath : "", (int)font_size,
                         (int *)codepoints.items, (int)codepoints.count);
    struct Iron_Font out;
    memcpy(&out, &rl, sizeof(struct Iron_Font));
    return out;
}

struct Iron_Font Iron_font_from_image(struct Iron_Image image, struct Iron_Color key,
                                       int32_t first_char) {
    Image img;
    Color col;
    memcpy(&img, &image, sizeof(Image));
    memcpy(&col, &key,   sizeof(Color));
    Font rl = LoadFontFromImage(img, col, (int)first_char);
    struct Iron_Font out;
    memcpy(&out, &rl, sizeof(struct Iron_Font));
    return out;
}

bool Iron_font_is_valid(struct Iron_Font font) {
    Font rl;
    memcpy(&rl, &font, sizeof(Font));
    return (bool)(IsFontValid(rl) != 0);
}

void Iron_font_unload(struct Iron_Font font) {
    Font rl;
    memcpy(&rl, &font, sizeof(Font));
    UnloadFont(rl);
}

bool Iron_font_export_as_code(struct Iron_Font font, Iron_String file_name) {
    Font rl;
    memcpy(&rl, &font, sizeof(Font));
    const char *cpath = iron_string_cstr(&file_name);
    return (bool)(ExportFontAsCode(rl, cpath ? cpath : "") != 0);
}

/*
 * TEXT-05 Font data operations (2 shims — gen_image_atlas + unload_data).
 *
 * Iron_font_gen_image_atlas takes [GlyphInfo] by value (Iron_List_Iron_GlyphInfo
 * is 24 B items/count/capacity wrapper — same ARRAY_PARAM_LIST mode as
 * Phase 63-03 triangle_fan/strip and Phase 66-02 image_load_colors).
 * Returns a (Image, [Rectangle]) tuple. raylib's GenImageFontAtlas writes
 * per-glyph source rectangles via a Rectangle** out-param; the shim
 * deep-copies those into a fresh Iron_List_Iron_Rectangle and calls
 * free() on the raylib RL_MALLOC'd outer buffer (Pattern 4 copy-out
 * template from Iron_image_load_colors at line 2971).
 *
 * The cast (const GlyphInfo *)glyphs.items is byte-safe because
 * Phase 60-03 _Static_assert grid pins Iron_GlyphInfo layout to raylib's
 * GlyphInfo (40 B, field-for-field).
 *
 * Font.load_data + Font.from_memory remain DEFERRED: both take [UInt8]
 * fileData which requires Iron_List_uint8_t (not in iron_runtime.h yet).
 * See iron_raylib.h Phase 67 section comment for full defer rationale.
 */

Iron_Tuple_Image__Rectangle_
Iron_font_gen_image_atlas(Iron_List_Iron_GlyphInfo glyphs, int32_t font_size,
                           int32_t padding, int32_t pack_method) {
    Rectangle *recs_out = NULL;
    Image rl = GenImageFontAtlas((const GlyphInfo *)glyphs.items, &recs_out,
                                  (int)glyphs.count, (int)font_size,
                                  (int)padding, (int)pack_method);

    /* Build Iron_List_Iron_Rectangle by memcpy'ing each rec into an
     * Iron-owned calloc buffer. raylib allocates recs_out via RL_MALLOC
     * which is plain malloc by default; free() is the matched deallocator. */
    Iron_List_Iron_Rectangle recs;
    recs.items    = NULL;
    recs.count    = 0;
    recs.capacity = 0;
    if (recs_out && glyphs.count > 0) {
        recs.items    = (struct Iron_Rectangle *)calloc((size_t)glyphs.count,
                                                         sizeof(struct Iron_Rectangle));
        recs.capacity = glyphs.count;
        if (recs.items) {
            memcpy(recs.items, recs_out,
                   (size_t)glyphs.count * sizeof(struct Iron_Rectangle));
            recs.count = glyphs.count;
        }
    }
    free(recs_out);

    Iron_Tuple_Image__Rectangle_ out;
    memcpy(&out.v0, &rl, sizeof(struct Iron_Image));
    out.v1 = recs;
    return out;
}

void Iron_font_unload_data(Iron_List_Iron_GlyphInfo glyphs) {
    /* Cast-safe: Iron_GlyphInfo is byte-identical to raylib GlyphInfo
     * per Phase 60-03 _Static_assert grid. */
    UnloadFontData((GlyphInfo *)glyphs.items, (int)glyphs.count);
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Plan 67-02 Task 1: Text.* namespace utilities ────────────────────
 *
 * TEXT-09 (default-font measure) + TEXT-10 (line spacing). Both are
 * default-font-only; custom-font measure lives on Font.measure_ex in
 * Plan 67-02 Task 2. SetTextLineSpacing mutates raylib's internal
 * gTextLineSpacing state — affects every subsequent multi-line draw
 * (default or custom font).
 *
 * Pitfall 1: MeasureText reads the default atlas; requires Window.init.
 * ══════════════════════════════════════════════════════════════════════ */

int32_t Iron_text_measure(Iron_String text, int32_t font_size) {
    return (int32_t)MeasureText(iron_string_cstr(&text), (int)font_size);
}

void Iron_text_set_line_spacing(int32_t spacing) {
    SetTextLineSpacing((int)spacing);
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Plan 67-02 Task 2: Font.* instance methods ───────────────────────
 *
 * TEXT-08 custom-font draws + TEXT-10 custom-font measure + TEXT-11
 * glyph lookup. Every shim memcpys Iron_Font (48 B) by value into a
 * raylib Font local using Phase 66-04 Texture-by-value INPUT template.
 *
 * Font_draw_codepoints takes Iron_List_int32_t by value — second raylib
 * call site after Plan 67-01 Font.load_ex. The .items cast from
 * int32_t * to const int * is safe on all 64-bit targets Iron supports
 * (Iron Int32 is int32_t, raylib takes int which is also 32-bit).
 *
 * Iron_font_get_glyph_info returns Iron_GlyphInfo (40 B) by value —
 * FIRST GlyphInfo RETURN in the binding. The embedded Image at
 * offset 16 carries a .data pointer aliasing the parent Font's heap
 * glyph array (Pitfall 7). Users must keep the parent Font alive as
 * long as the returned GlyphInfo is read — documented in raylib.iron.
 *
 * 40 B is under clang's default -Wlarge-by-value-copy threshold (64 B),
 * so no warning is expected for the GlyphInfo return.
 * ══════════════════════════════════════════════════════════════════════ */

/* Custom-font draws (TEXT-08) */

void Iron_font_draw_ex(struct Iron_Font font, Iron_String text,
                        struct Iron_Vector2 position, float font_size,
                        float spacing, struct Iron_Color tint) {
    Font    f;
    Vector2 p;
    Color   c;
    memcpy(&f, &font,     sizeof(Font));
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&c, &tint,     sizeof(Color));
    DrawTextEx(f, iron_string_cstr(&text), p, font_size, spacing, c);
}

void Iron_font_draw_pro(struct Iron_Font font, Iron_String text,
                         struct Iron_Vector2 position, struct Iron_Vector2 origin,
                         float rotation, float font_size, float spacing,
                         struct Iron_Color tint) {
    Font    f;
    Vector2 p, o;
    Color   c;
    memcpy(&f, &font,     sizeof(Font));
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&o, &origin,   sizeof(Vector2));
    memcpy(&c, &tint,     sizeof(Color));
    DrawTextPro(f, iron_string_cstr(&text), p, o, rotation, font_size, spacing, c);
}

void Iron_font_draw_codepoint(struct Iron_Font font, int32_t codepoint,
                               struct Iron_Vector2 position, float font_size,
                               struct Iron_Color tint) {
    Font    f;
    Vector2 p;
    Color   c;
    memcpy(&f, &font,     sizeof(Font));
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&c, &tint,     sizeof(Color));
    DrawTextCodepoint(f, (int)codepoint, p, font_size, c);
}

void Iron_font_draw_codepoints(struct Iron_Font font, Iron_List_int32_t codepoints,
                                struct Iron_Vector2 position, float font_size,
                                float spacing, struct Iron_Color tint) {
    Font    f;
    Vector2 p;
    Color   c;
    memcpy(&f, &font,     sizeof(Font));
    memcpy(&p, &position, sizeof(Vector2));
    memcpy(&c, &tint,     sizeof(Color));
    /* Iron_List_int32_t.items is int32_t *; raylib wants const int *.
     * Same int32==int target-ABI assumption as Plan 67-01 Font.load_ex. */
    DrawTextCodepoints(f, (const int *)codepoints.items, (int)codepoints.count,
                        p, font_size, spacing, c);
}

/* Custom-font measure (TEXT-10) — Vector2 RETURN */

struct Iron_Vector2 Iron_font_measure_ex(struct Iron_Font font, Iron_String text,
                                          float font_size, float spacing) {
    Font f;
    memcpy(&f, &font, sizeof(Font));
    Vector2 rv = MeasureTextEx(f, iron_string_cstr(&text), font_size, spacing);
    struct Iron_Vector2 out;
    memcpy(&out, &rv, sizeof(struct Iron_Vector2));
    return out;
}

/* Glyph lookup (TEXT-11) */

int32_t Iron_font_get_glyph_index(struct Iron_Font font, int32_t codepoint) {
    Font f;
    memcpy(&f, &font, sizeof(Font));
    return (int32_t)GetGlyphIndex(f, (int)codepoint);
}

struct Iron_GlyphInfo Iron_font_get_glyph_info(struct Iron_Font font, int32_t codepoint) {
    Font f;
    memcpy(&f, &font, sizeof(Font));
    /* First GlyphInfo (40 B) struct-by-value RETURN in the binding.
     * Byte-identity pinned by Phase 60-03 _Static_assert grid — the
     * embedded Image at offset 16 carries a .data pointer aliasing the
     * parent Font's heap glyph array (Pitfall 7). */
    GlyphInfo rl = GetGlyphInfo(f, (int)codepoint);
    struct Iron_GlyphInfo out;
    memcpy(&out, &rl, sizeof(struct Iron_GlyphInfo));
    return out;
}

struct Iron_Rectangle Iron_font_get_glyph_atlas_rec(struct Iron_Font font, int32_t codepoint) {
    Font f;
    memcpy(&f, &font, sizeof(Font));
    Rectangle rv = GetGlyphAtlasRec(f, (int)codepoint);
    struct Iron_Rectangle out;
    memcpy(&out, &rv, sizeof(struct Iron_Rectangle));
    return out;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Plan 67-03 Task 1: PROBE — [Int32] RETURN via Text.load_codepoints
 *
 * Pattern 4 (novel for primitive elements): raylib returns int *cps +
 * count via out-param. Shim calloc-s a fresh Iron_List_int32_t, memcpys
 * the codepoints in, calls UnloadCodepoints to free raylib's original
 * buffer.
 *
 * Template source: Iron_image_load_colors at iron_raylib.c:2979-3010
 * (Phase 66-02 — [Color] RETURN via Iron_List_Iron_Color). The Int32
 * variant uses calloc with sizeof(int32_t) instead of sizeof(Iron_Color).
 *
 * Iron_List_int32_t is pre-declared at iron_runtime.h:826 — no macro
 * expansion needed on the runtime side. This probe confirms the
 * foreign-method-stub return-type scan at emit_structs.c:300-313 (Scan B)
 * behaves correctly for primitive-element lists. Because Scan B's
 * PLAN_63_04_EMIT_LIST_FOR macro only fires for IRON_TYPE_OBJECT
 * elements, primitive [Int32] bypasses Scan B entirely and relies on
 * the runtime pre-declaration — exactly the contract we want.
 * ══════════════════════════════════════════════════════════════════════ */

Iron_List_int32_t Iron_text_load_codepoints(Iron_String text) {
    int count = 0;
    int *cps = LoadCodepoints(iron_string_cstr(&text), &count);
    Iron_List_int32_t out;
    out.items    = NULL;
    out.count    = 0;
    out.capacity = 0;
    if (count > 0) {
        out.items    = (int32_t *)calloc((size_t)count, sizeof(int32_t));
        out.capacity = count;
        if (cps && out.items) {
            memcpy(out.items, cps, (size_t)count * sizeof(int32_t));
            out.count = count;
        }
    }
    UnloadCodepoints(cps);
    return out;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Plan 67-03 Task 2: PROBE — Iron_String allocation from raylib char*
 *
 * Pattern 5 (static-buffer variant): raylib's CodepointToUTF8 returns
 * a const char* into a 6-byte static buffer (rtext.c:1914 `static char
 * utf8[6] = { 0 }`). Buffer is reset to zero BEFORE the next call. Shim
 * iron_string_from_cstr's it immediately so Iron owns the returned
 * String lifetime.
 *
 * Template source: Iron_window_get_clipboard_text at iron_raylib.c:205-211
 * + Iron_window_get_monitor_name at lines 192-198 (Phase 61).
 *
 * Pitfalls 2 + 3 (67-RESEARCH.md): shim MUST copy before control returns
 * to user or to any subsequent raylib call. iron_string_from_cstr does
 * the copy.
 * ══════════════════════════════════════════════════════════════════════ */

Iron_String Iron_text_codepoint_to_utf8(int32_t codepoint) {
    int size = 0;
    const char *buf = CodepointToUTF8((int)codepoint, &size);
    if (!buf || size <= 0) {
        return iron_string_from_literal("", 0);
    }
    return iron_string_from_cstr(buf, (size_t)size);
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Plan 67-03 Task 3: Bulk TEXT-12 — 5 remaining UTF-8 / codepoint shims
 *
 * Both probes (Task 1 = [Int32] RETURN, Task 2 = Iron_String from raylib
 * char*) green first-try. Bulk commits the remainder of TEXT-12:
 *
 *   1. Iron_text_load_utf8        — Pattern 5 caller-must-free variant
 *                                    (LoadUTF8 heap char* -> Iron_String;
 *                                    shim frees via UnloadUTF8 after copy)
 *   2. Iron_text_codepoint_count  — trivial scalar wrap
 *   3. Iron_text_codepoint_at     — Pattern 6 2-tuple (cp, byteSize)
 *   4. Iron_text_codepoint_next   — Pattern 6 2-tuple (cp, byteSize)
 *   5. Iron_text_codepoint_previous — Pattern 6 2-tuple (cp, byteSize)
 *
 * 2-tuple RETURN uses Iron_Tuple_Int32_Int32 (typedef in iron_raylib.h
 * guard block). First primitive-element tuple in the binding — Phase 64-01
 * (Iron_Tuple_Bool_Vector2) + Phase 65-04 (3-tuple) precedent confirms
 * ironc's tuple auto-emit is arity- and element-agnostic.
 * ══════════════════════════════════════════════════════════════════════ */

Iron_String Iron_text_load_utf8(Iron_List_int32_t codepoints) {
    /* LoadUTF8 returns heap char* (RL_MALLOC) that caller must UnloadUTF8.
     * Copy bytes into Iron-owned String THEN free raylib's buffer. */
    char *buf = LoadUTF8((const int *)codepoints.items, (int)codepoints.count);
    Iron_String out = buf ? iron_string_from_cstr(buf, strlen(buf))
                          : iron_string_from_literal("", 0);
    UnloadUTF8(buf);
    return out;
}

int32_t Iron_text_codepoint_count(Iron_String text) {
    return (int32_t)GetCodepointCount(iron_string_cstr(&text));
}

Iron_Tuple_Int32_Int32 Iron_text_codepoint_at(Iron_String text, int32_t offset) {
    int size = 0;
    int cp = GetCodepoint(iron_string_cstr(&text) + offset, &size);
    Iron_Tuple_Int32_Int32 out;
    out.v0 = (int32_t)cp;
    out.v1 = (int32_t)size;
    return out;
}

Iron_Tuple_Int32_Int32 Iron_text_codepoint_next(Iron_String text, int32_t offset) {
    int size = 0;
    int cp = GetCodepointNext(iron_string_cstr(&text) + offset, &size);
    Iron_Tuple_Int32_Int32 out;
    out.v0 = (int32_t)cp;
    out.v1 = (int32_t)size;
    return out;
}

Iron_Tuple_Int32_Int32 Iron_text_codepoint_previous(Iron_String text, int32_t offset) {
    int size = 0;
    int cp = GetCodepointPrevious(iron_string_cstr(&text) + offset, &size);
    Iron_Tuple_Int32_Int32 out;
    out.v0 = (int32_t)cp;
    out.v1 = (int32_t)size;
    return out;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Plan 67-04 Task 1: TEXT-13 string utilities (20 shims)
 *
 * Binds 17 Text.* utilities + 3 TextFormat overloads (20 entry points
 * total). Every string-returning shim uses Pattern 5 (iron_string_from_cstr
 * copy-out) because raylib's Text.* returns point into rotating/shared
 * static char buffers (rtext.c:1820 `MAX_TEXT_BUFFER_LENGTH = 1024`,
 * rotating index mechanism). Pitfalls 2 + 3 (67-RESEARCH.md) mandate
 * immediate copy before control returns to user or to any subsequent
 * raylib call.
 *
 * TextReplace + TextInsert are the two Pattern 5 caller-must-free
 * variants — raylib RL_CALLOC's a fresh buffer; shim copies into
 * Iron_String then free()s (Pitfall 4).
 *
 * Text.append OMITTED: raylib's TextAppend mutates a caller-provided
 * char buffer and advances int *position. Iron Strings are immutable —
 * binding this would require a (String, Int32) tuple wrapping fresh
 * String + new position. Use Text.insert instead for the same semantics
 * with a cleaner API. See 67-RESEARCH.md Open Question 3.
 *
 * Text.copy decision: raylib's TextCopy(char *dst, const char *src)
 * mutates a caller-provided buffer — same immutable-String mismatch as
 * TextAppend. Bound here as a pure functional alternative that returns
 * a fresh Iron String copy of the source, matching what raylib users
 * actually want in most contexts.
 *
 * Iron_text_join + Iron_text_split exercise Iron_List_Iron_String on
 * both sides of the FFI (INPUT for join, RETURN for split). Helpers
 * declared at iron_runtime.h:829 + defined at iron_string.c:434-491.
 * ══════════════════════════════════════════════════════════════════════ */

Iron_String Iron_text_copy(Iron_String source) {
    /* TextCopy mutates a caller-provided char buffer — bind a pure
     * functional alternative instead: return a fresh Iron String copy
     * of the source. Iron Strings are already immutable + copy-on-
     * reference so this matters only at the FFI boundary. */
    const char *src = iron_string_cstr(&source);
    return src ? iron_string_from_cstr(src, strlen(src))
               : iron_string_from_literal("", 0);
}

bool Iron_text_is_equal(Iron_String a, Iron_String b) {
    return (bool)(TextIsEqual(iron_string_cstr(&a), iron_string_cstr(&b)) != 0);
}

int32_t Iron_text_length(Iron_String text) {
    return (int32_t)TextLength(iron_string_cstr(&text));
}

Iron_String Iron_text_format_i(Iron_String fmt, int32_t value) {
    const char *buf = TextFormat(iron_string_cstr(&fmt), (int)value);
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_format_f(Iron_String fmt, float value) {
    /* C promotes float -> double for varargs; %f expects double. */
    const char *buf = TextFormat(iron_string_cstr(&fmt), (double)value);
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_format_s(Iron_String fmt, Iron_String value) {
    const char *buf = TextFormat(iron_string_cstr(&fmt), iron_string_cstr(&value));
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_subtext(Iron_String text, int32_t position, int32_t length) {
    const char *buf = TextSubtext(iron_string_cstr(&text), (int)position, (int)length);
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_replace(Iron_String text, Iron_String replace, Iron_String by) {
    /* Pitfall 4: caller MUST free TextReplace's return (RL_CALLOC). */
    char *buf = TextReplace(iron_string_cstr(&text),
                             iron_string_cstr(&replace),
                             iron_string_cstr(&by));
    Iron_String out = buf ? iron_string_from_cstr(buf, strlen(buf))
                          : iron_string_from_literal("", 0);
    free(buf);
    return out;
}

Iron_String Iron_text_insert(Iron_String text, Iron_String insert_text, int32_t position) {
    /* Pitfall 4: caller MUST free TextInsert's return (RL_CALLOC). */
    char *buf = TextInsert(iron_string_cstr(&text),
                            iron_string_cstr(&insert_text),
                            (int)position);
    Iron_String out = buf ? iron_string_from_cstr(buf, strlen(buf))
                          : iron_string_from_literal("", 0);
    free(buf);
    return out;
}

Iron_String Iron_text_join(Iron_List_Iron_String parts, Iron_String delimiter) {
    /* raylib TextJoin takes `const char **textList, int count, const char
     * *delimiter` and returns a pointer into its own static buffer (no
     * free needed). Build a transient const char ** from the Iron_String
     * list, call TextJoin, copy the result into a fresh Iron String. */
    int32_t count = (int32_t)Iron_List_Iron_String_len(&parts);
    if (count <= 0) {
        return iron_string_from_literal("", 0);
    }
    const char **c_parts = (const char **)calloc((size_t)count, sizeof(const char *));
    if (!c_parts) {
        return iron_string_from_literal("", 0);
    }
    /* Materialize all cstr pointers first — every iron_string_cstr call
     * reads from the Iron_String struct by reference, and the Iron_Strings
     * live in the list's items array for the duration of this shim. */
    for (int32_t i = 0; i < count; i++) {
        Iron_String p = Iron_List_Iron_String_get(&parts, (int64_t)i);
        c_parts[i] = iron_string_cstr(&p);
    }
    const char *joined = TextJoin(c_parts, (int)count, iron_string_cstr(&delimiter));
    Iron_String out = joined ? iron_string_from_cstr(joined, strlen(joined))
                             : iron_string_from_literal("", 0);
    free(c_parts);
    return out;
}

Iron_List_Iron_String Iron_text_split(Iron_String text, Iron_String delimiter) {
    /* raylib TextSplit uses internal static char buffers + a static
     * const char ** result array — no free needed. Copy each part into
     * Iron_List_Iron_String before returning. Delimiter is a single
     * char (raylib signature is `char delimiter`); if the Iron caller
     * passes a multi-byte String, only the first byte is used. */
    int count = 0;
    const char *d = iron_string_cstr(&delimiter);
    char delim = (d && d[0]) ? d[0] : ',';
    const char **parts = TextSplit(iron_string_cstr(&text), delim, &count);
    Iron_List_Iron_String out = Iron_List_Iron_String_create();
    for (int i = 0; i < count; i++) {
        Iron_String part = parts[i]
            ? iron_string_from_cstr(parts[i], strlen(parts[i]))
            : iron_string_from_literal("", 0);
        Iron_List_Iron_String_push(&out, part);
    }
    return out;
}

int32_t Iron_text_find_index(Iron_String text, Iron_String find) {
    return (int32_t)TextFindIndex(iron_string_cstr(&text), iron_string_cstr(&find));
}

Iron_String Iron_text_to_upper(Iron_String text) {
    const char *buf = TextToUpper(iron_string_cstr(&text));
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_to_lower(Iron_String text) {
    const char *buf = TextToLower(iron_string_cstr(&text));
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_to_pascal(Iron_String text) {
    const char *buf = TextToPascal(iron_string_cstr(&text));
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_to_snake(Iron_String text) {
    const char *buf = TextToSnake(iron_string_cstr(&text));
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

Iron_String Iron_text_to_camel(Iron_String text) {
    const char *buf = TextToCamel(iron_string_cstr(&text));
    return buf ? iron_string_from_cstr(buf, strlen(buf))
               : iron_string_from_literal("", 0);
}

int32_t Iron_text_to_integer(Iron_String text) {
    return (int32_t)TextToInteger(iron_string_cstr(&text));
}

float Iron_text_to_float(Iron_String text) {
    return TextToFloat(iron_string_cstr(&text));
}

/* ── Audio (Phase 68) ─────────────────────────────────────────────── */
/* Plan 68-01 Task 3: ABI-CALLBACK trampoline registry (16 slots).
 *
 * raylib's AudioCallback = void(*)(void *bufferData, unsigned int frames).
 * Iron closures have ABI { void *env; void (*fn)(void *); }.  We bridge
 * via a pool of 16 static entry functions (audio_cb_0..audio_cb_15), each
 * dispatching to g_audio_cb[slot] with (env, buffer, frames).
 *
 * Slot count = 16 is conservative.  raylib AttachAudioStreamProcessor
 * is typically used for 1-2 processors per stream; overflow returns
 * slot = -1 (silent failure — callback not attached).
 *
 * Thread safety: raylib invokes the callback from miniaudio's audio
 * thread.  Iron closure bodies MUST be audio-thread-safe (no Iron heap
 * allocation, no GC, no blocking).  This constraint is documented in
 * raylib.iron's header comment above stream.set_callback in Plan 68-05.
 *
 * Detach semantics: stream.detach_processor / Audio.detach_mixed_processor
 * free ALL slots associated with the given Iron stream / mixer rather
 * than matching a specific Iron_Closure (detach-all per stream).  raylib's
 * DetachAudioStreamProcessor matches the C function pointer, not the
 * closure — Iron users passing the same lambda twice would create two
 * slots with the same Iron_Closure, and matching-by-closure is ambiguous.
 * Detach-all is simpler and matches 99% of real usage (users typically
 * detach AFTER stopping audio, not selectively). */

#define IRON_AUDIO_CB_SLOTS 16

static Iron_Closure g_audio_cb[IRON_AUDIO_CB_SLOTS];
static bool         g_audio_cb_used[IRON_AUDIO_CB_SLOTS];

/* Cast closure fn to the 3-arg signature (env, buffer, frames) and invoke.
 * Uses memcpy through a typed function pointer to avoid
 * -Wcast-function-type-mismatch (same pattern as
 * IRON_LIST_COLL_IMPL in iron_runtime.h:545 — "to avoid
 * -Wcast-function-type-mismatch"). */
typedef void (*Iron_AudioCallback_Fn)(void *, void *, unsigned int);
static void audio_cb_slot_invoke(int slot, void *buf, unsigned int frames) {
    Iron_Closure c = g_audio_cb[slot];
    if (!c.fn) return;
    Iron_AudioCallback_Fn cb_fn;
    memcpy(&cb_fn, &c.fn, sizeof(cb_fn));
    cb_fn(c.env, buf, frames);
}

/* One static entry function per slot — raylib takes a plain fn pointer. */
static void audio_cb_0 (void *buf, unsigned int n) { audio_cb_slot_invoke(0,  buf, n); }
static void audio_cb_1 (void *buf, unsigned int n) { audio_cb_slot_invoke(1,  buf, n); }
static void audio_cb_2 (void *buf, unsigned int n) { audio_cb_slot_invoke(2,  buf, n); }
static void audio_cb_3 (void *buf, unsigned int n) { audio_cb_slot_invoke(3,  buf, n); }
static void audio_cb_4 (void *buf, unsigned int n) { audio_cb_slot_invoke(4,  buf, n); }
static void audio_cb_5 (void *buf, unsigned int n) { audio_cb_slot_invoke(5,  buf, n); }
static void audio_cb_6 (void *buf, unsigned int n) { audio_cb_slot_invoke(6,  buf, n); }
static void audio_cb_7 (void *buf, unsigned int n) { audio_cb_slot_invoke(7,  buf, n); }
static void audio_cb_8 (void *buf, unsigned int n) { audio_cb_slot_invoke(8,  buf, n); }
static void audio_cb_9 (void *buf, unsigned int n) { audio_cb_slot_invoke(9,  buf, n); }
static void audio_cb_10(void *buf, unsigned int n) { audio_cb_slot_invoke(10, buf, n); }
static void audio_cb_11(void *buf, unsigned int n) { audio_cb_slot_invoke(11, buf, n); }
static void audio_cb_12(void *buf, unsigned int n) { audio_cb_slot_invoke(12, buf, n); }
static void audio_cb_13(void *buf, unsigned int n) { audio_cb_slot_invoke(13, buf, n); }
static void audio_cb_14(void *buf, unsigned int n) { audio_cb_slot_invoke(14, buf, n); }
static void audio_cb_15(void *buf, unsigned int n) { audio_cb_slot_invoke(15, buf, n); }

/* Fixed table of entry pointers — index by slot id.  Matches raylib's
 * AudioCallback typedef (void(*)(void *, unsigned int)).  Referenced by
 * Plan 68-05's stream.set_callback / attach_processor shims.
 * __attribute__((unused)) silences -Wunused-variable until Plan 68-05 lands. */
__attribute__((unused))
static AudioCallback g_audio_cb_fns[IRON_AUDIO_CB_SLOTS] = {
    audio_cb_0,  audio_cb_1,  audio_cb_2,  audio_cb_3,
    audio_cb_4,  audio_cb_5,  audio_cb_6,  audio_cb_7,
    audio_cb_8,  audio_cb_9,  audio_cb_10, audio_cb_11,
    audio_cb_12, audio_cb_13, audio_cb_14, audio_cb_15,
};

/* Allocate a trampoline slot for the given Iron closure.
 * Returns slot index on success, -1 on overflow.  Plan 68-05 uses this
 * from Iron_stream_set_callback and the 4 processor shims.
 * __attribute__((unused)) silences -Wunused-function until Plan 68-05. */
__attribute__((unused))
static int audio_cb_alloc(Iron_Closure c) {
    for (int i = 0; i < IRON_AUDIO_CB_SLOTS; i++) {
        if (!g_audio_cb_used[i]) {
            g_audio_cb[i] = c;
            g_audio_cb_used[i] = true;
            return i;
        }
    }
    return -1;  /* overflow — caller should log and no-op */
}

/* Free a trampoline slot.  Safe for slot = -1 (no-op).
 * __attribute__((unused)) silences -Wunused-function until Plan 68-05. */
__attribute__((unused))
static void audio_cb_free(int slot) {
    if (slot >= 0 && slot < IRON_AUDIO_CB_SLOTS) {
        g_audio_cb_used[slot] = false;
        g_audio_cb[slot] = (Iron_Closure){0};
    }
}

/* audio_cb_alloc / audio_cb_free / g_audio_cb_fns are currently unreferenced —
 * Plan 68-05 consumes them via the 5 AUDIO-12 callback shims.  Until then,
 * -Wunused-function would warn; prefix with __attribute__((unused)) if the
 * project's warning flags are strict (see below). */

/* ── End of ABI-CALLBACK trampoline infrastructure ─────────────────── */
/* Plan 68-02+ will add raudio binding shims after this block. */

/* ── AUDIO-01 Audio device lifecycle (5 shims) ─────────────────────── */
/*
 * Freestanding shims — no instance receiver. Mirrors Phase 61 Window.*
 * shape (e.g., Iron_window_init / Iron_window_close).
 *
 * Pitfall 7 (68-RESEARCH.md): InitAudioDevice logs a TRACELOG warning
 * on backend failure but does NOT crash. Subsequent play / update calls
 * are silent no-ops when !IsAudioDeviceReady(). Iron users guard with
 * `if Audio.is_ready() { ... }` — the smoke test (Plan 68-05) does
 * this for graceful headless / CI degradation.
 */

void Iron_audio_init(void) {
    InitAudioDevice();
}

void Iron_audio_close(void) {
    CloseAudioDevice();
}

bool Iron_audio_is_ready(void) {
    return (bool)(IsAudioDeviceReady() != 0);
}

void Iron_audio_set_master_volume(float volume) {
    SetMasterVolume(volume);
}

float Iron_audio_get_master_volume(void) {
    return GetMasterVolume();
}

/* ── AUDIO-02 Wave load/unload + AUDIO-04 export (6 shims) ─────────── */
/*
 * Wave is 24 B (4 x uint32_t + void *_data). Struct-by-value in+out
 * uses Phase 66-04 RenderTexture template: memcpy into Wave local for
 * INPUT-by-value shims, memcpy out of Wave local for RETURN-by-value.
 *
 * ABI-UINT8 consumer: Wave.load_from_memory takes [UInt8] data —
 * first real consumer of Plan 68-01 Task 1's IRON_LIST_DECL(uint8_t,
 * uint8_t). Shim forwards file_data.items directly (already uint8_t *).
 *
 * Note: LoadWave returns Wave with frameCount=0 on failure + prints
 * TRACELOG warning. Users MUST call wave.is_valid() before wave.to_sound().
 */

struct Iron_Wave Iron_wave_load(Iron_String file_name) {
    Wave rl = LoadWave(iron_string_cstr(&file_name));
    struct Iron_Wave out;
    memcpy(&out, &rl, sizeof(struct Iron_Wave));
    return out;
}

struct Iron_Wave Iron_wave_load_from_memory(Iron_String file_type,
                                             Iron_List_uint8_t file_data,
                                             int32_t data_size) {
    /* Pass Iron_List_uint8_t.items directly — raylib takes
     * `const unsigned char *fileData`. items is already uint8_t *. */
    Wave rl = LoadWaveFromMemory(iron_string_cstr(&file_type),
                                  file_data.items,
                                  (int)data_size);
    struct Iron_Wave out;
    memcpy(&out, &rl, sizeof(struct Iron_Wave));
    return out;
}

bool Iron_wave_is_valid(struct Iron_Wave wave) {
    Wave rl;
    memcpy(&rl, &wave, sizeof(Wave));
    return (bool)(IsWaveValid(rl) != 0);
}

void Iron_wave_unload(struct Iron_Wave wave) {
    Wave rl;
    memcpy(&rl, &wave, sizeof(Wave));
    UnloadWave(rl);
}

bool Iron_wave_export(struct Iron_Wave wave, Iron_String file_name) {
    Wave rl;
    memcpy(&rl, &wave, sizeof(Wave));
    return (bool)(ExportWave(rl, iron_string_cstr(&file_name)) != 0);
}

bool Iron_wave_export_as_code(struct Iron_Wave wave, Iron_String file_name) {
    Wave rl;
    memcpy(&rl, &wave, sizeof(Wave));
    return (bool)(ExportWaveAsCode(rl, iron_string_cstr(&file_name)) != 0);
}

/* ── AUDIO-03 Wave manipulation (4 shims; wave.to_sound in 68-03) ──── */
/*
 * Pattern 1: WaveCopy returns Wave by value. Shim memcpys in+out.
 * Pattern 2 (mutating-return-by-value, Phase 66-03 template):
 *   WaveCrop + WaveFormat take `Wave *wave` and mutate in place.
 *   Shim: memcpy INPUT to local, call raylib with &local, memcpy
 *   OUT to return slot. Iron sees a fresh Wave by value.
 * Pattern 3 (ABI-FLOAT32 RETURN, Plan 68-01 consumer):
 *   LoadWaveSamples returns raylib-owned float*. Shim deep-copies
 *   into Iron_List_float via _create_with_capacity + _push
 *   loop, then calls UnloadWaveSamples to release raylib's buffer.
 *   No user-facing unload-samples method — lifecycle owned by shim.
 *
 * Naming note (Plan 68-01 SUMMARY key finding #2): the monomorphized
 * List type suffix is `float`, not `Iron_Float32` — matches Iron's
 * emit_type_to_c output (Float32 -> "float"). Helpers are
 * Iron_List_float_create_with_capacity + Iron_List_float_push.
 */

struct Iron_Wave Iron_wave_copy(struct Iron_Wave wave) {
    Wave rl;
    memcpy(&rl, &wave, sizeof(Wave));
    Wave dup = WaveCopy(rl);
    struct Iron_Wave out;
    memcpy(&out, &dup, sizeof(struct Iron_Wave));
    return out;
}

struct Iron_Wave Iron_wave_crop(struct Iron_Wave wave, int32_t init_frame, int32_t final_frame) {
    /* Phase 66-03 mutating-return-by-value template. raylib prints
     * TRACELOG warning on out-of-range inputs but doesn't fail;
     * we return the (possibly unchanged) Wave — same as Image.crop. */
    Wave local;
    memcpy(&local, &wave, sizeof(Wave));
    WaveCrop(&local, (int)init_frame, (int)final_frame);
    struct Iron_Wave out;
    memcpy(&out, &local, sizeof(struct Iron_Wave));
    return out;
}

struct Iron_Wave Iron_wave_format(struct Iron_Wave wave, int32_t sample_rate, int32_t sample_size, int32_t channels) {
    /* Same Phase 66-03 mutating-return-by-value shape. */
    Wave local;
    memcpy(&local, &wave, sizeof(Wave));
    WaveFormat(&local, (int)sample_rate, (int)sample_size, (int)channels);
    struct Iron_Wave out;
    memcpy(&out, &local, sizeof(struct Iron_Wave));
    return out;
}

Iron_List_float Iron_wave_load_samples(struct Iron_Wave wave) {
    /* Plan 68-01 ABI-FLOAT32 RETURN consumer. Deep-copies raylib's
     * float* into Iron-owned Iron_List_float, then calls
     * UnloadWaveSamples so users never see the raw pointer.
     * Length = frameCount × channels (raylib convention). */
    Wave w;
    memcpy(&w, &wave, sizeof(Wave));
    float *raw = LoadWaveSamples(w);
    int64_t n = (int64_t)(w.frameCount * w.channels);
    Iron_List_float out = Iron_List_float_create_with_capacity(n);
    if (raw) {
        for (int64_t i = 0; i < n; i++) {
            Iron_List_float_push(&out, raw[i]);
        }
        UnloadWaveSamples(raw);
    }
    return out;
}

/* ── AUDIO-05 Sound load/unload + alias (6 shims) ──────────────────── */
/*
 * Sound is 32 B (embeds AudioStream by value + frameCount). Well under
 * the Phase 64 validated 120 B struct-by-value ceiling. All shims use
 * the INPUT-by-value memcpy template established in Phase 61 (Window)
 * and Phase 66 (Image/Texture).
 *
 * Alias ownership discipline (CONTEXT.md line 103-107 + header above):
 *   - Sound.alias(source) produces a Sound sharing source's samples.
 *   - sound.unload() frees sample data — all aliases then dangle.
 *   - alias.unload_alias() releases only the AudioStream envelope;
 *     sample data survives until the source's sound.unload().
 *   - Users MUST keep the source Sound alive until every alias is
 *     released via alias.unload_alias(). This is surfaced as two
 *     distinct methods on the same Iron Sound type — no runtime tag,
 *     no silent dispatch.
 */

struct Iron_Sound Iron_sound_load(Iron_String file_name) {
    Sound rl = LoadSound(iron_string_cstr(&file_name));
    struct Iron_Sound out;
    memcpy(&out, &rl, sizeof(struct Iron_Sound));
    return out;
}

struct Iron_Sound Iron_sound_from_wave(struct Iron_Wave wave) {
    /* Shared with AUDIO-03 wave.to_sound — raylib.iron exposes both
     * entry points (Sound.from_wave + wave.to_sound); ironc mangles
     * them to distinct C symbols (Path B in the plan's aliasing
     * strategy), so Iron_wave_to_sound below forwards here. */
    Wave w;
    memcpy(&w, &wave, sizeof(Wave));
    Sound rl = LoadSoundFromWave(w);
    struct Iron_Sound out;
    memcpy(&out, &rl, sizeof(struct Iron_Sound));
    return out;
}

struct Iron_Sound Iron_sound_alias(struct Iron_Sound source) {
    /* LoadSoundAlias shares the source's sample data. The returned
     * Sound does NOT own samples — call alias.unload_alias() to
     * release the audio-stream envelope only. The source must
     * outlive every alias, or aliases dangle. */
    Sound rl;
    memcpy(&rl, &source, sizeof(Sound));
    Sound dup = LoadSoundAlias(rl);
    struct Iron_Sound out;
    memcpy(&out, &dup, sizeof(struct Iron_Sound));
    return out;
}

bool Iron_sound_is_valid(struct Iron_Sound sound) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    return (bool)(IsSoundValid(rl) != 0);
}

void Iron_sound_unload(struct Iron_Sound sound) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    UnloadSound(rl);
}

void Iron_sound_unload_alias(struct Iron_Sound alias) {
    Sound rl;
    memcpy(&rl, &alias, sizeof(Sound));
    UnloadSoundAlias(rl);
}

/* ── AUDIO-03 bridge: wave.to_sound() forwards to Iron_sound_from_wave */
/*
 * Pattern-2 forwarding stub. Iron exposes both wave.to_sound() and
 * Sound.from_wave(wave) as alias entry points; ironc name-mangles
 * them to distinct C symbols (Iron_wave_to_sound vs
 * Iron_sound_from_wave). Keeping one shim body avoids divergent
 * semantics; the forwarder is a pure call-through.
 */
struct Iron_Sound Iron_wave_to_sound(struct Iron_Wave wave) {
    return Iron_sound_from_wave(wave);
}

/* ── AUDIO-06 Sound management (5 shims) ───────────────────────────── */
/*
 * All 5 methods follow the Phase 61 INPUT-by-value memcpy template:
 * memcpy Iron_Sound → Sound local, invoke raylib, optional Bool
 * coercion via (bool)(... != 0). No return-slot memcpy needed (4 void
 * returns + 1 bool return).
 */

void Iron_sound_play(struct Iron_Sound sound) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    PlaySound(rl);
}

void Iron_sound_stop(struct Iron_Sound sound) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    StopSound(rl);
}

void Iron_sound_pause(struct Iron_Sound sound) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    PauseSound(rl);
}

void Iron_sound_resume(struct Iron_Sound sound) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    ResumeSound(rl);
}

bool Iron_sound_is_playing(struct Iron_Sound sound) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    return (bool)(IsSoundPlaying(rl) != 0);
}

/* ── AUDIO-07 Sound configure (3 shims) ────────────────────────────── */
/*
 * raylib volume/pitch/pan are float (Iron Float32). SetSoundPan takes
 * 0.0..1.0 (0=full-left, 0.5=centre, 1=full-right). Out-of-range
 * values are clamped internally by raylib — no Iron-side validation.
 */

void Iron_sound_set_volume(struct Iron_Sound sound, float volume) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    SetSoundVolume(rl, volume);
}

void Iron_sound_set_pitch(struct Iron_Sound sound, float pitch) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    SetSoundPitch(rl, pitch);
}

void Iron_sound_set_pan(struct Iron_Sound sound, float pan) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    SetSoundPan(rl, pan);
}

/* ── AUDIO-08 Sound update (1 shim, ABI-FLOAT32 INPUT) ─────────────── */
/*
 * First live ABI-FLOAT32 INPUT consumer. raylib's UpdateSound
 * signature is `void(Sound, const void *, int)`.
 * Iron_List_float.items is already `float *` — pass directly as
 * `const void *` (C implicitly converts any object pointer to
 * `void *`; no cast needed).
 *
 * Sample count semantics (raudio.c): sampleCount is the number of
 * individual samples (NOT frames). For stereo 16-bit:
 *   bytes_to_write = sampleCount × sizeof(short) × 2    -- 16-bit
 *   bytes_to_write = sampleCount × sizeof(float)        -- 32-bit
 * raylib internally reinterprets based on sound.stream.sampleSize.
 *
 * Iron_List element-type mangling: the list type is `Iron_List_float`
 * (emit_type_to_c output for Float32 = "float"). Plan 68-02 SUMMARY
 * key finding #2 + Rule 3 Blocking deviation established this. Do
 * NOT use `Iron_List_Iron_Float32` — that symbol does not exist.
 */

void Iron_sound_update(struct Iron_Sound sound,
                        Iron_List_float data,
                        int32_t sample_count) {
    Sound rl;
    memcpy(&rl, &sound, sizeof(Sound));
    UpdateSound(rl, data.items, (int)sample_count);
}

/* ── AUDIO-09 Music load/unload (4 shims) ──────────────────────────── */
/*
 * Music is the largest audio struct (48 B = AudioStream 24 B + uint32
 * frameCount + bool looping + int ctxType + void *_ctxData). Well
 * under Phase 64's validated 120 B struct-by-value FFI ceiling.
 *
 * 2nd live ABI-UINT8 INPUT consumer after Plan 68-02
 * Wave.load_from_memory — same Iron_List_uint8_t pattern. Confirms the
 * abi-uint8 probe (Plan 68-01) works for multiple distinct type
 * contexts.
 *
 * Failure semantics: LoadMusicStream sets ctxType = MUSIC_AUDIO_NONE
 * (0) and _ctxData = NULL when the file cannot be loaded, and prints
 * a TRACELOG warning. Callers MUST guard with music.is_valid() before
 * play/update. IsMusicValid returns false when ctxType == NONE or
 * _ctxData is NULL.
 */

struct Iron_Music Iron_music_load(Iron_String file_name) {
    Music rl = LoadMusicStream(iron_string_cstr(&file_name));
    struct Iron_Music out;
    memcpy(&out, &rl, sizeof(struct Iron_Music));
    return out;
}

struct Iron_Music Iron_music_load_from_memory(Iron_String file_type,
                                                Iron_List_uint8_t data,
                                                int32_t data_size) {
    Music rl = LoadMusicStreamFromMemory(iron_string_cstr(&file_type),
                                          data.items,
                                          (int)data_size);
    struct Iron_Music out;
    memcpy(&out, &rl, sizeof(struct Iron_Music));
    return out;
}

bool Iron_music_is_valid(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    return (bool)(IsMusicValid(rl) != 0);
}

void Iron_music_unload(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    UnloadMusicStream(rl);
}

/*
 * Music.set_looping — val-field write fallback, mutating-return-by-
 * value template (Phase 66-03 applied to audio). Source analysis of
 * src/analyzer/typecheck.c:2758 shows val-immutability is enforced
 * ONLY on IRON_NODE_IDENT targets (local variables), NOT on
 * IRON_NODE_FIELD_ACCESS targets — so `m.looping = true` would
 * likely compile after the next ironc rebuild. This setter exists
 * as the defensive portable form: works under current ironc, works
 * after a rebuild, and makes the "return a fresh Music" semantics
 * explicit at call sites. Iron use:
 *
 *     val m2 = m.set_looping(true)
 *     m2.update()
 */
struct Iron_Music Iron_music_set_looping(struct Iron_Music music, bool looping) {
    Music local;
    memcpy(&local, &music, sizeof(Music));
    local.looping = looping;
    struct Iron_Music out;
    memcpy(&out, &local, sizeof(struct Iron_Music));
    return out;
}

/* ── AUDIO-10 Music management (6 shims) ───────────────────────────── */
/*
 * INPUT-by-value memcpy template (Phase 61 / Plan 68-02..03 precedent).
 * All six shims follow the same shape: memcpy Iron_Music → local Music,
 * dispatch to raylib with the local, return is void (or Bool for the
 * is_playing predicate).
 *
 * Naming asymmetry: raylib uses IsMusicStreamPlaying (with Stream
 * infix) but IsMusicValid (without). Iron flattens both to
 * music.is_valid + music.is_playing. This shim binds is_playing to
 * the with-Stream raylib function; the is_valid shim (Iron_music_is_valid
 * above) binds to the without-Stream raylib function. Iron surface
 * stays symmetric.
 *
 * music.update must be called every frame from the user's main loop
 * to feed raylib's audio buffer. raylib's internals are no-ops when
 * the audio device is not ready, so guarding with Audio.is_ready() is
 * optional but recommended for clarity in headless environments.
 */

void Iron_music_play(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    PlayMusicStream(rl);
}

bool Iron_music_is_playing(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    return (bool)(IsMusicStreamPlaying(rl) != 0);
}

void Iron_music_update(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    UpdateMusicStream(rl);
}

void Iron_music_stop(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    StopMusicStream(rl);
}

void Iron_music_pause(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    PauseMusicStream(rl);
}

void Iron_music_resume(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    ResumeMusicStream(rl);
}

/* ── AUDIO-11 Music configure + query (6 shims) ────────────────────── */
/*
 * All parameters and returns are raylib's float == Iron's Float32.
 * seek position, get_time_length, get_time_played are in seconds.
 * volume and pan are in the 0..1 range (clamped inside raylib).
 * pitch is a multiplier relative to the track's native playback speed
 * (1.0 = original, 2.0 = double speed, 0.5 = half speed).
 *
 * Same INPUT-by-value memcpy template as AUDIO-10 management block.
 */

void Iron_music_seek(struct Iron_Music music, float position) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    SeekMusicStream(rl, position);
}

void Iron_music_set_volume(struct Iron_Music music, float volume) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    SetMusicVolume(rl, volume);
}

void Iron_music_set_pitch(struct Iron_Music music, float pitch) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    SetMusicPitch(rl, pitch);
}

void Iron_music_set_pan(struct Iron_Music music, float pan) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    SetMusicPan(rl, pan);
}

float Iron_music_get_time_length(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    return GetMusicTimeLength(rl);
}

float Iron_music_get_time_played(struct Iron_Music music) {
    Music rl;
    memcpy(&rl, &music, sizeof(Music));
    return GetMusicTimePlayed(rl);
}

/* ── AUDIO-12 AudioStream standard (14 shims; 5 callback shims below) ─ */
/*
 * 24 B AudioStream struct-by-value crossing. memcpy INPUT (Iron_AudioStream
 * → raylib AudioStream) before every raylib call; memcpy OUTPUT on load.
 * 2nd live ABI-FLOAT32 INPUT consumer (stream.update) — shim forwards
 * data.items (float *) as raylib's `const void *` (UpdateAudioStream
 * reinterprets based on stream.sampleSize).  frame_count is the number
 * of FRAMES (not samples — interleaved for multi-channel audio).
 *
 * ironc mangles Iron methods `AudioStream.X(stream: AudioStream, ...)` as
 * Iron_audiostream_X (lowercase namespace — matches existing Font.*,
 * Image.*, Texture.* precedent).
 */

struct Iron_AudioStream Iron_audiostream_load(uint32_t sample_rate,
                                               uint32_t sample_size,
                                               uint32_t channels) {
    AudioStream rl = LoadAudioStream((unsigned int)sample_rate,
                                      (unsigned int)sample_size,
                                      (unsigned int)channels);
    struct Iron_AudioStream out;
    memcpy(&out, &rl, sizeof(struct Iron_AudioStream));
    return out;
}

bool Iron_audiostream_is_valid(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    return (bool)(IsAudioStreamValid(rl) != 0);
}

void Iron_audiostream_unload(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    UnloadAudioStream(rl);
}

void Iron_audiostream_update(struct Iron_AudioStream stream,
                              Iron_List_float data,
                              int32_t frame_count) {
    /* 2nd live ABI-FLOAT32 INPUT consumer after sound.update (Plan 68-03).
     * frame_count is the number of FRAMES (interleaved for multi-channel). */
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    UpdateAudioStream(rl, data.items, (int)frame_count);
}

bool Iron_audiostream_is_processed(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    return (bool)(IsAudioStreamProcessed(rl) != 0);
}

void Iron_audiostream_play(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    PlayAudioStream(rl);
}

void Iron_audiostream_pause(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    PauseAudioStream(rl);
}

void Iron_audiostream_resume(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    ResumeAudioStream(rl);
}

bool Iron_audiostream_is_playing(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    return (bool)(IsAudioStreamPlaying(rl) != 0);
}

void Iron_audiostream_stop(struct Iron_AudioStream stream) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    StopAudioStream(rl);
}

void Iron_audiostream_set_volume(struct Iron_AudioStream stream, float volume) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    SetAudioStreamVolume(rl, volume);
}

void Iron_audiostream_set_pitch(struct Iron_AudioStream stream, float pitch) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    SetAudioStreamPitch(rl, pitch);
}

void Iron_audiostream_set_pan(struct Iron_AudioStream stream, float pan) {
    AudioStream rl;
    memcpy(&rl, &stream, sizeof(AudioStream));
    SetAudioStreamPan(rl, pan);
}

void Iron_audiostream_set_buffer_size_default(int32_t size) {
    SetAudioStreamBufferSizeDefault((int)size);
}

/* ── 3D Drawing (Phase 69) ────────────────────────────────────────── */

/* DRAW3D-01: 3D draw-mode stack — raylib.h:1032-1033.
 * Parallels Phase 63's Iron_draw_begin_mode_2d at line 595. Note that
 * raylib's `Camera3D camera` parameter uses the typedef Camera3D;
 * no relation to the Iron `Camera` type (which is 2D, 16 B). */
void Iron_draw_begin_mode_3d(struct Iron_Camera3D camera) {
    Camera3D rl;
    memcpy(&rl, &camera, sizeof(Camera3D));
    BeginMode3D(rl);
}

void Iron_draw_end_mode_3d(void) { EndMode3D(); }

/* DRAW3D-02: Camera3D update — raylib.h:1233-1234.
 * Mutating-return-by-value template (Phase 66-03 Iron_image_crop at
 * iron_raylib.c:3121-3130; Phase 68-02 Iron_wave_crop at line 4719-4729).
 * raylib's `Camera *camera` is a `Camera3D *` at runtime (typedef
 * alias at raylib.h:334). Iron's val-declared Camera3D fields are
 * immutable, so the shim copies into a local, raylib mutates, then
 * we memcpy back out.
 *
 * CAMERA_CUSTOM (mode == 0) is a no-op in raylib's UpdateCamera
 * (documented in rcamera.c); returned Camera3D is bit-identical to
 * the input. Users in .custom mode should mutate Camera3D fields
 * directly via val re-binding and skip this call. */
struct Iron_Camera3D Iron_camera3d_update(struct Iron_Camera3D camera, int32_t mode) {
    Camera3D local;
    memcpy(&local, &camera, sizeof(Camera3D));
    UpdateCamera(&local, (int)mode);
    struct Iron_Camera3D out;
    memcpy(&out, &local, sizeof(struct Iron_Camera3D));
    return out;
}

struct Iron_Camera3D Iron_camera3d_update_pro(struct Iron_Camera3D camera,
                                               struct Iron_Vector3 movement,
                                               struct Iron_Vector3 rotation,
                                               float zoom) {
    Camera3D local;
    Vector3  m, r;
    memcpy(&local, &camera,   sizeof(Camera3D));
    memcpy(&m,     &movement, sizeof(Vector3));
    memcpy(&r,     &rotation, sizeof(Vector3));
    UpdateCameraPro(&local, m, r, zoom);
    struct Iron_Camera3D out;
    memcpy(&out, &local, sizeof(struct Iron_Camera3D));
    return out;
}

/* DRAW3D-03: Screen↔world + view matrix — raylib.h:1063-1070.
 * First Ray-by-value RETURN shim in Phase 69 (24 B). First Matrix-64B
 * return outside the raymath section (reuses Phase 65-03 template —
 * Iron_matrix_look_at at line 2360-2369). raylib's Camera typedef is
 * Camera3D at runtime; the local raylib-typed variable uses the full
 * Camera3D name for clarity per Pitfall 1. */

struct Iron_Ray Iron_camera3d_screen_to_world_ray(struct Iron_Camera3D camera,
                                                   struct Iron_Vector2 position) {
    Camera3D cam;
    Vector2  p;
    memcpy(&cam, &camera,   sizeof(Camera3D));
    memcpy(&p,   &position, sizeof(Vector2));
    Ray r = GetScreenToWorldRay(p, cam);
    struct Iron_Ray out;
    memcpy(&out, &r, sizeof(Ray));
    return out;
}

struct Iron_Ray Iron_camera3d_screen_to_world_ray_ex(struct Iron_Camera3D camera,
                                                      struct Iron_Vector2 position,
                                                      int32_t width, int32_t height) {
    Camera3D cam;
    Vector2  p;
    memcpy(&cam, &camera,   sizeof(Camera3D));
    memcpy(&p,   &position, sizeof(Vector2));
    Ray r = GetScreenToWorldRayEx(p, cam, (int)width, (int)height);
    struct Iron_Ray out;
    memcpy(&out, &r, sizeof(Ray));
    return out;
}

struct Iron_Vector2 Iron_camera3d_world_to_screen(struct Iron_Camera3D camera,
                                                   struct Iron_Vector3 position) {
    Camera3D cam;
    Vector3  p;
    memcpy(&cam, &camera,   sizeof(Camera3D));
    memcpy(&p,   &position, sizeof(Vector3));
    Vector2 r = GetWorldToScreen(p, cam);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Vector2 Iron_camera3d_world_to_screen_ex(struct Iron_Camera3D camera,
                                                      struct Iron_Vector3 position,
                                                      int32_t width, int32_t height) {
    Camera3D cam;
    Vector3  p;
    memcpy(&cam, &camera,   sizeof(Camera3D));
    memcpy(&p,   &position, sizeof(Vector3));
    Vector2 r = GetWorldToScreenEx(p, cam, (int)width, (int)height);
    struct Iron_Vector2 out;
    memcpy(&out, &r, sizeof(Vector2));
    return out;
}

struct Iron_Matrix Iron_camera3d_matrix(struct Iron_Camera3D camera) {
    Camera3D cam;
    memcpy(&cam, &camera, sizeof(Camera3D));
    Matrix r = GetCameraMatrix(cam);
    struct Iron_Matrix out;
    memcpy(&out, &r, sizeof(Matrix));
    return out;
}

/* DRAW3D-04 batch 1: line / point / circle / triangle / triangle-strip
 * / cube (4 variants) / sphere (3 variants) — raylib.h:1526-1537.
 *
 * Every shim is a direct variant of Phase 63 Template F (Vector-in +
 * Color). The [Vector3] array shim for triangle_strip_3d reuses the
 * Phase 63-04 Branch B pattern (iron_raylib.c:941-951) with
 * element-type substitution Vector2 -> Vector3. The Iron_List_Iron_Vector3
 * typedef lives in iron_raylib.h under the IRON_LIST_IRON_VECTOR3_STRUCT_
 * DEFINED guard (mirrors the Vector2 precedent at iron_raylib.h:604-611).
 *
 * Pitfall 3: radius / width / height / length / rotation_angle arrive
 * as `float` from ironc (emit_type_to_c lowers Float32 -> "float").
 * Pitfall 5: rings / slices / sides arrive as int32_t; cast to raylib's
 * `int` at the FFI boundary.
 */

void Iron_draw_line_3d(struct Iron_Vector3 start, struct Iron_Vector3 end,
                       struct Iron_Color color) {
    Vector3 s, e;
    Color c;
    memcpy(&s, &start, sizeof(Vector3));
    memcpy(&e, &end,   sizeof(Vector3));
    memcpy(&c, &color, sizeof(Color));
    DrawLine3D(s, e, c);
}

void Iron_draw_point_3d(struct Iron_Vector3 position, struct Iron_Color color) {
    Vector3 p;
    Color   c;
    memcpy(&p, &position, sizeof(Vector3));
    memcpy(&c, &color,    sizeof(Color));
    DrawPoint3D(p, c);
}

void Iron_draw_circle_3d(struct Iron_Vector3 center, float radius,
                         struct Iron_Vector3 rotation_axis, float rotation_angle,
                         struct Iron_Color color) {
    Vector3 ctr, ax;
    Color   c;
    memcpy(&ctr, &center,        sizeof(Vector3));
    memcpy(&ax,  &rotation_axis, sizeof(Vector3));
    memcpy(&c,   &color,         sizeof(Color));
    DrawCircle3D(ctr, radius, ax, rotation_angle, c);
}

void Iron_draw_triangle_3d(struct Iron_Vector3 v1, struct Iron_Vector3 v2,
                           struct Iron_Vector3 v3, struct Iron_Color color) {
    Vector3 a, b, d;
    Color   c;
    memcpy(&a, &v1,    sizeof(Vector3));
    memcpy(&b, &v2,    sizeof(Vector3));
    memcpy(&d, &v3,    sizeof(Vector3));
    memcpy(&c, &color, sizeof(Color));
    DrawTriangle3D(a, b, d, c);
}

/* First [Vector3] array input across the FFI. Iron_List_Iron_Vector3 is
 * defined in iron_raylib.h above the DRAW3D-04 prototypes, under the
 * IRON_LIST_IRON_VECTOR3_STRUCT_DEFINED guard. Vector3 layout is pinned
 * byte-identical by Phase 60-02 _Static_assert grid, so the reinterpret
 * of points.items to `const Vector3 *` is safe. */
void Iron_draw_triangle_strip_3d(Iron_List_Iron_Vector3 points,
                                 int32_t count, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawTriangleStrip3D((const Vector3 *)points.items, (int)count, c);
}

void Iron_draw_cube(struct Iron_Vector3 position, float width, float height,
                    float length, struct Iron_Color color) {
    Vector3 p;
    Color   c;
    memcpy(&p, &position, sizeof(Vector3));
    memcpy(&c, &color,    sizeof(Color));
    DrawCube(p, width, height, length, c);
}

void Iron_draw_cube_v(struct Iron_Vector3 position, struct Iron_Vector3 size,
                      struct Iron_Color color) {
    Vector3 p, s;
    Color   c;
    memcpy(&p, &position, sizeof(Vector3));
    memcpy(&s, &size,     sizeof(Vector3));
    memcpy(&c, &color,    sizeof(Color));
    DrawCubeV(p, s, c);
}

void Iron_draw_cube_wires(struct Iron_Vector3 position, float width, float height,
                          float length, struct Iron_Color color) {
    Vector3 p;
    Color   c;
    memcpy(&p, &position, sizeof(Vector3));
    memcpy(&c, &color,    sizeof(Color));
    DrawCubeWires(p, width, height, length, c);
}

void Iron_draw_cube_wires_v(struct Iron_Vector3 position, struct Iron_Vector3 size,
                            struct Iron_Color color) {
    Vector3 p, s;
    Color   c;
    memcpy(&p, &position, sizeof(Vector3));
    memcpy(&s, &size,     sizeof(Vector3));
    memcpy(&c, &color,    sizeof(Color));
    DrawCubeWiresV(p, s, c);
}

void Iron_draw_sphere(struct Iron_Vector3 center, float radius,
                      struct Iron_Color color) {
    Vector3 ctr;
    Color   c;
    memcpy(&ctr, &center, sizeof(Vector3));
    memcpy(&c,   &color,  sizeof(Color));
    DrawSphere(ctr, radius, c);
}

void Iron_draw_sphere_ex(struct Iron_Vector3 center, float radius,
                         int32_t rings, int32_t slices, struct Iron_Color color) {
    Vector3 ctr;
    Color   c;
    memcpy(&ctr, &center, sizeof(Vector3));
    memcpy(&c,   &color,  sizeof(Color));
    DrawSphereEx(ctr, radius, (int)rings, (int)slices, c);
}

void Iron_draw_sphere_wires(struct Iron_Vector3 center, float radius,
                            int32_t rings, int32_t slices, struct Iron_Color color) {
    Vector3 ctr;
    Color   c;
    memcpy(&ctr, &center, sizeof(Vector3));
    memcpy(&c,   &color,  sizeof(Color));
    DrawSphereWires(ctr, radius, (int)rings, (int)slices, c);
}

/* DRAW3D-04 batch 2: cylinder (4 variants) / capsule (2 variants) /
 * plane / ray / grid — raylib.h:1538-1546. Closes DRAW3D-04 at 21/21.
 *
 * Pitfall 4 (Draw.plane): size param is Vector2 (XZ extent), not
 * Vector3. Shim uses Iron_Vector2 explicitly.
 * Pitfall 6 (Draw.grid): no Color parameter. raylib draws grid in its
 * internal gray.
 * First Ray-by-value INPUT across the FFI (Iron_draw_ray). 24 B
 * struct, pinned layout from Phase 60-05. */

void Iron_draw_cylinder(struct Iron_Vector3 position, float radius_top,
                        float radius_bottom, float height, int32_t slices,
                        struct Iron_Color color) {
    Vector3 p;
    Color   c;
    memcpy(&p, &position, sizeof(Vector3));
    memcpy(&c, &color,    sizeof(Color));
    DrawCylinder(p, radius_top, radius_bottom, height, (int)slices, c);
}

void Iron_draw_cylinder_ex(struct Iron_Vector3 start, struct Iron_Vector3 end,
                           float start_radius, float end_radius, int32_t sides,
                           struct Iron_Color color) {
    Vector3 s, e;
    Color   c;
    memcpy(&s, &start, sizeof(Vector3));
    memcpy(&e, &end,   sizeof(Vector3));
    memcpy(&c, &color, sizeof(Color));
    DrawCylinderEx(s, e, start_radius, end_radius, (int)sides, c);
}

void Iron_draw_cylinder_wires(struct Iron_Vector3 position, float radius_top,
                              float radius_bottom, float height, int32_t slices,
                              struct Iron_Color color) {
    Vector3 p;
    Color   c;
    memcpy(&p, &position, sizeof(Vector3));
    memcpy(&c, &color,    sizeof(Color));
    DrawCylinderWires(p, radius_top, radius_bottom, height, (int)slices, c);
}

void Iron_draw_cylinder_wires_ex(struct Iron_Vector3 start, struct Iron_Vector3 end,
                                 float start_radius, float end_radius, int32_t sides,
                                 struct Iron_Color color) {
    Vector3 s, e;
    Color   c;
    memcpy(&s, &start, sizeof(Vector3));
    memcpy(&e, &end,   sizeof(Vector3));
    memcpy(&c, &color, sizeof(Color));
    DrawCylinderWiresEx(s, e, start_radius, end_radius, (int)sides, c);
}

void Iron_draw_capsule(struct Iron_Vector3 start, struct Iron_Vector3 end,
                       float radius, int32_t slices, int32_t rings,
                       struct Iron_Color color) {
    Vector3 s, e;
    Color   c;
    memcpy(&s, &start, sizeof(Vector3));
    memcpy(&e, &end,   sizeof(Vector3));
    memcpy(&c, &color, sizeof(Color));
    DrawCapsule(s, e, radius, (int)slices, (int)rings, c);
}

void Iron_draw_capsule_wires(struct Iron_Vector3 start, struct Iron_Vector3 end,
                             float radius, int32_t slices, int32_t rings,
                             struct Iron_Color color) {
    Vector3 s, e;
    Color   c;
    memcpy(&s, &start, sizeof(Vector3));
    memcpy(&e, &end,   sizeof(Vector3));
    memcpy(&c, &color, sizeof(Color));
    DrawCapsuleWires(s, e, radius, (int)slices, (int)rings, c);
}

/* Pitfall 4: Vector2 size (XZ extent). */
void Iron_draw_plane(struct Iron_Vector3 center, struct Iron_Vector2 size,
                     struct Iron_Color color) {
    Vector3 ctr;
    Vector2 sz;
    Color   c;
    memcpy(&ctr, &center, sizeof(Vector3));
    memcpy(&sz,  &size,   sizeof(Vector2));
    memcpy(&c,   &color,  sizeof(Color));
    DrawPlane(ctr, sz, c);
}

/* First Ray-by-value INPUT across the FFI. */
void Iron_draw_ray(struct Iron_Ray ray, struct Iron_Color color) {
    Ray   rl;
    Color c;
    memcpy(&rl, &ray,   sizeof(Ray));
    memcpy(&c,  &color, sizeof(Color));
    DrawRay(rl, c);
}

/* Pitfall 6: no Color parameter. raylib draws grid in its internal gray. */
void Iron_draw_grid(int32_t slices, float spacing) {
    DrawGrid((int)slices, spacing);
}

/* ── Models (Phase 70) ────────────────────────────────────────────── */

/* Phase 70 binds rmodels.c: Model / Mesh / Material / ModelAnimation +
 * billboard + bounding-box draws. All struct sizes under the Phase 64-02
 * 120 B validated ceiling. Model = 120 B, Mesh = 120 B, Material = 40 B,
 * BoundingBox = 24 B, ModelAnimation = 56 B.
 *
 * Templates used (all proven in Phase 63-69):
 *   B — struct-in (memcpy-in)
 *   E — scalar + Color
 *   F — Vector3 + Color
 *   G — struct-by-value RETURN (Phase 65-03 validated 64 B; Plan 70-03
 *       probes 120 B direction)
 *   Mutating-return-by-value (Phase 66-03 / 68-02) — for raylib APIs
 *       that take Type* and mutate in place (mesh.upload, material.set_texture)
 *
 * Memory disciplined: each shim makes at most one `memcpy` per struct arg
 * plus one `memcpy` per struct return. Zero heap allocation in MODEL-01/02/03.
 */

/* MODEL-01: Model load / from_mesh / is_valid / unload — raylib.h:1553-1556 */

struct Iron_Model Iron_model_load(Iron_String file_name) {
    Model rl = LoadModel(iron_string_cstr(&file_name));
    struct Iron_Model out;
    memcpy(&out, &rl, sizeof(struct Iron_Model));
    return out;
}

struct Iron_Model Iron_model_from_mesh(struct Iron_Mesh mesh) {
    Mesh rm;
    memcpy(&rm, &mesh, sizeof(Mesh));
    Model rl = LoadModelFromMesh(rm);
    struct Iron_Model out;
    memcpy(&out, &rl, sizeof(struct Iron_Model));
    return out;
}

bool Iron_model_is_valid(struct Iron_Model model) {
    Model rm;
    memcpy(&rm, &model, sizeof(Model));
    return (bool)(IsModelValid(rm) != 0);
}

void Iron_model_unload(struct Iron_Model model) {
    Model rm;
    memcpy(&rm, &model, sizeof(Model));
    UnloadModel(rm);
}

/* MODEL-02: Bounding box — raylib.h:1557 */

struct Iron_BoundingBox Iron_model_bounding_box(struct Iron_Model model) {
    Model rm;
    memcpy(&rm, &model, sizeof(Model));
    BoundingBox rl = GetModelBoundingBox(rm);
    struct Iron_BoundingBox out;
    memcpy(&out, &rl, sizeof(struct Iron_BoundingBox));
    return out;
}

/* MODEL-03: Draw variants (6) — raylib.h:1560-1565 */

void Iron_model_draw(struct Iron_Model model, struct Iron_Vector3 position,
                     float scale, struct Iron_Color tint) {
    Model   rm;
    Vector3 pos;
    Color   c;
    memcpy(&rm,  &model,    sizeof(Model));
    memcpy(&pos, &position, sizeof(Vector3));
    memcpy(&c,   &tint,     sizeof(Color));
    DrawModel(rm, pos, scale, c);
}

void Iron_model_draw_ex(struct Iron_Model model, struct Iron_Vector3 position,
                        struct Iron_Vector3 rotation_axis, float rotation_angle,
                        struct Iron_Vector3 scale, struct Iron_Color tint) {
    Model   rm;
    Vector3 pos, ax, sc;
    Color   c;
    memcpy(&rm,  &model,         sizeof(Model));
    memcpy(&pos, &position,      sizeof(Vector3));
    memcpy(&ax,  &rotation_axis, sizeof(Vector3));
    memcpy(&sc,  &scale,         sizeof(Vector3));
    memcpy(&c,   &tint,          sizeof(Color));
    DrawModelEx(rm, pos, ax, rotation_angle, sc, c);
}

void Iron_model_draw_wires(struct Iron_Model model, struct Iron_Vector3 position,
                           float scale, struct Iron_Color tint) {
    Model   rm;
    Vector3 pos;
    Color   c;
    memcpy(&rm,  &model,    sizeof(Model));
    memcpy(&pos, &position, sizeof(Vector3));
    memcpy(&c,   &tint,     sizeof(Color));
    DrawModelWires(rm, pos, scale, c);
}

void Iron_model_draw_wires_ex(struct Iron_Model model, struct Iron_Vector3 position,
                              struct Iron_Vector3 rotation_axis, float rotation_angle,
                              struct Iron_Vector3 scale, struct Iron_Color tint) {
    Model   rm;
    Vector3 pos, ax, sc;
    Color   c;
    memcpy(&rm,  &model,         sizeof(Model));
    memcpy(&pos, &position,      sizeof(Vector3));
    memcpy(&ax,  &rotation_axis, sizeof(Vector3));
    memcpy(&sc,  &scale,         sizeof(Vector3));
    memcpy(&c,   &tint,          sizeof(Color));
    DrawModelWiresEx(rm, pos, ax, rotation_angle, sc, c);
}

void Iron_model_draw_points(struct Iron_Model model, struct Iron_Vector3 position,
                            float scale, struct Iron_Color tint) {
    Model   rm;
    Vector3 pos;
    Color   c;
    memcpy(&rm,  &model,    sizeof(Model));
    memcpy(&pos, &position, sizeof(Vector3));
    memcpy(&c,   &tint,     sizeof(Color));
    DrawModelPoints(rm, pos, scale, c);
}

void Iron_model_draw_points_ex(struct Iron_Model model, struct Iron_Vector3 position,
                               struct Iron_Vector3 rotation_axis, float rotation_angle,
                               struct Iron_Vector3 scale, struct Iron_Color tint) {
    Model   rm;
    Vector3 pos, ax, sc;
    Color   c;
    memcpy(&rm,  &model,         sizeof(Model));
    memcpy(&pos, &position,      sizeof(Vector3));
    memcpy(&ax,  &rotation_axis, sizeof(Vector3));
    memcpy(&sc,  &scale,         sizeof(Vector3));
    memcpy(&c,   &tint,          sizeof(Color));
    DrawModelPointsEx(rm, pos, ax, rotation_angle, sc, c);
}

/* MODEL-04: Mesh operations (7) — raylib.h:1572-1580
 *
 * Iron_mesh_upload / Iron_mesh_gen_tangents use mutating-return-by-value
 * (Phase 66-03 template) since raylib's UploadMesh / GenMeshTangents take
 * Mesh* and mutate in place. Iron val-fields are immutable; users rebind
 * via `var mesh = Mesh.cube(...); mesh = mesh.upload(true)` or
 * `val uploaded = Mesh.upload(mesh, true)`.
 *
 * Iron_mesh_update_buffer forwards Iron_List_uint8_t.items as (const void *)
 * — ABI-UINT8 closed Phase 68-01.
 */

struct Iron_Mesh Iron_mesh_upload(struct Iron_Mesh mesh, bool dynamic) {
    Mesh local;
    memcpy(&local, &mesh, sizeof(Mesh));
    UploadMesh(&local, dynamic);
    struct Iron_Mesh out;
    memcpy(&out, &local, sizeof(struct Iron_Mesh));
    return out;
}

void Iron_mesh_update_buffer(struct Iron_Mesh mesh, int32_t index,
                             Iron_List_uint8_t data, int32_t data_size,
                             int32_t offset) {
    Mesh rm;
    memcpy(&rm, &mesh, sizeof(Mesh));
    UpdateMeshBuffer(rm, (int)index, (const void *)data.items,
                     (int)data_size, (int)offset);
}

void Iron_mesh_unload(struct Iron_Mesh mesh) {
    Mesh rm;
    memcpy(&rm, &mesh, sizeof(Mesh));
    UnloadMesh(rm);
}

bool Iron_mesh_export(struct Iron_Mesh mesh, Iron_String file_name) {
    Mesh rm;
    memcpy(&rm, &mesh, sizeof(Mesh));
    return (bool)(ExportMesh(rm, iron_string_cstr(&file_name)) != 0);
}

bool Iron_mesh_export_as_code(struct Iron_Mesh mesh, Iron_String file_name) {
    Mesh rm;
    memcpy(&rm, &mesh, sizeof(Mesh));
    return (bool)(ExportMeshAsCode(rm, iron_string_cstr(&file_name)) != 0);
}

struct Iron_BoundingBox Iron_mesh_bounding_box(struct Iron_Mesh mesh) {
    Mesh rm;
    memcpy(&rm, &mesh, sizeof(Mesh));
    BoundingBox rl = GetMeshBoundingBox(rm);
    struct Iron_BoundingBox out;
    memcpy(&out, &rl, sizeof(struct Iron_BoundingBox));
    return out;
}

struct Iron_Mesh Iron_mesh_gen_tangents(struct Iron_Mesh mesh) {
    Mesh local;
    memcpy(&local, &mesh, sizeof(Mesh));
    GenMeshTangents(&local);
    struct Iron_Mesh out;
    memcpy(&out, &local, sizeof(struct Iron_Mesh));
    return out;
}

/* MODEL-05: Mesh draw (2) — raylib.h:1575-1576
 *
 * Iron_mesh_draw — first simultaneous 120+40+64 = 224 B struct-by-value
 * input across the FFI. Each arg independently under 120 B ceiling; clang
 * evaluates per-struct, not cumulative. Phase 64-02 validated 120+64 B
 * double input (Iron_ray_hit_mesh); this adds Material (40 B) to the mix.
 *
 * Iron_mesh_draw_instanced — first [Matrix] list input via
 * Iron_List_Iron_Matrix (Scan B auto-emitted, verified by Plan 70-01).
 * Forward .items as (const Matrix *).
 */

void Iron_mesh_draw(struct Iron_Mesh mesh, struct Iron_Material material,
                    struct Iron_Matrix transform) {
    Mesh     rm;
    Material rmat;
    Matrix   rt;
    memcpy(&rm,   &mesh,      sizeof(Mesh));
    memcpy(&rmat, &material,  sizeof(Material));
    memcpy(&rt,   &transform, sizeof(Matrix));
    DrawMesh(rm, rmat, rt);
}

void Iron_mesh_draw_instanced(struct Iron_Mesh mesh, struct Iron_Material material,
                              Iron_List_Iron_Matrix transforms, int32_t instances) {
    Mesh     rm;
    Material rmat;
    memcpy(&rm,   &mesh,     sizeof(Mesh));
    memcpy(&rmat, &material, sizeof(Material));
    DrawMeshInstanced(rm, rmat, (const Matrix *)transforms.items, (int)instances);
}

/* MODEL-06: Mesh generation (Plan 70-03 Task 1 probe) — raylib.h:1585
 *
 * First 120 B struct-by-value RETURN in the Iron stdlib. Phase 64-02
 * validated 120 B ARG clean; Phase 65-03 validated 64 B RETURN clean.
 * clang -Wall -Wextra may fire -Wlarge-by-value-copy (default threshold
 * strictly > 64 B); if it does, refactor to hidden-out-param.
 * AAPCS64 / SysV both transparently use indirect return slots for large
 * structs at the ABI level — warning is about the source-level signature,
 * not the lowered ABI, so GREEN is the expected outcome.
 *
 * Template G: no memcpy-in (scalar args only); call raylib; memcpy-out
 * the Iron struct return.
 */

struct Iron_Mesh Iron_mesh_cube(float width, float height, float length) {
    Mesh rl = GenMeshCube(width, height, length);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

/* MODEL-06 remaining (10): poly / plane / sphere / hemi_sphere / cylinder
 * / cone / torus / knot / heightmap / cubicmap — raylib.h:1583-1593.
 * All return 120 B Mesh struct-by-value. Template G (confirmed by Task 1
 * probe). Heightmap + Cubicmap also take Image (40 B) + Vector3 (12 B)
 * by value — Template B composition. */

struct Iron_Mesh Iron_mesh_poly(int32_t sides, float radius) {
    Mesh rl = GenMeshPoly((int)sides, radius);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_plane(float width, float length, int32_t res_x, int32_t res_z) {
    Mesh rl = GenMeshPlane(width, length, (int)res_x, (int)res_z);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_sphere(float radius, int32_t rings, int32_t slices) {
    Mesh rl = GenMeshSphere(radius, (int)rings, (int)slices);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_hemi_sphere(float radius, int32_t rings, int32_t slices) {
    Mesh rl = GenMeshHemiSphere(radius, (int)rings, (int)slices);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_cylinder(float radius, float height, int32_t slices) {
    Mesh rl = GenMeshCylinder(radius, height, (int)slices);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_cone(float radius, float height, int32_t slices) {
    Mesh rl = GenMeshCone(radius, height, (int)slices);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_torus(float radius, float size, int32_t rad_seg, int32_t sides) {
    Mesh rl = GenMeshTorus(radius, size, (int)rad_seg, (int)sides);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_knot(float radius, float size, int32_t rad_seg, int32_t sides) {
    Mesh rl = GenMeshKnot(radius, size, (int)rad_seg, (int)sides);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_heightmap(struct Iron_Image heightmap, struct Iron_Vector3 size) {
    Image   hm;
    Vector3 sz;
    memcpy(&hm, &heightmap, sizeof(Image));
    memcpy(&sz, &size,      sizeof(Vector3));
    Mesh rl = GenMeshHeightmap(hm, sz);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

struct Iron_Mesh Iron_mesh_cubicmap(struct Iron_Image cubicmap, struct Iron_Vector3 cube_size) {
    Image   cm;
    Vector3 sz;
    memcpy(&cm, &cubicmap,  sizeof(Image));
    memcpy(&sz, &cube_size, sizeof(Vector3));
    Mesh rl = GenMeshCubicmap(cm, sz);
    struct Iron_Mesh out;
    memcpy(&out, &rl, sizeof(struct Iron_Mesh));
    return out;
}

/* ── Shaders (Phase 71) ───────────────────────────────────────────── */
/* ── File I/O & Utils (Phase 72) ──────────────────────────────────── */

/* Phase 60 leaves this file intentionally empty of wrapper functions.
 * A later Phase 60 plan (60-08 clean-break) may rewrite pong.iron /
 * game_raylib.iron / hello_raylib.iron to only use TYPE/ENUM
 * definitions — no wrapper calls — so zero-function iron_raylib.c
 * still links. If a rewrite needs a specific wrapper earlier than
 * Phase 61, that plan will add a single section-scoped shim here. */
