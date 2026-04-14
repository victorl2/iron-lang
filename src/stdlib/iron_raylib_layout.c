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

/* Vector2: size + 2 offsets */
_Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2),
               "Iron_Vector2 size must equal Vector2");
_Static_assert(offsetof(struct Iron_Vector2, x) == offsetof(Vector2, x),
               "Iron_Vector2.x offset must equal Vector2.x");
_Static_assert(offsetof(struct Iron_Vector2, y) == offsetof(Vector2, y),
               "Iron_Vector2.y offset must equal Vector2.y");

/* Vector3: size + 3 offsets */
_Static_assert(sizeof(struct Iron_Vector3) == sizeof(Vector3),
               "Iron_Vector3 size must equal Vector3");
_Static_assert(offsetof(struct Iron_Vector3, x) == offsetof(Vector3, x),
               "Iron_Vector3.x offset must equal Vector3.x");
_Static_assert(offsetof(struct Iron_Vector3, y) == offsetof(Vector3, y),
               "Iron_Vector3.y offset must equal Vector3.y");
_Static_assert(offsetof(struct Iron_Vector3, z) == offsetof(Vector3, z),
               "Iron_Vector3.z offset must equal Vector3.z");

/* Vector4: size + 4 offsets */
_Static_assert(sizeof(struct Iron_Vector4) == sizeof(Vector4),
               "Iron_Vector4 size must equal Vector4");
_Static_assert(offsetof(struct Iron_Vector4, x) == offsetof(Vector4, x),
               "Iron_Vector4.x offset must equal Vector4.x");
_Static_assert(offsetof(struct Iron_Vector4, y) == offsetof(Vector4, y),
               "Iron_Vector4.y offset must equal Vector4.y");
_Static_assert(offsetof(struct Iron_Vector4, z) == offsetof(Vector4, z),
               "Iron_Vector4.z offset must equal Vector4.z");
_Static_assert(offsetof(struct Iron_Vector4, w) == offsetof(Vector4, w),
               "Iron_Vector4.w offset must equal Vector4.w");

/* Quaternion: layout-compat with Vector4 (raylib typedef). Assert
 * against the concrete Vector4 struct — raylib's `Quaternion` is a
 * typedef to Vector4, so sizeof/offsetof work on both names. */
_Static_assert(sizeof(struct Iron_Quaternion) == sizeof(Quaternion),
               "Iron_Quaternion size must equal Quaternion");
_Static_assert(sizeof(struct Iron_Quaternion) == sizeof(Vector4),
               "Iron_Quaternion must be layout-identical to Vector4");
_Static_assert(offsetof(struct Iron_Quaternion, x) == offsetof(Quaternion, x),
               "Iron_Quaternion.x offset must equal Quaternion.x");
_Static_assert(offsetof(struct Iron_Quaternion, y) == offsetof(Quaternion, y),
               "Iron_Quaternion.y offset must equal Quaternion.y");
_Static_assert(offsetof(struct Iron_Quaternion, z) == offsetof(Quaternion, z),
               "Iron_Quaternion.z offset must equal Quaternion.z");
_Static_assert(offsetof(struct Iron_Quaternion, w) == offsetof(Quaternion, w),
               "Iron_Quaternion.w offset must equal Quaternion.w");

/* Matrix: size + 16 offsets */
_Static_assert(sizeof(struct Iron_Matrix) == sizeof(Matrix),
               "Iron_Matrix size must equal Matrix");
_Static_assert(offsetof(struct Iron_Matrix, m0)  == offsetof(Matrix, m0),  "Iron_Matrix.m0 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m4)  == offsetof(Matrix, m4),  "Iron_Matrix.m4 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m8)  == offsetof(Matrix, m8),  "Iron_Matrix.m8 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m12) == offsetof(Matrix, m12), "Iron_Matrix.m12 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m1)  == offsetof(Matrix, m1),  "Iron_Matrix.m1 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m5)  == offsetof(Matrix, m5),  "Iron_Matrix.m5 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m9)  == offsetof(Matrix, m9),  "Iron_Matrix.m9 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m13) == offsetof(Matrix, m13), "Iron_Matrix.m13 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m2)  == offsetof(Matrix, m2),  "Iron_Matrix.m2 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m6)  == offsetof(Matrix, m6),  "Iron_Matrix.m6 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m10) == offsetof(Matrix, m10), "Iron_Matrix.m10 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m14) == offsetof(Matrix, m14), "Iron_Matrix.m14 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m3)  == offsetof(Matrix, m3),  "Iron_Matrix.m3 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m7)  == offsetof(Matrix, m7),  "Iron_Matrix.m7 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m11) == offsetof(Matrix, m11), "Iron_Matrix.m11 offset mismatch");
_Static_assert(offsetof(struct Iron_Matrix, m15) == offsetof(Matrix, m15), "Iron_Matrix.m15 offset mismatch");

/* Rectangle: size + 4 offsets */
_Static_assert(sizeof(struct Iron_Rectangle) == sizeof(Rectangle),
               "Iron_Rectangle size must equal Rectangle");
_Static_assert(offsetof(struct Iron_Rectangle, x)      == offsetof(Rectangle, x),      "Iron_Rectangle.x offset mismatch");
_Static_assert(offsetof(struct Iron_Rectangle, y)      == offsetof(Rectangle, y),      "Iron_Rectangle.y offset mismatch");
_Static_assert(offsetof(struct Iron_Rectangle, width)  == offsetof(Rectangle, width),  "Iron_Rectangle.width offset mismatch");
_Static_assert(offsetof(struct Iron_Rectangle, height) == offsetof(Rectangle, height), "Iron_Rectangle.height offset mismatch");

/* Color: size + 4 offsets */
_Static_assert(sizeof(struct Iron_Color) == sizeof(Color),
               "Iron_Color size must equal Color");
_Static_assert(offsetof(struct Iron_Color, r) == offsetof(Color, r), "Iron_Color.r offset mismatch");
_Static_assert(offsetof(struct Iron_Color, g) == offsetof(Color, g), "Iron_Color.g offset mismatch");
_Static_assert(offsetof(struct Iron_Color, b) == offsetof(Color, b), "Iron_Color.b offset mismatch");
_Static_assert(offsetof(struct Iron_Color, a) == offsetof(Color, a), "Iron_Color.a offset mismatch");

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
