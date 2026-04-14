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

#include "iron_raylib.h"
#include "raylib.h"

/* ════════════════════════════════════════════════════════════════════
 * Section markers — wrapper functions land here in later phases.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Window & System (Phase 61) ───────────────────────────────────── */

/* Phase 61 struct-by-value return ABI smoke test. Hardcodes a
 * Vector2{3.5f, 4.5f} literal and returns it. Iron code calls this
 * and checks both fields match; any mismatch means Iron's struct-
 * return ABI is broken and we must switch to an out-param pattern. */
struct Iron_Vector2 Iron_window_abi_smoke_test(void) {
    struct Iron_Vector2 v = { 3.5f, 4.5f };
    return v;
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
