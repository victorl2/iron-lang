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

/* ── Collision (Phase 64) ─────────────────────────────────────────── */
/* ── raymath (Phase 65) ───────────────────────────────────────────── */
/* ── Textures & Images (Phase 66) ─────────────────────────────────── */
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
