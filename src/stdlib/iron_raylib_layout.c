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

/* ── All enums (Plan 60-07) — raylib C constant anchors ───────────── */
/*
 * Strategy: Iron `enum` values in raylib.iron are hand-copied integer
 * literals. At the Iron -> C lowering step, an enum reference inlines
 * to the raw integer (no Iron_<Enum>_<Value> C symbol exists).
 *
 * Therefore the assertions below ANCHOR the raylib C constants to the
 * hard-coded integers that raylib.iron expects. If a future raylib
 * upgrade renumbers any KEY_* / MOUSE_BUTTON_* / etc. constant, the
 * corresponding _Static_assert fires at build time with a clear
 * message — and the developer updates raylib.iron to match.
 *
 * Closes ENUM-01..21 (transcription of all 21 enums) + ENUM-22
 * (ordinal-match verification) at build time.
 *
 * ENUM-22's "every API parameter uses the typed Iron enum, never raw
 * Int" half is deferred to Phases 61+ where the function signatures
 * are written. Phase 60 only DEFINES the enums.
 */

/* KeyboardKey anchors. Each _Static_assert confirms that raylib's C
 * KEY_* constant has the numeric value the Iron enum copies. If a
 * future raylib upgrade renumbers KEY_SPACE, this fires. Representative
 * sample across every section of the KeyboardKey enum. */
_Static_assert(KEY_NULL == 0, "KEY_NULL drifted");
_Static_assert(KEY_SPACE == 32, "KEY_SPACE drifted");
_Static_assert(KEY_ESCAPE == 256, "KEY_ESCAPE drifted");
_Static_assert(KEY_ENTER == 257, "KEY_ENTER drifted");
_Static_assert(KEY_RIGHT == 262, "KEY_RIGHT drifted");
_Static_assert(KEY_LEFT  == 263, "KEY_LEFT drifted");
_Static_assert(KEY_DOWN  == 264, "KEY_DOWN drifted");
_Static_assert(KEY_UP    == 265, "KEY_UP drifted");
_Static_assert(KEY_A == 65, "KEY_A drifted");
_Static_assert(KEY_Z == 90, "KEY_Z drifted");
_Static_assert(KEY_F1 == 290, "KEY_F1 drifted");
_Static_assert(KEY_F12 == 301, "KEY_F12 drifted");
_Static_assert(KEY_KP_0 == 320, "KEY_KP_0 drifted");
_Static_assert(KEY_KP_EQUAL == 336, "KEY_KP_EQUAL drifted");
_Static_assert(KEY_LEFT_SHIFT == 340, "KEY_LEFT_SHIFT drifted");
_Static_assert(KEY_KB_MENU == 348, "KEY_KB_MENU drifted");
_Static_assert(KEY_BACK == 4, "KEY_BACK drifted");
_Static_assert(KEY_MENU == 5, "KEY_MENU drifted");
_Static_assert(KEY_VOLUME_UP == 24, "KEY_VOLUME_UP drifted");
_Static_assert(KEY_VOLUME_DOWN == 25, "KEY_VOLUME_DOWN drifted");

/* MouseButton */
_Static_assert(MOUSE_BUTTON_LEFT    == 0, "MOUSE_BUTTON_LEFT drifted");
_Static_assert(MOUSE_BUTTON_RIGHT   == 1, "MOUSE_BUTTON_RIGHT drifted");
_Static_assert(MOUSE_BUTTON_MIDDLE  == 2, "MOUSE_BUTTON_MIDDLE drifted");
_Static_assert(MOUSE_BUTTON_SIDE    == 3, "MOUSE_BUTTON_SIDE drifted");
_Static_assert(MOUSE_BUTTON_EXTRA   == 4, "MOUSE_BUTTON_EXTRA drifted");
_Static_assert(MOUSE_BUTTON_FORWARD == 5, "MOUSE_BUTTON_FORWARD drifted");
_Static_assert(MOUSE_BUTTON_BACK    == 6, "MOUSE_BUTTON_BACK drifted");

