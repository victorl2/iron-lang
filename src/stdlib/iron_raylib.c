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
/* ── 2D Drawing (Phase 63) ────────────────────────────────────────── */
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
