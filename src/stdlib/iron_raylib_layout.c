/* iron_raylib_layout.c — Phase 60+ compile-time ABI enforcement.
 *
 * This translation unit has ONE job: fire compile-time static
 * assertions whenever an Iron-side `Iron_<Typename>` struct (declared
 * in iron_raylib.h, mirroring Iron `object <Typename>` in raylib.iron)
 * drifts from the C `<Typename>` defined in raylib.h.
 *
 * Two assertion classes per type (added by Plans 60-02..05):
 *   1. Size equality between sizeof(Iron_<T>) and sizeof(<T>).
 *   2. Per-field offset equality between offsetof(Iron_<T>, field)
 *      and offsetof(<T>, field) — one assertion per field.
 *
 * This is the only translation unit in the codebase where BOTH
 * iron_raylib.h AND raylib.h are included together. No double-
 * definition risk because iron_raylib.h is never included by Iron
 * codegen output — only by this file, iron_raylib.c, and future unit
 * tests. Same pattern as iron_net.h.
 *
 * The file emits no runtime code. Compile-time assertions evaluate at
 * build time and the resulting object file is effectively empty
 * (possibly a single compile-unit sentinel). It links into the final
 * binary with zero size impact.
 *
 * Populated by Plans 60-02 through 60-05 — one assertion group per
 * struct type, ~180 assertions in total at full Phase 60 completion.
 */

#include <stddef.h>
#include "iron_raylib.h"
#include "raylib.h"

/* ════════════════════════════════════════════════════════════════════
 * Struct layout assertions — one group per raylib type.
 * Added by Plans 60-02 through 60-05.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Core math types (Plan 60-02) ─────────────────────────────────── */
/* Static-assertion groups for Vector2, Vector3, Vector4, Quaternion,
 * Matrix, Rectangle, Color. */

/* ── Image / Texture / Font types (Plan 60-03) ────────────────────── */
/* Static-assertion groups for Image, Texture, RenderTexture,
 * NPatchInfo, GlyphInfo, Font. */

/* ── Camera / Mesh / Model types (Plan 60-04) ─────────────────────── */
/* Static-assertion groups for Camera2D, Camera3D, Mesh, Shader,
 * MaterialMap, Material, Transform, BoneInfo, Model, ModelAnimation. */

/* ── 3D helpers / audio / file types (Plan 60-05) ─────────────────── */
/* Static-assertion groups for Ray, RayCollision, BoundingBox, Wave,
 * AudioStream, Sound, Music, FilePathList. */

/* ════════════════════════════════════════════════════════════════════
 * Enum ordinal assertions — populated by Plan 60-07.
 * One assertion per enum value: ~240 total across 22 enums.
 * ════════════════════════════════════════════════════════════════════ */

/* ── All enums (Plan 60-07) ───────────────────────────────────────── */
/* Static-assertion groups for KeyboardKey, MouseButton, MouseCursor,
 * GamepadButton, GamepadAxis, ConfigFlags, TraceLogLevel, BlendMode,
 * PixelFormat, TextureFilter, TextureWrap, CubemapLayout, FontType,
 * CameraMode, CameraProjection, MaterialMapIndex, ShaderLocationIndex,
 * ShaderUniformDataType, ShaderAttributeDataType, Gesture,
 * NPatchLayout. */

/* Compile-unit sentinel: ensures at least one symbol exists in the
 * object file so the linker doesn't warn about an "empty" TU. The
 * `static` + unused attribute silences unused-variable warnings until
 * real static-assertions land in Plans 60-02..05 and 60-07. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static const int iron_raylib_layout_sentinel = 0;