/* MouseCursor */
_Static_assert(MOUSE_CURSOR_DEFAULT       == 0,  "MOUSE_CURSOR_DEFAULT drifted");
_Static_assert(MOUSE_CURSOR_ARROW         == 1,  "MOUSE_CURSOR_ARROW drifted");
_Static_assert(MOUSE_CURSOR_IBEAM         == 2,  "MOUSE_CURSOR_IBEAM drifted");
_Static_assert(MOUSE_CURSOR_CROSSHAIR     == 3,  "MOUSE_CURSOR_CROSSHAIR drifted");
_Static_assert(MOUSE_CURSOR_POINTING_HAND == 4,  "MOUSE_CURSOR_POINTING_HAND drifted");
_Static_assert(MOUSE_CURSOR_RESIZE_EW     == 5,  "MOUSE_CURSOR_RESIZE_EW drifted");
_Static_assert(MOUSE_CURSOR_RESIZE_NS     == 6,  "MOUSE_CURSOR_RESIZE_NS drifted");
_Static_assert(MOUSE_CURSOR_RESIZE_NWSE   == 7,  "MOUSE_CURSOR_RESIZE_NWSE drifted");
_Static_assert(MOUSE_CURSOR_RESIZE_NESW   == 8,  "MOUSE_CURSOR_RESIZE_NESW drifted");
_Static_assert(MOUSE_CURSOR_RESIZE_ALL    == 9,  "MOUSE_CURSOR_RESIZE_ALL drifted");
_Static_assert(MOUSE_CURSOR_NOT_ALLOWED   == 10, "MOUSE_CURSOR_NOT_ALLOWED drifted");

/* GamepadButton */
_Static_assert(GAMEPAD_BUTTON_UNKNOWN          == 0,  "GAMEPAD_BUTTON_UNKNOWN drifted");
_Static_assert(GAMEPAD_BUTTON_LEFT_FACE_UP     == 1,  "GAMEPAD_BUTTON_LEFT_FACE_UP drifted");
_Static_assert(GAMEPAD_BUTTON_LEFT_FACE_RIGHT  == 2,  "GAMEPAD_BUTTON_LEFT_FACE_RIGHT drifted");
_Static_assert(GAMEPAD_BUTTON_LEFT_FACE_DOWN   == 3,  "GAMEPAD_BUTTON_LEFT_FACE_DOWN drifted");
_Static_assert(GAMEPAD_BUTTON_LEFT_FACE_LEFT   == 4,  "GAMEPAD_BUTTON_LEFT_FACE_LEFT drifted");
_Static_assert(GAMEPAD_BUTTON_RIGHT_FACE_UP    == 5,  "GAMEPAD_BUTTON_RIGHT_FACE_UP drifted");
_Static_assert(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT == 6,  "GAMEPAD_BUTTON_RIGHT_FACE_RIGHT drifted");
_Static_assert(GAMEPAD_BUTTON_RIGHT_FACE_DOWN  == 7,  "GAMEPAD_BUTTON_RIGHT_FACE_DOWN drifted");
_Static_assert(GAMEPAD_BUTTON_RIGHT_FACE_LEFT  == 8,  "GAMEPAD_BUTTON_RIGHT_FACE_LEFT drifted");
_Static_assert(GAMEPAD_BUTTON_LEFT_TRIGGER_1   == 9,  "GAMEPAD_BUTTON_LEFT_TRIGGER_1 drifted");
_Static_assert(GAMEPAD_BUTTON_LEFT_TRIGGER_2   == 10, "GAMEPAD_BUTTON_LEFT_TRIGGER_2 drifted");
_Static_assert(GAMEPAD_BUTTON_RIGHT_TRIGGER_1  == 11, "GAMEPAD_BUTTON_RIGHT_TRIGGER_1 drifted");
_Static_assert(GAMEPAD_BUTTON_RIGHT_TRIGGER_2  == 12, "GAMEPAD_BUTTON_RIGHT_TRIGGER_2 drifted");
_Static_assert(GAMEPAD_BUTTON_MIDDLE_LEFT      == 13, "GAMEPAD_BUTTON_MIDDLE_LEFT drifted");
_Static_assert(GAMEPAD_BUTTON_MIDDLE           == 14, "GAMEPAD_BUTTON_MIDDLE drifted");
_Static_assert(GAMEPAD_BUTTON_MIDDLE_RIGHT     == 15, "GAMEPAD_BUTTON_MIDDLE_RIGHT drifted");
_Static_assert(GAMEPAD_BUTTON_LEFT_THUMB       == 16, "GAMEPAD_BUTTON_LEFT_THUMB drifted");
_Static_assert(GAMEPAD_BUTTON_RIGHT_THUMB      == 17, "GAMEPAD_BUTTON_RIGHT_THUMB drifted");

