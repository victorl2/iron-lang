#ifndef IRON_RAYLIB_H
#define IRON_RAYLIB_H

/* iron_raylib.h — Phase 60+ raylib stdlib surface.
 *
 * This header is consumed by:
 *   - src/stdlib/iron_raylib.c         (shim wrapper implementations)
 *   - src/stdlib/iron_raylib_layout.c  (compile-time ABI assertions)
 *   - tests/unit/test_stdlib_raylib_*.c (future Unity tests, if any)
 *
 * It is intentionally NOT included by the Iron codegen output. Generated
 * C files receive extern prototypes for shim-call stubs through the
 * emit_c.c Phase 3 "is_extern && !extern_c_name" branch, which lands the
 * prototypes in ctx.prototypes AFTER the compiler's own struct body
 * emission — avoiding the double-definition conflict that arises when
 * both the header and the compiler declare Iron_Vector3 (etc.) twice.
 * Same trap as iron_net.h; see that file's top-of-file comment for the
 * full history.
 *
 * Layout contract: every Iron-side `Iron_<Typename>` struct declared
 * below must be byte-for-byte identical to raylib's corresponding
 * `typedef ... <Typename>` from src/vendor/raylib/raylib.h. iron_raylib_layout.c
 * emits a `_Static_assert` for the total size PLUS one per field offset
 * to enforce this at build time — no runtime drift possible.
 *
 * Method-call mangling: the Iron-side `func Texture.draw(t: Texture, ...)
 * {}` lowers to `Iron_texture_draw(Iron_Texture t, ...)` at the C level
 * via hir_to_lir's lowercase-type mangling. Shim implementations in
 * iron_raylib.c must spell those exact symbol names.
 *
 * Phase 60 scope: this file is a SCAFFOLD. It contains the include
 * guards, the runtime/stdbool includes, and section-marker comments
 * where later plans will add Iron_* struct definitions and shim
 * function prototypes. No content beyond the scaffold ships in Plan
 * 60-01.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "runtime/iron_runtime.h"

/* ════════════════════════════════════════════════════════════════════
 * Iron-side struct mirrors — layout-compatible with raylib.h typedefs.
 * Populated by Plans 60-02 through 60-05.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Core math types (Plan 60-02) ─────────────────────────────────── */
/* Iron_Vector2, Iron_Vector3, Iron_Vector4, Iron_Quaternion,
 * Iron_Matrix, Iron_Rectangle, Iron_Color — added by 60-02. */

/* ── Image / Texture / Font types (Plan 60-03) ────────────────────── */
/* Iron_Image, Iron_Texture, Iron_RenderTexture, Iron_NPatchInfo,
 * Iron_GlyphInfo, Iron_Font — added by 60-03. */

/* ── Camera / Mesh / Model types (Plan 60-04) ─────────────────────── */
/* Iron_Camera2D, Iron_Camera3D, Iron_Mesh, Iron_Shader,
 * Iron_MaterialMap, Iron_Material, Iron_Transform, Iron_BoneInfo,
 * Iron_Model, Iron_ModelAnimation — added by 60-04. */

/* ── 3D helpers / audio / file types (Plan 60-05) ─────────────────── */
/* Iron_Ray, Iron_RayCollision, Iron_BoundingBox, Iron_Wave,
 * Iron_AudioStream, Iron_Sound, Iron_Music, Iron_FilePathList —
 * added by 60-05. */

/* ════════════════════════════════════════════════════════════════════
 * Shim wrapper function prototypes — added by Phases 61–72.
 * Phase 60 declares no functions; the file is populated incrementally
 * as each binding phase lands its subset.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Window & System (Phase 61) ───────────────────────────────────── */
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

#endif /* IRON_RAYLIB_H */