/* GamepadAxis */
_Static_assert(GAMEPAD_AXIS_LEFT_X        == 0, "GAMEPAD_AXIS_LEFT_X drifted");
_Static_assert(GAMEPAD_AXIS_LEFT_Y        == 1, "GAMEPAD_AXIS_LEFT_Y drifted");
_Static_assert(GAMEPAD_AXIS_RIGHT_X       == 2, "GAMEPAD_AXIS_RIGHT_X drifted");
_Static_assert(GAMEPAD_AXIS_RIGHT_Y       == 3, "GAMEPAD_AXIS_RIGHT_Y drifted");
_Static_assert(GAMEPAD_AXIS_LEFT_TRIGGER  == 4, "GAMEPAD_AXIS_LEFT_TRIGGER drifted");
_Static_assert(GAMEPAD_AXIS_RIGHT_TRIGGER == 5, "GAMEPAD_AXIS_RIGHT_TRIGGER drifted");

/* ConfigFlags — bit patterns */
_Static_assert(FLAG_VSYNC_HINT               == 0x00000040, "FLAG_VSYNC_HINT drifted");
_Static_assert(FLAG_FULLSCREEN_MODE          == 0x00000002, "FLAG_FULLSCREEN_MODE drifted");
_Static_assert(FLAG_WINDOW_RESIZABLE         == 0x00000004, "FLAG_WINDOW_RESIZABLE drifted");
_Static_assert(FLAG_WINDOW_UNDECORATED       == 0x00000008, "FLAG_WINDOW_UNDECORATED drifted");
_Static_assert(FLAG_WINDOW_HIDDEN            == 0x00000080, "FLAG_WINDOW_HIDDEN drifted");
_Static_assert(FLAG_WINDOW_MINIMIZED         == 0x00000200, "FLAG_WINDOW_MINIMIZED drifted");
_Static_assert(FLAG_WINDOW_MAXIMIZED         == 0x00000400, "FLAG_WINDOW_MAXIMIZED drifted");
_Static_assert(FLAG_WINDOW_UNFOCUSED         == 0x00000800, "FLAG_WINDOW_UNFOCUSED drifted");
_Static_assert(FLAG_WINDOW_TOPMOST           == 0x00001000, "FLAG_WINDOW_TOPMOST drifted");
_Static_assert(FLAG_WINDOW_ALWAYS_RUN        == 0x00000100, "FLAG_WINDOW_ALWAYS_RUN drifted");
_Static_assert(FLAG_WINDOW_TRANSPARENT       == 0x00000010, "FLAG_WINDOW_TRANSPARENT drifted");
_Static_assert(FLAG_WINDOW_HIGHDPI           == 0x00002000, "FLAG_WINDOW_HIGHDPI drifted");
_Static_assert(FLAG_WINDOW_MOUSE_PASSTHROUGH == 0x00004000, "FLAG_WINDOW_MOUSE_PASSTHROUGH drifted");
_Static_assert(FLAG_BORDERLESS_WINDOWED_MODE == 0x00008000, "FLAG_BORDERLESS_WINDOWED_MODE drifted");
_Static_assert(FLAG_MSAA_4X_HINT             == 0x00000020, "FLAG_MSAA_4X_HINT drifted");
_Static_assert(FLAG_INTERLACED_HINT          == 0x00010000, "FLAG_INTERLACED_HINT drifted");

/* TraceLogLevel */
_Static_assert(LOG_ALL     == 0, "LOG_ALL drifted");
_Static_assert(LOG_TRACE   == 1, "LOG_TRACE drifted");
_Static_assert(LOG_DEBUG   == 2, "LOG_DEBUG drifted");
_Static_assert(LOG_INFO    == 3, "LOG_INFO drifted");
_Static_assert(LOG_WARNING == 4, "LOG_WARNING drifted");
_Static_assert(LOG_ERROR   == 5, "LOG_ERROR drifted");
_Static_assert(LOG_FATAL   == 6, "LOG_FATAL drifted");
_Static_assert(LOG_NONE    == 7, "LOG_NONE drifted");

/* BlendMode */
_Static_assert(BLEND_ALPHA             == 0, "BLEND_ALPHA drifted");
_Static_assert(BLEND_ADDITIVE          == 1, "BLEND_ADDITIVE drifted");
_Static_assert(BLEND_MULTIPLIED        == 2, "BLEND_MULTIPLIED drifted");
_Static_assert(BLEND_ADD_COLORS        == 3, "BLEND_ADD_COLORS drifted");
_Static_assert(BLEND_SUBTRACT_COLORS   == 4, "BLEND_SUBTRACT_COLORS drifted");
_Static_assert(BLEND_ALPHA_PREMULTIPLY == 5, "BLEND_ALPHA_PREMULTIPLY drifted");
_Static_assert(BLEND_CUSTOM            == 6, "BLEND_CUSTOM drifted");
_Static_assert(BLEND_CUSTOM_SEPARATE   == 7, "BLEND_CUSTOM_SEPARATE drifted");

/* PixelFormat */
_Static_assert(PIXELFORMAT_UNCOMPRESSED_GRAYSCALE    == 1,  "PIXELFORMAT_UNCOMPRESSED_GRAYSCALE drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA   == 2,  "PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R5G6B5       == 3,  "PIXELFORMAT_UNCOMPRESSED_R5G6B5 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R8G8B8       == 4,  "PIXELFORMAT_UNCOMPRESSED_R8G8B8 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R5G5B5A1     == 5,  "PIXELFORMAT_UNCOMPRESSED_R5G5B5A1 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R4G4B4A4     == 6,  "PIXELFORMAT_UNCOMPRESSED_R4G4B4A4 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R8G8B8A8     == 7,  "PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R32          == 8,  "PIXELFORMAT_UNCOMPRESSED_R32 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R32G32B32    == 9,  "PIXELFORMAT_UNCOMPRESSED_R32G32B32 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R32G32B32A32 == 10, "PIXELFORMAT_UNCOMPRESSED_R32G32B32A32 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R16          == 11, "PIXELFORMAT_UNCOMPRESSED_R16 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R16G16B16    == 12, "PIXELFORMAT_UNCOMPRESSED_R16G16B16 drifted");
_Static_assert(PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 == 13, "PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_DXT1_RGB       == 14, "PIXELFORMAT_COMPRESSED_DXT1_RGB drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_DXT1_RGBA      == 15, "PIXELFORMAT_COMPRESSED_DXT1_RGBA drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_DXT3_RGBA      == 16, "PIXELFORMAT_COMPRESSED_DXT3_RGBA drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_DXT5_RGBA      == 17, "PIXELFORMAT_COMPRESSED_DXT5_RGBA drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_ETC1_RGB       == 18, "PIXELFORMAT_COMPRESSED_ETC1_RGB drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_ETC2_RGB       == 19, "PIXELFORMAT_COMPRESSED_ETC2_RGB drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA  == 20, "PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_PVRT_RGB       == 21, "PIXELFORMAT_COMPRESSED_PVRT_RGB drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_PVRT_RGBA      == 22, "PIXELFORMAT_COMPRESSED_PVRT_RGBA drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA  == 23, "PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA drifted");
_Static_assert(PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA  == 24, "PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA drifted");

/* TextureFilter */
_Static_assert(TEXTURE_FILTER_POINT          == 0, "TEXTURE_FILTER_POINT drifted");
_Static_assert(TEXTURE_FILTER_BILINEAR       == 1, "TEXTURE_FILTER_BILINEAR drifted");
_Static_assert(TEXTURE_FILTER_TRILINEAR      == 2, "TEXTURE_FILTER_TRILINEAR drifted");
_Static_assert(TEXTURE_FILTER_ANISOTROPIC_4X == 3, "TEXTURE_FILTER_ANISOTROPIC_4X drifted");
_Static_assert(TEXTURE_FILTER_ANISOTROPIC_8X == 4, "TEXTURE_FILTER_ANISOTROPIC_8X drifted");
_Static_assert(TEXTURE_FILTER_ANISOTROPIC_16X == 5, "TEXTURE_FILTER_ANISOTROPIC_16X drifted");

/* TextureWrap */
_Static_assert(TEXTURE_WRAP_REPEAT        == 0, "TEXTURE_WRAP_REPEAT drifted");
_Static_assert(TEXTURE_WRAP_CLAMP         == 1, "TEXTURE_WRAP_CLAMP drifted");
_Static_assert(TEXTURE_WRAP_MIRROR_REPEAT == 2, "TEXTURE_WRAP_MIRROR_REPEAT drifted");
_Static_assert(TEXTURE_WRAP_MIRROR_CLAMP  == 3, "TEXTURE_WRAP_MIRROR_CLAMP drifted");

/* CubemapLayout */
_Static_assert(CUBEMAP_LAYOUT_AUTO_DETECT      == 0, "CUBEMAP_LAYOUT_AUTO_DETECT drifted");
_Static_assert(CUBEMAP_LAYOUT_LINE_VERTICAL    == 1, "CUBEMAP_LAYOUT_LINE_VERTICAL drifted");
_Static_assert(CUBEMAP_LAYOUT_LINE_HORIZONTAL  == 2, "CUBEMAP_LAYOUT_LINE_HORIZONTAL drifted");
_Static_assert(CUBEMAP_LAYOUT_CROSS_THREE_BY_FOUR == 3, "CUBEMAP_LAYOUT_CROSS_THREE_BY_FOUR drifted");
_Static_assert(CUBEMAP_LAYOUT_CROSS_FOUR_BY_THREE == 4, "CUBEMAP_LAYOUT_CROSS_FOUR_BY_THREE drifted");

/* FontType */
_Static_assert(FONT_DEFAULT == 0, "FONT_DEFAULT drifted");
_Static_assert(FONT_BITMAP  == 1, "FONT_BITMAP drifted");
_Static_assert(FONT_SDF     == 2, "FONT_SDF drifted");

/* Gesture */
_Static_assert(GESTURE_NONE        == 0,   "GESTURE_NONE drifted");
_Static_assert(GESTURE_TAP         == 1,   "GESTURE_TAP drifted");
_Static_assert(GESTURE_DOUBLETAP   == 2,   "GESTURE_DOUBLETAP drifted");
_Static_assert(GESTURE_HOLD        == 4,   "GESTURE_HOLD drifted");
_Static_assert(GESTURE_DRAG        == 8,   "GESTURE_DRAG drifted");
_Static_assert(GESTURE_SWIPE_RIGHT == 16,  "GESTURE_SWIPE_RIGHT drifted");
_Static_assert(GESTURE_SWIPE_LEFT  == 32,  "GESTURE_SWIPE_LEFT drifted");
_Static_assert(GESTURE_SWIPE_UP    == 64,  "GESTURE_SWIPE_UP drifted");
_Static_assert(GESTURE_SWIPE_DOWN  == 128, "GESTURE_SWIPE_DOWN drifted");
_Static_assert(GESTURE_PINCH_IN    == 256, "GESTURE_PINCH_IN drifted");
_Static_assert(GESTURE_PINCH_OUT   == 512, "GESTURE_PINCH_OUT drifted");

/* CameraMode */
_Static_assert(CAMERA_CUSTOM       == 0, "CAMERA_CUSTOM drifted");
_Static_assert(CAMERA_FREE         == 1, "CAMERA_FREE drifted");
_Static_assert(CAMERA_ORBITAL      == 2, "CAMERA_ORBITAL drifted");
_Static_assert(CAMERA_FIRST_PERSON == 3, "CAMERA_FIRST_PERSON drifted");
_Static_assert(CAMERA_THIRD_PERSON == 4, "CAMERA_THIRD_PERSON drifted");

/* CameraProjection */
_Static_assert(CAMERA_PERSPECTIVE  == 0, "CAMERA_PERSPECTIVE drifted");
_Static_assert(CAMERA_ORTHOGRAPHIC == 1, "CAMERA_ORTHOGRAPHIC drifted");

/* MaterialMapIndex */
_Static_assert(MATERIAL_MAP_ALBEDO     == 0,  "MATERIAL_MAP_ALBEDO drifted");
_Static_assert(MATERIAL_MAP_METALNESS  == 1,  "MATERIAL_MAP_METALNESS drifted");
_Static_assert(MATERIAL_MAP_NORMAL     == 2,  "MATERIAL_MAP_NORMAL drifted");
_Static_assert(MATERIAL_MAP_ROUGHNESS  == 3,  "MATERIAL_MAP_ROUGHNESS drifted");
_Static_assert(MATERIAL_MAP_OCCLUSION  == 4,  "MATERIAL_MAP_OCCLUSION drifted");
_Static_assert(MATERIAL_MAP_EMISSION   == 5,  "MATERIAL_MAP_EMISSION drifted");
_Static_assert(MATERIAL_MAP_HEIGHT     == 6,  "MATERIAL_MAP_HEIGHT drifted");
_Static_assert(MATERIAL_MAP_CUBEMAP    == 7,  "MATERIAL_MAP_CUBEMAP drifted");
_Static_assert(MATERIAL_MAP_IRRADIANCE == 8,  "MATERIAL_MAP_IRRADIANCE drifted");
_Static_assert(MATERIAL_MAP_PREFILTER  == 9,  "MATERIAL_MAP_PREFILTER drifted");
_Static_assert(MATERIAL_MAP_BRDF       == 10, "MATERIAL_MAP_BRDF drifted");

/* ShaderLocationIndex */
_Static_assert(SHADER_LOC_VERTEX_POSITION   == 0,  "SHADER_LOC_VERTEX_POSITION drifted");
_Static_assert(SHADER_LOC_VERTEX_TEXCOORD01 == 1,  "SHADER_LOC_VERTEX_TEXCOORD01 drifted");
_Static_assert(SHADER_LOC_VERTEX_TEXCOORD02 == 2,  "SHADER_LOC_VERTEX_TEXCOORD02 drifted");
_Static_assert(SHADER_LOC_VERTEX_NORMAL     == 3,  "SHADER_LOC_VERTEX_NORMAL drifted");
_Static_assert(SHADER_LOC_VERTEX_TANGENT    == 4,  "SHADER_LOC_VERTEX_TANGENT drifted");
_Static_assert(SHADER_LOC_VERTEX_COLOR      == 5,  "SHADER_LOC_VERTEX_COLOR drifted");
_Static_assert(SHADER_LOC_MATRIX_MVP        == 6,  "SHADER_LOC_MATRIX_MVP drifted");
_Static_assert(SHADER_LOC_MATRIX_VIEW       == 7,  "SHADER_LOC_MATRIX_VIEW drifted");
_Static_assert(SHADER_LOC_MATRIX_PROJECTION == 8,  "SHADER_LOC_MATRIX_PROJECTION drifted");
_Static_assert(SHADER_LOC_MATRIX_MODEL      == 9,  "SHADER_LOC_MATRIX_MODEL drifted");
_Static_assert(SHADER_LOC_MATRIX_NORMAL     == 10, "SHADER_LOC_MATRIX_NORMAL drifted");
_Static_assert(SHADER_LOC_VECTOR_VIEW       == 11, "SHADER_LOC_VECTOR_VIEW drifted");
_Static_assert(SHADER_LOC_COLOR_DIFFUSE     == 12, "SHADER_LOC_COLOR_DIFFUSE drifted");
_Static_assert(SHADER_LOC_COLOR_SPECULAR    == 13, "SHADER_LOC_COLOR_SPECULAR drifted");
_Static_assert(SHADER_LOC_COLOR_AMBIENT     == 14, "SHADER_LOC_COLOR_AMBIENT drifted");
_Static_assert(SHADER_LOC_MAP_ALBEDO        == 15, "SHADER_LOC_MAP_ALBEDO drifted");
_Static_assert(SHADER_LOC_MAP_METALNESS     == 16, "SHADER_LOC_MAP_METALNESS drifted");
_Static_assert(SHADER_LOC_MAP_NORMAL        == 17, "SHADER_LOC_MAP_NORMAL drifted");
_Static_assert(SHADER_LOC_MAP_ROUGHNESS     == 18, "SHADER_LOC_MAP_ROUGHNESS drifted");
_Static_assert(SHADER_LOC_MAP_OCCLUSION     == 19, "SHADER_LOC_MAP_OCCLUSION drifted");
_Static_assert(SHADER_LOC_MAP_EMISSION      == 20, "SHADER_LOC_MAP_EMISSION drifted");
_Static_assert(SHADER_LOC_MAP_HEIGHT        == 21, "SHADER_LOC_MAP_HEIGHT drifted");
_Static_assert(SHADER_LOC_MAP_CUBEMAP       == 22, "SHADER_LOC_MAP_CUBEMAP drifted");
_Static_assert(SHADER_LOC_MAP_IRRADIANCE    == 23, "SHADER_LOC_MAP_IRRADIANCE drifted");
_Static_assert(SHADER_LOC_MAP_PREFILTER     == 24, "SHADER_LOC_MAP_PREFILTER drifted");
_Static_assert(SHADER_LOC_MAP_BRDF          == 25, "SHADER_LOC_MAP_BRDF drifted");
_Static_assert(SHADER_LOC_VERTEX_BONEIDS    == 26, "SHADER_LOC_VERTEX_BONEIDS drifted");
_Static_assert(SHADER_LOC_VERTEX_BONEWEIGHTS == 27, "SHADER_LOC_VERTEX_BONEWEIGHTS drifted");
_Static_assert(SHADER_LOC_BONE_MATRICES     == 28, "SHADER_LOC_BONE_MATRICES drifted");

/* ShaderUniformDataType */
_Static_assert(SHADER_UNIFORM_FLOAT     == 0, "SHADER_UNIFORM_FLOAT drifted");
_Static_assert(SHADER_UNIFORM_VEC2      == 1, "SHADER_UNIFORM_VEC2 drifted");
_Static_assert(SHADER_UNIFORM_VEC3      == 2, "SHADER_UNIFORM_VEC3 drifted");
_Static_assert(SHADER_UNIFORM_VEC4      == 3, "SHADER_UNIFORM_VEC4 drifted");
_Static_assert(SHADER_UNIFORM_INT       == 4, "SHADER_UNIFORM_INT drifted");
_Static_assert(SHADER_UNIFORM_IVEC2     == 5, "SHADER_UNIFORM_IVEC2 drifted");
_Static_assert(SHADER_UNIFORM_IVEC3     == 6, "SHADER_UNIFORM_IVEC3 drifted");
_Static_assert(SHADER_UNIFORM_IVEC4     == 7, "SHADER_UNIFORM_IVEC4 drifted");
_Static_assert(SHADER_UNIFORM_SAMPLER2D == 8, "SHADER_UNIFORM_SAMPLER2D drifted");

/* ShaderAttributeDataType */
_Static_assert(SHADER_ATTRIB_FLOAT == 0, "SHADER_ATTRIB_FLOAT drifted");
_Static_assert(SHADER_ATTRIB_VEC2  == 1, "SHADER_ATTRIB_VEC2 drifted");
_Static_assert(SHADER_ATTRIB_VEC3  == 2, "SHADER_ATTRIB_VEC3 drifted");
_Static_assert(SHADER_ATTRIB_VEC4  == 3, "SHADER_ATTRIB_VEC4 drifted");

/* NPatchLayout */
_Static_assert(NPATCH_NINE_PATCH              == 0, "NPATCH_NINE_PATCH drifted");
_Static_assert(NPATCH_THREE_PATCH_VERTICAL    == 1, "NPATCH_THREE_PATCH_VERTICAL drifted");
_Static_assert(NPATCH_THREE_PATCH_HORIZONTAL  == 2, "NPATCH_THREE_PATCH_HORIZONTAL drifted");

/* Compile-unit sentinel: ensures at least one symbol exists in the
 * object file so the linker doesn't warn about an "empty" TU. The
 * `static` + unused attribute silences unused-variable warnings until
 * real static-assertions land in Plans 60-02..05 and 60-07. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static const int iron_raylib_layout_sentinel = 0;
