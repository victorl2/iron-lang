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

/* Layout-compatible mirror of raylib `Vector2` — 2 floats. */
struct Iron_Vector2 {
    float x;
    float y;
};

/* Layout-compatible mirror of raylib `Vector3` — 3 floats. */
struct Iron_Vector3 {
    float x;
    float y;
    float z;
};

/* Layout-compatible mirror of raylib `Vector4` — 4 floats. */
struct Iron_Vector4 {
    float x;
    float y;
    float z;
    float w;
};

/* Layout-compatible mirror of raylib `Quaternion` (which is a
 * typedef for Vector4 in raylib.h). Same 4-float layout; a distinct
 * Iron type name to preserve math-domain meaning in Iron signatures. */
struct Iron_Quaternion {
    float x;
    float y;
    float z;
    float w;
};

/* Layout-compatible mirror of raylib `Matrix` — 16 floats, column-major.
 * raylib.h orders the fields m0 m4 m8 m12 m1 m5 m9 m13 m2 m6 m10 m14
 * m3 m7 m11 m15, which is the same order used here. */
struct Iron_Matrix {
    float m0, m4, m8, m12;
    float m1, m5, m9, m13;
    float m2, m6, m10, m14;
    float m3, m7, m11, m15;
};

/* raymath helper types (Phase 65 Plan 03) — byte-identical to raymath's
 * float3 (12 B) / float16 (64 B). See raymath.h lines 163-169.
 * C adjacent-same-size-float contiguity guarantees these mirror
 * `struct float3 { float v[3]; }` / `struct float16 { float v[16]; }`. */
struct Iron_Float3 {
    float x, y, z;
};

struct Iron_Float16 {
    float m0,  m1,  m2,  m3;
    float m4,  m5,  m6,  m7;
    float m8,  m9,  m10, m11;
    float m12, m13, m14, m15;
};

/* Layout-compatible mirror of raylib `Rectangle` — 4 floats. */
struct Iron_Rectangle {
    float x;
    float y;
    float width;
    float height;
};

/* Layout-compatible mirror of raylib `Color` — 4 unsigned chars, RGBA8888. */
struct Iron_Color {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char a;
};

/* ── Image / Texture / Font types (Plan 60-03) ────────────────────── */

/* CPU image. `_data` is an opaque pointer-as-int64 on the Iron side;
 * on the C side we declare it as `void *` so offsetof matches the
 * raylib `Image.data` field. Layout-compat relies on:
 *   sizeof(int64_t) == sizeof(void *)        [holds on every 64-bit target]
 * If Iron ever supports 32-bit targets, this assertion in
 * iron_raylib_layout.c will fail and we revisit. */
struct Iron_Image {
    void *_data;
    int width;
    int height;
    int mipmaps;
    int format;
};

/* GPU texture — raylib.h `Texture { unsigned int id; int width; ... }`. */
struct Iron_Texture {
    unsigned int id;
    int width;
    int height;
    int mipmaps;
    int format;
};

/* Render texture — embedded Iron_Texture by value. */
struct Iron_RenderTexture {
    unsigned int id;
    struct Iron_Texture texture;
    struct Iron_Texture depth;
};

/* N-patch info — embedded Iron_Rectangle (from Plan 60-02) by value. */
struct Iron_NPatchInfo {
    struct Iron_Rectangle source;
    int left;
    int top;
    int right;
    int bottom;
    int layout;
};

/* Glyph info — embedded Iron_Image by value. */
struct Iron_GlyphInfo {
    int value;
    int offsetX;
    int offsetY;
    int advanceX;
    struct Iron_Image image;
};

/* Font atlas — embedded Iron_Texture plus opaque Rectangle* / GlyphInfo*
 * pointers mirrored as void * to preserve pointer-sized offsets. */
struct Iron_Font {
    int baseSize;
    int glyphCount;
    int glyphPadding;
    struct Iron_Texture texture;
    void *_recs;
    void *_glyphs;
};

/* ── Camera / Mesh / Model types (Plan 60-04) ─────────────────────── */

/* Iron_Camera mirrors raylib `Camera2D` (naming inversion). */
struct Iron_Camera {
    struct Iron_Vector2 offset;
    struct Iron_Vector2 target;
    float rotation;
    float zoom;
};

/* Iron_Camera3D mirrors raylib `Camera3D` (and raylib's `Camera`
 * typedef). */
struct Iron_Camera3D {
    struct Iron_Vector3 position;
    struct Iron_Vector3 target;
    struct Iron_Vector3 up;
    float fovy;
    int projection;
};

/* Transform — embeds Vector3 / Quaternion / Vector3. */
struct Iron_Transform {
    struct Iron_Vector3   translation;
    struct Iron_Quaternion rotation;
    struct Iron_Vector3   scale;
};

/* BoneInfo — inline char[32] matches raylib exactly. The flattened
 * _name_00 .. _name_31 Iron fields land at the same offsets (0..31). */
struct Iron_BoneInfo {
    char name[32];
    int  parent;
};

/* Mesh — 14 pointer fields + plain `vaoId` + 1 pointer-array. */
struct Iron_Mesh {
    int            vertexCount;
    int            triangleCount;
    float         *_vertices;
    float         *_texcoords;
    float         *_texcoords2;
    float         *_normals;
    float         *_tangents;
    unsigned char *_colors;
    unsigned short*_indices;
    float         *_animVertices;
    float         *_animNormals;
    unsigned char *_boneIds;
    float         *_boneWeights;
    struct Iron_Matrix *_boneMatrices;
    int            boneCount;
    unsigned int   vaoId;
    unsigned int  *_vboId;
};

/* Shader — unsigned int id + opaque int* locs. */
struct Iron_Shader {
    unsigned int id;
    int         *_locs;
};

/* MaterialMap — embedded Texture + Color + float. */
struct Iron_MaterialMap {
    struct Iron_Texture texture;
    struct Iron_Color   color;
    float               value;
};

/* Material — embedded Shader + opaque MaterialMap* + inline float[4]. */
struct Iron_Material {
    struct Iron_Shader       shader;
    struct Iron_MaterialMap *_maps;
    float                    params[4];
};

/* Model — embedded Matrix + 5 opaque pointer fields. */
struct Iron_Model {
    struct Iron_Matrix transform;
    int                meshCount;
    int                materialCount;
    struct Iron_Mesh      *_meshes;
    struct Iron_Material  *_materials;
    int                   *_meshMaterial;
    int                    boneCount;
    struct Iron_BoneInfo  *_bones;
    struct Iron_Transform *_bindPose;
};

/* ModelAnimation — 2 pointers, double-indirect framePoses, and inline
 * char[32] name at the end. */
struct Iron_ModelAnimation {
    int                     boneCount;
    int                     frameCount;
    struct Iron_BoneInfo   *_bones;
    struct Iron_Transform **_framePoses;
    char                    name[32];
};

/* ── 3D helpers / audio / file types (Plan 60-05) ─────────────────── */

/* Ray — origin + direction. */
struct Iron_Ray {
    struct Iron_Vector3 position;
    struct Iron_Vector3 direction;
};

/* RayCollision — `hit` is a stdbool.h `bool` (1 byte).
 * C compiler inserts padding before `distance` to 4-byte alignment,
 * so offsetof(distance) is typically 4. The _Static_assert in
 * iron_raylib_layout.c verifies this matches Iron's `Bool` layout. */
struct Iron_RayCollision {
    bool                 hit;
    float                distance;
    struct Iron_Vector3  point;
    struct Iron_Vector3  normal;
};

/* BoundingBox — min + max corners. */
struct Iron_BoundingBox {
    struct Iron_Vector3 min;
    struct Iron_Vector3 max;
};

/* Wave — 4 unsigned ints + opaque data pointer. */
struct Iron_Wave {
    unsigned int frameCount;
    unsigned int sampleRate;
    unsigned int sampleSize;
    unsigned int channels;
    void        *_data;
};

/* AudioStream — 2 opaque raylib-internal pointers + 3 unsigned ints.
 * Note: the C field types are `rAudioBuffer *` and
 * `rAudioProcessor *`. We use `void *` in the Iron mirror because
 * the opaque types have no definition visible to Iron, and `void *`
 * has the same size/alignment as any pointer on every target. */
struct Iron_AudioStream {
    void        *_buffer;
    void        *_processor;
    unsigned int sampleRate;
    unsigned int sampleSize;
    unsigned int channels;
};

/* Sound — embedded AudioStream by value. */
struct Iron_Sound {
    struct Iron_AudioStream stream;
    unsigned int            frameCount;
};

/* Music — embedded AudioStream, metadata, `looping` bool, and opaque
 * ctxData pointer. */
struct Iron_Music {
    struct Iron_AudioStream stream;
    unsigned int            frameCount;
    bool                    looping;
    int                     ctxType;
    void                   *_ctxData;
};

/* FilePathList — `_paths` mirrors raylib's `char **paths`. */
struct Iron_FilePathList {
    unsigned int capacity;
    unsigned int count;
    void        *_paths;
};

/* ════════════════════════════════════════════════════════════════════
 * Shim wrapper function prototypes — added by Phases 61–72.
 * Phase 60 declares no functions; the file is populated incrementally
 * as each binding phase lands its subset.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Window & System (Phase 61) ───────────────────────────────────── */

/* Window lifecycle (WIN-01) */
void Iron_window_init(int32_t w, int32_t h, Iron_String title);
void Iron_window_close(void);
bool Iron_window_should_close(void);

/* Window state queries (WIN-02) */
bool Iron_window_is_ready(void);
bool Iron_window_is_fullscreen(void);
bool Iron_window_is_hidden(void);
bool Iron_window_is_minimized(void);
bool Iron_window_is_maximized(void);
bool Iron_window_is_focused(void);
bool Iron_window_is_resized(void);

/* Window state toggles (WIN-03) */
void Iron_window_toggle_fullscreen(void);
void Iron_window_toggle_borderless_windowed(void);
void Iron_window_maximize(void);
void Iron_window_minimize(void);
void Iron_window_restore(void);
void Iron_window_set_state(uint32_t flags);
void Iron_window_clear_state(uint32_t flags);
bool Iron_window_is_state(uint32_t flag);

/* Window runtime properties (WIN-04) */
void Iron_window_set_icon(struct Iron_Image image);
void Iron_window_set_title(Iron_String title);
void Iron_window_set_position(int32_t x, int32_t y);
void Iron_window_set_monitor(int32_t monitor);
void Iron_window_set_min_size(int32_t w, int32_t h);
void Iron_window_set_max_size(int32_t w, int32_t h);
void Iron_window_set_size(int32_t w, int32_t h);
void Iron_window_set_opacity(float opacity);
void Iron_window_set_focused(void);

/* Screen and window geometry (WIN-05) */
int32_t Iron_window_get_screen_width(void);
int32_t Iron_window_get_screen_height(void);
int32_t Iron_window_get_render_width(void);
int32_t Iron_window_get_render_height(void);
struct Iron_Vector2 Iron_window_get_window_position(void);
struct Iron_Vector2 Iron_window_get_window_scale_dpi(void);

/* Monitor enumeration (WIN-06) */
int32_t Iron_window_get_monitor_count(void);
int32_t Iron_window_get_current_monitor(void);
struct Iron_Vector2 Iron_window_get_monitor_position(int32_t monitor);
int32_t Iron_window_get_monitor_width(int32_t monitor);
int32_t Iron_window_get_monitor_height(int32_t monitor);
int32_t Iron_window_get_monitor_physical_width(int32_t monitor);
int32_t Iron_window_get_monitor_physical_height(int32_t monitor);
int32_t Iron_window_get_monitor_refresh_rate(int32_t monitor);
Iron_String Iron_window_get_monitor_name(int32_t monitor);

/* Clipboard (WIN-07) */
Iron_String Iron_window_get_clipboard_text(void);
void Iron_window_set_clipboard_text(Iron_String text);
struct Iron_Image Iron_window_get_clipboard_image(void);

/* Screenshot (WIN-09) */
void Iron_window_take_screenshot(Iron_String filename);
struct Iron_Image Iron_window_load_image_from_screen(void);

/* Config flags + trace log + event waiting (WIN-08, WIN-10) */
void Iron_window_set_config_flags(uint32_t flags);
void Iron_window_set_trace_log_level(int32_t level);
void Iron_window_enable_event_waiting(void);
void Iron_window_disable_event_waiting(void);

/* Cursor (WIN-11) */
void Iron_window_show_cursor(void);
void Iron_window_hide_cursor(void);
bool Iron_window_is_cursor_hidden(void);
void Iron_window_enable_cursor(void);
void Iron_window_disable_cursor(void);
bool Iron_window_is_cursor_on_screen(void);
void Iron_window_set_mouse_cursor(int32_t cursor);

/* Frame loop (WIN-12) */
void    Iron_window_set_target_fps(int32_t fps);
int32_t Iron_window_get_fps(void);
float   Iron_window_get_frame_time(void);
double  Iron_window_get_time(void);

/* URL (WIN-13) */
void Iron_window_open_url(Iron_String url);

/* Wait (FILE-07) */
void Iron_window_wait_time(double seconds);

/* ── Input (Phase 62) ─────────────────────────────────────────────── */

/* Keyboard (INPUT-01, INPUT-02, INPUT-03) */
bool    Iron_keyboard_is_pressed(int32_t key);
bool    Iron_keyboard_is_pressed_repeat(int32_t key);
bool    Iron_keyboard_is_down(int32_t key);
bool    Iron_keyboard_is_released(int32_t key);
bool    Iron_keyboard_is_up(int32_t key);
int32_t Iron_keyboard_get_pressed(void);     /* returns KeyboardKey ordinal or 0 (NULL) */
int32_t Iron_keyboard_get_char_pressed(void); /* returns unicode codepoint or 0 — NOT an enum */
void    Iron_keyboard_set_exit_key(int32_t key);

/* Mouse (INPUT-04, INPUT-05, INPUT-06, INPUT-07) */
bool                 Iron_mouse_is_button_pressed(int32_t button);
bool                 Iron_mouse_is_button_down(int32_t button);
bool                 Iron_mouse_is_button_released(int32_t button);
bool                 Iron_mouse_is_button_up(int32_t button);
int32_t              Iron_mouse_get_x(void);
int32_t              Iron_mouse_get_y(void);
struct Iron_Vector2  Iron_mouse_get_position(void);
struct Iron_Vector2  Iron_mouse_get_delta(void);
void                 Iron_mouse_set_position(int32_t x, int32_t y);
void                 Iron_mouse_set_offset(int32_t offset_x, int32_t offset_y);
void                 Iron_mouse_set_scale(float scale_x, float scale_y);
float                Iron_mouse_get_wheel_move(void);
struct Iron_Vector2  Iron_mouse_get_wheel_move_v(void);
void                 Iron_mouse_set_cursor(int32_t cursor);

/* Gamepad (INPUT-08, INPUT-09, INPUT-10) */
bool         Iron_gamepad_is_available(int32_t gamepad);
const char * Iron_gamepad_get_name(int32_t gamepad);
bool         Iron_gamepad_is_button_pressed(int32_t gamepad, int32_t button);
bool         Iron_gamepad_is_button_down(int32_t gamepad, int32_t button);
bool         Iron_gamepad_is_button_released(int32_t gamepad, int32_t button);
bool         Iron_gamepad_is_button_up(int32_t gamepad, int32_t button);
int32_t      Iron_gamepad_get_button_pressed(void);   /* returns GamepadButton ordinal or 0 (UNKNOWN) */
int32_t      Iron_gamepad_get_axis_count(int32_t gamepad);
float        Iron_gamepad_get_axis_movement(int32_t gamepad, int32_t axis);
int32_t      Iron_gamepad_set_mappings(const char * mappings);
void         Iron_gamepad_set_vibration(int32_t gamepad, float left_motor, float right_motor, float duration);

/* Touch (INPUT-11) */
int32_t              Iron_touch_get_x(void);
int32_t              Iron_touch_get_y(void);
struct Iron_Vector2  Iron_touch_get_position(int32_t index);
int32_t              Iron_touch_get_point_id(int32_t index);
int32_t              Iron_touch_get_point_count(void);

/* Gestures (INPUT-12) — raylib uses `unsigned int` bitmask for SetGesturesEnabled
 * and IsGestureDetected; Iron-side stubs take the typed Gesture enum (which
 * lowers to int32_t at the FFI boundary, consistent with every other enum
 * parameter in the Phase 61/62 shim surface). The shim casts int32_t to
 * unsigned int. Multi-gesture masks are built by OR-ing Gesture ordinals
 * at the Iron call site — Iron's bitwise-OR on enum values (Phase 59
 * Bitwise Operators landed the underlying support; enum OR ergonomics may
 * need Phase 73 polish but the type is still `Gesture`, not raw int).
 *
 * Namespace is PLURAL (`Gestures`) — locked by Plan 62-01 after ironc's
 * E0201 collision between `object Gesture {}` and `enum Gesture {}`. The
 * Iron-side enum `Gesture` is unchanged; only the namespace is plural. */
void                 Iron_gestures_set_enabled(int32_t flags);
bool                 Iron_gestures_is_detected(int32_t gesture);
int32_t              Iron_gestures_get_detected(void);  /* returns Gesture ordinal or 0 (NONE) */
float                Iron_gestures_get_hold_duration(void);
struct Iron_Vector2  Iron_gestures_get_drag_vector(void);
float                Iron_gestures_get_drag_angle(void);
struct Iron_Vector2  Iron_gestures_get_pinch_vector(void);
float                Iron_gestures_get_pinch_angle(void);

/* File drop (INPUT-13) */
bool                 Iron_files_is_dropped(void);
struct Iron_FilePathList Iron_files_load_dropped(void);
void                 Iron_files_unload_dropped(struct Iron_FilePathList files);

/* FilePathList accessors (INPUT-13 iteration clause) */
int32_t              Iron_filepathlist_count(struct Iron_FilePathList list);
const char *         Iron_filepathlist_get(struct Iron_FilePathList list, int32_t index);

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

/* Line primitives (DRAW2D-08) — DrawLineStrip deferred to Plan 63-04 (array ABI) */
void Iron_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, struct Iron_Color color);
void Iron_draw_line_v(struct Iron_Vector2 start, struct Iron_Vector2 end, struct Iron_Color color);
void Iron_draw_line_ex(struct Iron_Vector2 start, struct Iron_Vector2 end, float thick, struct Iron_Color color);
void Iron_draw_line_bezier(struct Iron_Vector2 start, struct Iron_Vector2 end, float thick, struct Iron_Color color);

/* Circle primitives (DRAW2D-09) */
void Iron_draw_circle(int32_t cx, int32_t cy, float r, struct Iron_Color color);
void Iron_draw_circle_sector(struct Iron_Vector2 center, float r, float start, float end, int32_t segments, struct Iron_Color color);
void Iron_draw_circle_sector_lines(struct Iron_Vector2 center, float r, float start, float end, int32_t segments, struct Iron_Color color);
void Iron_draw_circle_gradient(int32_t cx, int32_t cy, float r, struct Iron_Color inner, struct Iron_Color outer);
void Iron_draw_circle_v(struct Iron_Vector2 center, float r, struct Iron_Color color);
void Iron_draw_circle_lines(int32_t cx, int32_t cy, float r, struct Iron_Color color);
void Iron_draw_circle_lines_v(struct Iron_Vector2 center, float r, struct Iron_Color color);

/* Ellipse primitives (DRAW2D-10) */
void Iron_draw_ellipse(int32_t cx, int32_t cy, float rh, float rv, struct Iron_Color color);
void Iron_draw_ellipse_lines(int32_t cx, int32_t cy, float rh, float rv, struct Iron_Color color);

/* Ring primitives (DRAW2D-11) */
void Iron_draw_ring(struct Iron_Vector2 center, float inner_r, float outer_r, float start, float end, int32_t segments, struct Iron_Color color);
void Iron_draw_ring_lines(struct Iron_Vector2 center, float inner_r, float outer_r, float start, float end, int32_t segments, struct Iron_Color color);

/* Rectangle primitives (DRAW2D-12) — 12 functions */
void Iron_draw_rectangle(int32_t x, int32_t y, int32_t w, int32_t h, struct Iron_Color color);
void Iron_draw_rectangle_v(struct Iron_Vector2 position, struct Iron_Vector2 size, struct Iron_Color color);
void Iron_draw_rectangle_rec(struct Iron_Rectangle rec, struct Iron_Color color);
void Iron_draw_rectangle_pro(struct Iron_Rectangle rec, struct Iron_Vector2 origin, float rotation, struct Iron_Color color);
void Iron_draw_rectangle_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h, struct Iron_Color top, struct Iron_Color bottom);
void Iron_draw_rectangle_gradient_h(int32_t x, int32_t y, int32_t w, int32_t h, struct Iron_Color left, struct Iron_Color right);
void Iron_draw_rectangle_gradient_ex(struct Iron_Rectangle rec, struct Iron_Color tl, struct Iron_Color bl, struct Iron_Color tr, struct Iron_Color br);
void Iron_draw_rectangle_lines(int32_t x, int32_t y, int32_t w, int32_t h, struct Iron_Color color);
void Iron_draw_rectangle_lines_ex(struct Iron_Rectangle rec, float thick, struct Iron_Color color);
void Iron_draw_rectangle_rounded(struct Iron_Rectangle rec, float roundness, int32_t segments, struct Iron_Color color);
void Iron_draw_rectangle_rounded_lines(struct Iron_Rectangle rec, float roundness, int32_t segments, struct Iron_Color color);
void Iron_draw_rectangle_rounded_lines_ex(struct Iron_Rectangle rec, float roundness, int32_t segments, float thick, struct Iron_Color color);

/* Triangle primitives (DRAW2D-13) — 2 fixed-point variants; 2 array
 * variants (DrawTriangleFan / DrawTriangleStrip) follow below after
 * the Iron_List_Iron_Vector2 struct declaration. Plan 63-03 Task 1
 * probe outcome A: ironc lowers `[Vector2]` parameters to
 * `Iron_List_Iron_Vector2` struct-by-value (items/count/capacity
 * wrapper, 24 bytes). */
void Iron_draw_triangle(struct Iron_Vector2 v1, struct Iron_Vector2 v2, struct Iron_Vector2 v3, struct Iron_Color color);
void Iron_draw_triangle_lines(struct Iron_Vector2 v1, struct Iron_Vector2 v2, struct Iron_Vector2 v3, struct Iron_Color color);

/* Polygon primitives (DRAW2D-14) — 3 functions */
void Iron_draw_poly(struct Iron_Vector2 center, int32_t sides, float r, float rotation, struct Iron_Color color);
void Iron_draw_poly_lines(struct Iron_Vector2 center, int32_t sides, float r, float rotation, struct Iron_Color color);
void Iron_draw_poly_lines_ex(struct Iron_Vector2 center, int32_t sides, float r, float rotation, float thick, struct Iron_Color color);

/* Iron's [Vector2] lowers to Iron_List_Iron_Vector2 in C (ARRAY_PARAM_LIST
 * mode, confirmed by Plan 63-03 Task 1 probe). Layout-compatible with
 * the compiler-emitted IRON_LIST_DECL expansion. Same guard pattern as
 * iron_net.h's Iron_List_Iron_Address block. */
#ifndef IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED
#define IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED
typedef struct Iron_List_Iron_Vector2 {
    struct Iron_Vector2 *items;
    int64_t              count;
    int64_t              capacity;
} Iron_List_Iron_Vector2;
#endif

/* Triangle array variants (DRAW2D-13 — array ABI confirmed by Task 1 probe).
 * ironc passes Iron_List_Iron_Vector2 by VALUE (not by pointer), so the
 * shim receives the items/count/capacity wrapper directly. Reinterpret
 * .items (Iron_Vector2 * byte-identical to raylib Vector2 per Phase 60-02
 * _Static_assert) as `const Vector2 *` before forwarding to raylib. */
void Iron_draw_triangle_fan(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color);
void Iron_draw_triangle_strip(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color);

/* Spline segment primitives (DRAW2D-15 — fixed-point-count variants) */
void Iron_draw_spline_segment_linear(struct Iron_Vector2 p1, struct Iron_Vector2 p2, float thick, struct Iron_Color color);
void Iron_draw_spline_segment_basis(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float thick, struct Iron_Color color);
void Iron_draw_spline_segment_catmull_rom(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float thick, struct Iron_Color color);
void Iron_draw_spline_segment_bezier_quadratic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 p3, float thick, struct Iron_Color color);
void Iron_draw_spline_segment_bezier_cubic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 c3, struct Iron_Vector2 p4, float thick, struct Iron_Color color);

/* Spline evaluators (DRAW2D-16) — Vector2 RETURN. Iron-side name
 * normalizes raylib's C name `BezierQuad` to `bezier_quadratic`
 * for symmetry with the draw side's `DrawSplineBezierQuadratic`. */
struct Iron_Vector2 Iron_draw_get_spline_point_linear(struct Iron_Vector2 start, struct Iron_Vector2 end, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_basis(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_catmull_rom(struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3, struct Iron_Vector2 p4, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_bezier_quadratic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 p3, float t);
struct Iron_Vector2 Iron_draw_get_spline_point_bezier_cubic(struct Iron_Vector2 p1, struct Iron_Vector2 c2, struct Iron_Vector2 c3, struct Iron_Vector2 p4, float t);

/* Deferred DRAW2D-08 variant (DrawLineStrip) and DRAW2D-15 whole-spline
 * primitives — array input via Iron_List_Iron_Vector2 BY VALUE (ABI
 * confirmed by Plan 63-03 Task 1 probe, matches triangle_fan/strip
 * shim pattern above). ironc lowers `[Vector2]` parameters to the
 * Iron_List_Iron_Vector2 struct wrapper (items/count/capacity, 24
 * bytes) and passes it by value; the shim forwards `.items` as
 * `const Vector2 *` leveraging Phase 60-02's
 * _Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2)). */
void Iron_draw_line_strip(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color);
void Iron_draw_spline_linear(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_basis(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_catmull_rom(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_bezier_quadratic(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_bezier_cubic(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);

/* Phase 67 extension: default-font draws (TEXT-07, TEXT-08 default variant).
 * Live in the Phase 63 Draw.* namespace because they consume the default
 * font that raylib's rcore initializes during InitWindow; `Draw.text` is
 * the default-font counterpart to `Font.draw_ex`. Pitfall 1: both require
 * Window.init() first — otherwise raylib dereferences a null default-font
 * pointer (rtext.c:130, LoadFontDefault runs during InitWindow only). */
void Iron_draw_fps(int32_t pos_x, int32_t pos_y);
void Iron_draw_text(Iron_String text, int32_t pos_x, int32_t pos_y,
                     int32_t font_size, struct Iron_Color color);

/* ── Collision (Phase 64) ─────────────────────────────────────────── */
/* 2D collision (COLL-01) — 11 functions. See src/vendor/raylib/raylib.h:1304-1315. */

/* Tuple typedef for Collision.lines -> (Bool, Vector2). ironc auto-emits
 * this into the generated consumer C TU (probe confirmed canonical name
 * `Iron_Tuple_Bool_Vector2`); guarded here so `clang -c iron_raylib.c`
 * compiles standalone. Same belt-and-suspenders pattern as
 * IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED above. */
#ifndef IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED
#define IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED
typedef struct {
    bool                 v0;
    struct Iron_Vector2  v1;
} Iron_Tuple_Bool_Vector2;
#endif

/* Rectangle receiver */
bool Iron_rectangle_collides(struct Iron_Rectangle self, struct Iron_Rectangle other);
struct Iron_Rectangle Iron_rectangle_intersection(struct Iron_Rectangle self, struct Iron_Rectangle other);
bool Iron_rectangle_contains_point(struct Iron_Rectangle self, struct Iron_Vector2 point);
bool Iron_rectangle_collides_circle(struct Iron_Rectangle self, struct Iron_Vector2 center, float radius);

/* Vector2 receiver */
bool Iron_vector2_inside_triangle(struct Iron_Vector2 self, struct Iron_Vector2 p1, struct Iron_Vector2 p2, struct Iron_Vector2 p3);
bool Iron_vector2_inside_polygon(struct Iron_Vector2 self, Iron_List_Iron_Vector2 points);
bool Iron_vector2_on_line(struct Iron_Vector2 self, struct Iron_Vector2 p1, struct Iron_Vector2 p2, int32_t threshold);

/* Collision namespace — 2D pair-based tests */
bool Iron_collision_circles(struct Iron_Vector2 c1, float r1, struct Iron_Vector2 c2, float r2);
bool Iron_collision_circle_line(struct Iron_Vector2 center, float radius, struct Iron_Vector2 p1, struct Iron_Vector2 p2);
bool Iron_collision_point_circle(struct Iron_Vector2 point, struct Iron_Vector2 center, float radius);
/* Collision.lines — tuple return. Typedef name from Task 1 probe. */
Iron_Tuple_Bool_Vector2 Iron_collision_lines(struct Iron_Vector2 start_a, struct Iron_Vector2 end_a, struct Iron_Vector2 start_b, struct Iron_Vector2 end_b);

/* 3D collision (COLL-02) — 8 functions. See src/vendor/raylib/raylib.h:1611-1619. */

/* BoundingBox receiver */
bool Iron_boundingbox_collides(struct Iron_BoundingBox self, struct Iron_BoundingBox other);
bool Iron_boundingbox_collides_sphere(struct Iron_BoundingBox self, struct Iron_Vector3 center, float radius);

/* Ray receiver — 5 functions, all return RayCollision (32 bytes) */
struct Iron_RayCollision Iron_ray_hit_sphere(struct Iron_Ray self, struct Iron_Vector3 center, float radius);
struct Iron_RayCollision Iron_ray_hit_box(struct Iron_Ray self, struct Iron_BoundingBox box);
struct Iron_RayCollision Iron_ray_hit_mesh(struct Iron_Ray self, struct Iron_Mesh mesh, struct Iron_Matrix transform);
struct Iron_RayCollision Iron_ray_hit_triangle(struct Iron_Ray self, struct Iron_Vector3 p1, struct Iron_Vector3 p2, struct Iron_Vector3 p3);
struct Iron_RayCollision Iron_ray_hit_quad(struct Iron_Ray self, struct Iron_Vector3 p1, struct Iron_Vector3 p2, struct Iron_Vector3 p3, struct Iron_Vector3 p4);

/* Collision namespace — 3D pair-based test */
bool Iron_collision_spheres(struct Iron_Vector3 c1, float r1, struct Iron_Vector3 c2, float r2);

/* ── raymath (Phase 65) ───────────────────────────────────────────── */

/* RMath namespace — scalar helpers (MATH-01, raymath.h lines 178-228).
 * RMath (raylib-math) avoids a name clash with the Iron stdlib's
 * pre-existing `object Math` in src/stdlib/math.iron and its
 * Iron_math_* symbol family declared by iron_math.h. */
float Iron_rmath_clamp(float value, float min, float max);
float Iron_rmath_lerp(float start, float end, float amount);
float Iron_rmath_normalize(float value, float start, float end);
float Iron_rmath_wrap(float value, float min, float max);
float Iron_rmath_remap(float value, float in_start, float in_end, float out_start, float out_end);
bool  Iron_rmath_float_equals(float x, float y);

/* Vector2 methods — 30 functions (MATH-02, raymath.h lines 236-620).
 * Symbol mangling Iron_vector2_<method> per hir_to_lir lowercase-type
 * convention. C-side param name `self` is NOT reserved (matches Phase 64
 * shim style); Iron-side stub must use `v` (E0101 guard). */
struct Iron_Vector2 Iron_vector2_zero(void);
struct Iron_Vector2 Iron_vector2_one(void);
struct Iron_Vector2 Iron_vector2_add(struct Iron_Vector2 self, struct Iron_Vector2 other);
struct Iron_Vector2 Iron_vector2_add_value(struct Iron_Vector2 self, float add);
struct Iron_Vector2 Iron_vector2_subtract(struct Iron_Vector2 self, struct Iron_Vector2 other);
struct Iron_Vector2 Iron_vector2_subtract_value(struct Iron_Vector2 self, float sub);
float Iron_vector2_length(struct Iron_Vector2 self);
float Iron_vector2_length_sqr(struct Iron_Vector2 self);
float Iron_vector2_dot_product(struct Iron_Vector2 self, struct Iron_Vector2 other);
float Iron_vector2_distance(struct Iron_Vector2 self, struct Iron_Vector2 other);
float Iron_vector2_distance_sqr(struct Iron_Vector2 self, struct Iron_Vector2 other);
float Iron_vector2_angle(struct Iron_Vector2 self, struct Iron_Vector2 other);
float Iron_vector2_line_angle(struct Iron_Vector2 start, struct Iron_Vector2 end);
struct Iron_Vector2 Iron_vector2_scale(struct Iron_Vector2 self, float scale);
struct Iron_Vector2 Iron_vector2_multiply(struct Iron_Vector2 self, struct Iron_Vector2 other);
struct Iron_Vector2 Iron_vector2_negate(struct Iron_Vector2 self);
struct Iron_Vector2 Iron_vector2_divide(struct Iron_Vector2 self, struct Iron_Vector2 other);
struct Iron_Vector2 Iron_vector2_normalize(struct Iron_Vector2 self);
struct Iron_Vector2 Iron_vector2_transform(struct Iron_Vector2 self, struct Iron_Matrix mat);
struct Iron_Vector2 Iron_vector2_lerp(struct Iron_Vector2 self, struct Iron_Vector2 other, float amount);
struct Iron_Vector2 Iron_vector2_reflect(struct Iron_Vector2 self, struct Iron_Vector2 normal);
struct Iron_Vector2 Iron_vector2_min(struct Iron_Vector2 self, struct Iron_Vector2 other);
struct Iron_Vector2 Iron_vector2_max(struct Iron_Vector2 self, struct Iron_Vector2 other);
struct Iron_Vector2 Iron_vector2_rotate(struct Iron_Vector2 self, float angle);
struct Iron_Vector2 Iron_vector2_move_towards(struct Iron_Vector2 self, struct Iron_Vector2 target, float max_distance);
struct Iron_Vector2 Iron_vector2_invert(struct Iron_Vector2 self);
struct Iron_Vector2 Iron_vector2_clamp(struct Iron_Vector2 self, struct Iron_Vector2 min, struct Iron_Vector2 max);
struct Iron_Vector2 Iron_vector2_clamp_value(struct Iron_Vector2 self, float min, float max);
bool Iron_vector2_equals(struct Iron_Vector2 self, struct Iron_Vector2 other);
struct Iron_Vector2 Iron_vector2_refract(struct Iron_Vector2 self, struct Iron_Vector2 n, float r);

/* Vector3 methods — 38 functions (MATH-03, raymath.h lines 621-1140).
 * Symbol mangling Iron_vector3_<method> per hir_to_lir lowercase-type
 * naming. Receiver param is named `self` on the C side (C does not
 * reserve it); Iron side uses `v`/`v1`/`source`/`p` per E0101 rule.
 * Cross-type args: Vector3+Quaternion (rotate_by_quaternion),
 * Vector3+Matrix (transform), Vector3+Matrix+Matrix (unproject) each
 * use one memcpy per struct-kind arg (Phase 64 Iron_ray_hit_mesh
 * precedent).
 * 2 functions deferred:
 *   - to_float_v (Float3 return) → Plan 65-03
 *   - ortho_normalize (out-param 2-tuple) → Plan 65-04 */
struct Iron_Vector3 Iron_vector3_zero(void);
struct Iron_Vector3 Iron_vector3_one(void);
struct Iron_Vector3 Iron_vector3_add(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_add_value(struct Iron_Vector3 self, float add);
struct Iron_Vector3 Iron_vector3_subtract(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_subtract_value(struct Iron_Vector3 self, float sub);
struct Iron_Vector3 Iron_vector3_scale(struct Iron_Vector3 self, float scalar);
struct Iron_Vector3 Iron_vector3_multiply(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_cross_product(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_perpendicular(struct Iron_Vector3 self);
float Iron_vector3_length(struct Iron_Vector3 self);
float Iron_vector3_length_sqr(struct Iron_Vector3 self);
float Iron_vector3_dot_product(struct Iron_Vector3 self, struct Iron_Vector3 other);
float Iron_vector3_distance(struct Iron_Vector3 self, struct Iron_Vector3 other);
float Iron_vector3_distance_sqr(struct Iron_Vector3 self, struct Iron_Vector3 other);
float Iron_vector3_angle(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_negate(struct Iron_Vector3 self);
struct Iron_Vector3 Iron_vector3_divide(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_normalize(struct Iron_Vector3 self);
struct Iron_Vector3 Iron_vector3_project(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_reject(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_transform(struct Iron_Vector3 self, struct Iron_Matrix mat);
struct Iron_Vector3 Iron_vector3_rotate_by_quaternion(struct Iron_Vector3 self, struct Iron_Quaternion q);
struct Iron_Vector3 Iron_vector3_rotate_by_axis_angle(struct Iron_Vector3 self, struct Iron_Vector3 axis, float angle);
struct Iron_Vector3 Iron_vector3_move_towards(struct Iron_Vector3 self, struct Iron_Vector3 target, float max_distance);
struct Iron_Vector3 Iron_vector3_lerp(struct Iron_Vector3 self, struct Iron_Vector3 other, float amount);
struct Iron_Vector3 Iron_vector3_cubic_hermite(struct Iron_Vector3 self, struct Iron_Vector3 tangent1, struct Iron_Vector3 v2, struct Iron_Vector3 tangent2, float amount);
struct Iron_Vector3 Iron_vector3_reflect(struct Iron_Vector3 self, struct Iron_Vector3 normal);
struct Iron_Vector3 Iron_vector3_min(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_max(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_barycenter(struct Iron_Vector3 self, struct Iron_Vector3 a, struct Iron_Vector3 b, struct Iron_Vector3 c);
struct Iron_Vector3 Iron_vector3_unproject(struct Iron_Vector3 self, struct Iron_Matrix projection, struct Iron_Matrix view);
struct Iron_Vector3 Iron_vector3_invert(struct Iron_Vector3 self);
struct Iron_Vector3 Iron_vector3_clamp(struct Iron_Vector3 self, struct Iron_Vector3 min, struct Iron_Vector3 max);
struct Iron_Vector3 Iron_vector3_clamp_value(struct Iron_Vector3 self, float min, float max);
bool Iron_vector3_equals(struct Iron_Vector3 self, struct Iron_Vector3 other);
struct Iron_Vector3 Iron_vector3_refract(struct Iron_Vector3 self, struct Iron_Vector3 n, float r);

/* Vector3.to_float_v (MATH-03 carried from Plan 65-02) — first raymath
 * helper returning a Float3 (12 B) via memcpy-out (Pattern 2g). */
struct Iron_Float3 Iron_vector3_to_float_v(struct Iron_Vector3 self);

/* Vector4 methods — 22 functions (MATH-04, raymath.h lines 1232-1440).
 * Same 16 B struct-by-value in/out template as Rectangle (Phase 63).
 * Vector4Equals returns int → shim coerces to bool. */
struct Iron_Vector4 Iron_vector4_zero(void);
struct Iron_Vector4 Iron_vector4_one(void);
struct Iron_Vector4 Iron_vector4_add(struct Iron_Vector4 self, struct Iron_Vector4 other);
struct Iron_Vector4 Iron_vector4_add_value(struct Iron_Vector4 self, float add);
struct Iron_Vector4 Iron_vector4_subtract(struct Iron_Vector4 self, struct Iron_Vector4 other);
struct Iron_Vector4 Iron_vector4_subtract_value(struct Iron_Vector4 self, float sub);
float Iron_vector4_length(struct Iron_Vector4 self);
float Iron_vector4_length_sqr(struct Iron_Vector4 self);
float Iron_vector4_dot_product(struct Iron_Vector4 self, struct Iron_Vector4 other);
float Iron_vector4_distance(struct Iron_Vector4 self, struct Iron_Vector4 other);
float Iron_vector4_distance_sqr(struct Iron_Vector4 self, struct Iron_Vector4 other);
struct Iron_Vector4 Iron_vector4_scale(struct Iron_Vector4 self, float scale);
struct Iron_Vector4 Iron_vector4_multiply(struct Iron_Vector4 self, struct Iron_Vector4 other);
struct Iron_Vector4 Iron_vector4_negate(struct Iron_Vector4 self);
struct Iron_Vector4 Iron_vector4_divide(struct Iron_Vector4 self, struct Iron_Vector4 other);
struct Iron_Vector4 Iron_vector4_normalize(struct Iron_Vector4 self);
struct Iron_Vector4 Iron_vector4_min(struct Iron_Vector4 self, struct Iron_Vector4 other);
struct Iron_Vector4 Iron_vector4_max(struct Iron_Vector4 self, struct Iron_Vector4 other);
struct Iron_Vector4 Iron_vector4_lerp(struct Iron_Vector4 self, struct Iron_Vector4 other, float amount);
struct Iron_Vector4 Iron_vector4_move_towards(struct Iron_Vector4 self, struct Iron_Vector4 target, float max_distance);
struct Iron_Vector4 Iron_vector4_invert(struct Iron_Vector4 self);
bool Iron_vector4_equals(struct Iron_Vector4 self, struct Iron_Vector4 other);

/* Matrix methods — 21 of 22 functions (MATH-05, raymath.h lines
 * 1459-1985). Decompose deferred to Plan 65-04 (3-tuple out-param).
 * First 64 B struct-by-value RETURN in the codebase. Frustum/Perspective/
 * Ortho widen Iron Float32 to raymath double via (double) casts. */
float Iron_matrix_determinant(struct Iron_Matrix self);
float Iron_matrix_trace(struct Iron_Matrix self);
struct Iron_Matrix Iron_matrix_transpose(struct Iron_Matrix self);
struct Iron_Matrix Iron_matrix_invert(struct Iron_Matrix self);
struct Iron_Matrix Iron_matrix_identity(void);
struct Iron_Matrix Iron_matrix_add(struct Iron_Matrix self, struct Iron_Matrix other);
struct Iron_Matrix Iron_matrix_subtract(struct Iron_Matrix self, struct Iron_Matrix other);
struct Iron_Matrix Iron_matrix_multiply(struct Iron_Matrix self, struct Iron_Matrix other);
struct Iron_Matrix Iron_matrix_translate(float x, float y, float z);
struct Iron_Matrix Iron_matrix_rotate(struct Iron_Vector3 axis, float angle);
struct Iron_Matrix Iron_matrix_rotate_x(float angle);
struct Iron_Matrix Iron_matrix_rotate_y(float angle);
struct Iron_Matrix Iron_matrix_rotate_z(float angle);
struct Iron_Matrix Iron_matrix_rotate_xyz(struct Iron_Vector3 angle);
struct Iron_Matrix Iron_matrix_rotate_zyx(struct Iron_Vector3 angle);
struct Iron_Matrix Iron_matrix_scale(float x, float y, float z);
struct Iron_Matrix Iron_matrix_frustum(float left, float right, float bottom, float top, float near_plane, float far_plane);
struct Iron_Matrix Iron_matrix_perspective(float fovy, float aspect, float near_plane, float far_plane);
struct Iron_Matrix Iron_matrix_ortho(float left, float right, float bottom, float top, float near_plane, float far_plane);
struct Iron_Matrix Iron_matrix_look_at(struct Iron_Vector3 eye, struct Iron_Vector3 target, struct Iron_Vector3 up);
struct Iron_Float16 Iron_matrix_to_float_v(struct Iron_Matrix self);

/* ── Phase 65 Plan 04: guarded tuple typedefs ──────────────────────
 * Mirrors Phase 64-01's IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED style.
 * ironc auto-emits the same typedef into generated consumer C TUs
 * via the generic arity-agnostic loop at emit_helpers.c:260-294
 * (Task 1 probe confirmed canonical names). The guards let this
 * header compile standalone via `clang -c iron_raylib.c`; the
 * matching auto-emitted typedefs in the consumer TU share the
 * same name, and the `*_STRUCT_DEFINED` guards prevent double
 * definition at link time. */

#ifndef IRON_TUPLE_VECTOR3_VECTOR3_STRUCT_DEFINED
#define IRON_TUPLE_VECTOR3_VECTOR3_STRUCT_DEFINED
typedef struct {
    struct Iron_Vector3 v0;
    struct Iron_Vector3 v1;
} Iron_Tuple_Vector3_Vector3;
#endif

#ifndef IRON_TUPLE_VECTOR3_FLOAT32_STRUCT_DEFINED
#define IRON_TUPLE_VECTOR3_FLOAT32_STRUCT_DEFINED
typedef struct {
    struct Iron_Vector3 v0;
    float               v1;
} Iron_Tuple_Vector3_Float32;
#endif

#ifndef IRON_TUPLE_VECTOR3_QUATERNION_VECTOR3_STRUCT_DEFINED
#define IRON_TUPLE_VECTOR3_QUATERNION_VECTOR3_STRUCT_DEFINED
typedef struct {
    struct Iron_Vector3    v0;
    struct Iron_Quaternion v1;
    struct Iron_Vector3    v2;
} Iron_Tuple_Vector3_Quaternion_Vector3;
#endif

/* Matrix.decompose (MATH-05 carried from Plan 65-03) — 3-tuple
 * out-param. Raymath signature:
 *   void MatrixDecompose(Matrix, Vector3 *translation,
 *                        Quaternion *rotation, Vector3 *scale);
 * First 3-tuple return in the Iron codebase. */
Iron_Tuple_Vector3_Quaternion_Vector3 Iron_matrix_decompose(struct Iron_Matrix self);

/* Vector3.ortho_normalize (MATH-03 carried from Plan 65-02) —
 * 2-tuple out-param, mutates both args:
 *   void Vector3OrthoNormalize(Vector3 *v1, Vector3 *v2); */
Iron_Tuple_Vector3_Vector3 Iron_vector3_ortho_normalize(struct Iron_Vector3 self, struct Iron_Vector3 other);

/* Quaternion methods — 24 of 24 functions (MATH-06, raymath.h lines
 * 1731-2170). Raymath 5.5 Quaternion RMAPI count verified at 24 via
 * `grep -cE '^RMAPI [A-Za-z0-9_]+ Quaternion[A-Z]' raymath.h` = 24.
 * Plan text claimed 26 pre-emptively — resolved GREEN at 24.
 *
 * Same 16 B memcpy-in/memcpy-out template as Vector4. Cross-type
 * shims use one memcpy per struct-kind arg (Phase 64 precedent).
 * QuaternionEquals returns int → shim coerces to bool.
 *
 * QuaternionToAxisAngle is the 2-tuple out-param case, returning
 * Iron_Tuple_Vector3_Float32. */
struct Iron_Quaternion Iron_quaternion_add(struct Iron_Quaternion self, struct Iron_Quaternion other);
struct Iron_Quaternion Iron_quaternion_add_value(struct Iron_Quaternion self, float add);
struct Iron_Quaternion Iron_quaternion_subtract(struct Iron_Quaternion self, struct Iron_Quaternion other);
struct Iron_Quaternion Iron_quaternion_subtract_value(struct Iron_Quaternion self, float sub);
struct Iron_Quaternion Iron_quaternion_identity(void);
float Iron_quaternion_length(struct Iron_Quaternion self);
struct Iron_Quaternion Iron_quaternion_normalize(struct Iron_Quaternion self);
struct Iron_Quaternion Iron_quaternion_invert(struct Iron_Quaternion self);
struct Iron_Quaternion Iron_quaternion_multiply(struct Iron_Quaternion self, struct Iron_Quaternion other);
struct Iron_Quaternion Iron_quaternion_scale(struct Iron_Quaternion self, float mul);
struct Iron_Quaternion Iron_quaternion_divide(struct Iron_Quaternion self, struct Iron_Quaternion other);
struct Iron_Quaternion Iron_quaternion_lerp(struct Iron_Quaternion self, struct Iron_Quaternion other, float amount);
struct Iron_Quaternion Iron_quaternion_nlerp(struct Iron_Quaternion self, struct Iron_Quaternion other, float amount);
struct Iron_Quaternion Iron_quaternion_slerp(struct Iron_Quaternion self, struct Iron_Quaternion other, float amount);
struct Iron_Quaternion Iron_quaternion_cubic_hermite_spline(struct Iron_Quaternion self, struct Iron_Quaternion out_tangent1, struct Iron_Quaternion q2, struct Iron_Quaternion in_tangent2, float t);
struct Iron_Quaternion Iron_quaternion_from_vector3_to_vector3(struct Iron_Vector3 from, struct Iron_Vector3 to);
struct Iron_Quaternion Iron_quaternion_from_matrix(struct Iron_Matrix mat);
struct Iron_Matrix     Iron_quaternion_to_matrix(struct Iron_Quaternion self);
struct Iron_Quaternion Iron_quaternion_from_axis_angle(struct Iron_Vector3 axis, float angle);
Iron_Tuple_Vector3_Float32 Iron_quaternion_to_axis_angle(struct Iron_Quaternion self);
struct Iron_Quaternion Iron_quaternion_from_euler(float pitch, float yaw, float roll);
struct Iron_Vector3    Iron_quaternion_to_euler(struct Iron_Quaternion self);
struct Iron_Quaternion Iron_quaternion_transform(struct Iron_Quaternion self, struct Iron_Matrix mat);
bool Iron_quaternion_equals(struct Iron_Quaternion self, struct Iron_Quaternion other);

/* ── Textures & Images (Phase 66) ─────────────────────────────────── */

/* Color math — TEX-13 (Plan 66-01).
 *
 * 18 Color-math shims forwarding to raylib's ColorIsEqual/Fade/ColorToInt/
 * ColorNormalize/ColorFromNormalized/ColorToHSV/ColorFromHSV/ColorTint/
 * ColorBrightness/ColorContrast/ColorAlpha/ColorAlphaBlend/ColorLerp/
 * GetColor/GetPixelColor/SetPixelColor/GetPixelDataSize. Iron-side methods
 * live on the Color namespace (Color.is_equal, Color.fade, Color.to_hsv,
 * ...). Bool returns use the Phase 62 `(bool)(... != 0)` coercion for
 * ColorIsEqual. Opaque `void *` pixel-buffer arguments cross the FFI as
 * Iron `Int` (int64_t) and are recovered via `(void *)(intptr_t)data` —
 * first opaque-pointer function argument in this TU.
 */
bool                Iron_color_is_equal(struct Iron_Color c1, struct Iron_Color c2);
struct Iron_Color   Iron_color_fade(struct Iron_Color c, float alpha);
int32_t             Iron_color_to_int(struct Iron_Color c);
struct Iron_Vector4 Iron_color_normalize(struct Iron_Color c);
struct Iron_Color   Iron_color_from_normalized(struct Iron_Vector4 v);
struct Iron_Vector3 Iron_color_to_hsv(struct Iron_Color c);
struct Iron_Color   Iron_color_from_hsv(float hue, float saturation, float value);
struct Iron_Color   Iron_color_tint(struct Iron_Color c, struct Iron_Color tint);
struct Iron_Color   Iron_color_brightness(struct Iron_Color c, float factor);
struct Iron_Color   Iron_color_contrast(struct Iron_Color c, float contrast);
struct Iron_Color   Iron_color_alpha(struct Iron_Color c, float alpha);
struct Iron_Color   Iron_color_alpha_blend(struct Iron_Color dst, struct Iron_Color src, struct Iron_Color tint);
struct Iron_Color   Iron_color_lerp(struct Iron_Color c1, struct Iron_Color c2, float factor);
struct Iron_Color   Iron_color_from_int(uint32_t hex_value);
struct Iron_Color   Iron_color_from_pixel_data(int64_t data, int32_t format);
void                Iron_color_to_pixel_data(int64_t data, struct Iron_Color c, int32_t format);
int32_t             Iron_color_pixel_data_size(int32_t width, int32_t height, int32_t format);

/* Iron's [Color] lowers to Iron_List_Iron_Color in C (ARRAY_PARAM_LIST
 * mode, confirmed by Plan 66-02 Task 1 probe — emit_structs.c:309-312
 * foreign-method-stub return scan auto-emits this typedef + IRON_LIST_DECL
 * + IRON_LIST_IMPL into the consumer TU). Guarded the same way as
 * IRON_LIST_IRON_VECTOR2 so `clang -c iron_raylib.c` compiles standalone
 * — the compiler's own emit_structs.c scan produces an identical typedef
 * in the consumer TU, and this header's guard prevents a redefinition. */
#ifndef IRON_LIST_IRON_COLOR_STRUCT_DEFINED
#define IRON_LIST_IRON_COLOR_STRUCT_DEFINED
typedef struct Iron_List_Iron_Color {
    struct Iron_Color *items;
    int64_t            count;
    int64_t            capacity;
} Iron_List_Iron_Color;
#endif

/* Image load/unload/valid (TEX-01) + narrowed TEX-02 (Plan 66-02).
 *
 * DEFERRED to a follow-up phase per RESEARCH Pitfall 7 ([UInt8] FFI
 * blocker): LoadImageRaw, LoadImageFromMemory, LoadImageAnimFromMemory,
 * ExportImageToMemory. All four take or return a raw byte buffer which
 * Iron cannot yet express as a function parameter / return type.
 *
 * Image.load_anim (below) drops raylib's `int *frames` out-param — Iron
 * has no out-ref mechanism. Callers needing the frame count should
 * re-bind to the raw raylib call once [UInt8] FFI lands. */
struct Iron_Image Iron_image_load(Iron_String path);
void              Iron_image_unload(struct Iron_Image img);
bool              Iron_image_is_valid(struct Iron_Image img);
struct Iron_Image Iron_image_load_anim(Iron_String path);
struct Iron_Image Iron_image_from_texture(struct Iron_Texture tex);
struct Iron_Image Iron_image_from_screen(void);

/* Image generation (TEX-03). raylib's GenImageText signature is
 * (width, height, text) — no color arg; output is a grayscale bitmap
 * rendered with the default font (requires Window.init() first per
 * RESEARCH Pitfall 4). */
struct Iron_Image Iron_image_color(int32_t width, int32_t height, struct Iron_Color color);
struct Iron_Image Iron_image_gradient_linear(int32_t width, int32_t height, int32_t direction,
                                              struct Iron_Color start, struct Iron_Color finish);
struct Iron_Image Iron_image_gradient_radial(int32_t width, int32_t height, float density,
                                              struct Iron_Color inner, struct Iron_Color outer);
struct Iron_Image Iron_image_gradient_square(int32_t width, int32_t height, float density,
                                              struct Iron_Color inner, struct Iron_Color outer);
struct Iron_Image Iron_image_checked(int32_t width, int32_t height, int32_t checks_x, int32_t checks_y,
                                      struct Iron_Color c1, struct Iron_Color c2);
struct Iron_Image Iron_image_white_noise(int32_t width, int32_t height, float factor);
struct Iron_Image Iron_image_perlin_noise(int32_t width, int32_t height, int32_t offset_x,
                                           int32_t offset_y, float scale);
struct Iron_Image Iron_image_cellular(int32_t width, int32_t height, int32_t tile_size);
struct Iron_Image Iron_image_text(int32_t width, int32_t height, Iron_String text);

/* Image export (narrowed TEX-04). */
bool Iron_image_export(struct Iron_Image img, Iron_String path);
bool Iron_image_export_as_code(struct Iron_Image img, Iron_String path);

/* Image data extraction (TEX-06). load_colors / load_palette return
 * Iron_List_Iron_Color — first reverse-direction Iron list in this TU.
 * The shims malloc the items buffer and rely on raylib's
 * UnloadImageColors / UnloadImagePalette to free the raylib-side
 * source; Iron owns the copy after memcpy. */
Iron_List_Iron_Color Iron_image_load_colors(struct Iron_Image img);
Iron_List_Iron_Color Iron_image_load_palette(struct Iron_Image img, int32_t max_palette_size);
struct Iron_Rectangle Iron_image_get_alpha_border(struct Iron_Image img, float threshold);
struct Iron_Color     Iron_image_get_color(struct Iron_Image img, int32_t x, int32_t y);

/* TEX-05 Image transforms (Plan 66-03 Task 1).
 *
 * 27 mutating-transform + by-value-return shims implementing the
 * "take Image by value, return mutated Image by value" idiom locked
 * in Phase 66 CONTEXT.md. Shim body template (Pattern 2): memcpy the
 * Iron_Image struct into a local raylib `Image`, call `ImageFoo(&src, ...)`
 * so raylib mutates `src` in place, memcpy the mutated `src` back out
 * into a fresh Iron_Image return value. Chain-style composition in Iron:
 *   image.crop(r).flip_vertical().color_tint(RED)
 *
 * 3 shims (copy / from_rectangle / from_channel) use Pattern 1 (Image
 * returned by raylib by value — no *dst mutation).
 *
 * Deferred:
 *   - ImageKernelConvolution: requires [Float32] FFI runtime support
 *     (Pitfall 7 — no Iron_List_float / Iron_List_double in primitive
 *     list types at iron_runtime.h:824-830). Rebind in the phase that
 *     lands [Float32] alongside [UInt8].
 *   - ImageTextEx: Font-dependent, owned by Phase 67.
 */
struct Iron_Image Iron_image_copy(struct Iron_Image img);
struct Iron_Image Iron_image_from_rectangle(struct Iron_Image img, struct Iron_Rectangle rec);
struct Iron_Image Iron_image_from_channel(struct Iron_Image img, int32_t selected_channel);
struct Iron_Image Iron_image_format(struct Iron_Image img, int32_t new_format);
struct Iron_Image Iron_image_to_pot(struct Iron_Image img, struct Iron_Color fill);
struct Iron_Image Iron_image_crop(struct Iron_Image img, struct Iron_Rectangle crop);
struct Iron_Image Iron_image_alpha_crop(struct Iron_Image img, float threshold);
struct Iron_Image Iron_image_alpha_clear(struct Iron_Image img, struct Iron_Color color, float threshold);
struct Iron_Image Iron_image_alpha_mask(struct Iron_Image img, struct Iron_Image mask);
struct Iron_Image Iron_image_alpha_premultiply(struct Iron_Image img);
struct Iron_Image Iron_image_blur_gaussian(struct Iron_Image img, int32_t blur_size);
struct Iron_Image Iron_image_resize(struct Iron_Image img, int32_t new_width, int32_t new_height);
struct Iron_Image Iron_image_resize_nn(struct Iron_Image img, int32_t new_width, int32_t new_height);
struct Iron_Image Iron_image_resize_canvas(struct Iron_Image img, int32_t new_width, int32_t new_height,
                                            int32_t offset_x, int32_t offset_y, struct Iron_Color fill);
struct Iron_Image Iron_image_mipmaps(struct Iron_Image img);
struct Iron_Image Iron_image_dither(struct Iron_Image img, int32_t r_bpp, int32_t g_bpp,
                                     int32_t b_bpp, int32_t a_bpp);
struct Iron_Image Iron_image_flip_vertical(struct Iron_Image img);
struct Iron_Image Iron_image_flip_horizontal(struct Iron_Image img);
struct Iron_Image Iron_image_rotate(struct Iron_Image img, int32_t degrees);
struct Iron_Image Iron_image_rotate_cw(struct Iron_Image img);
struct Iron_Image Iron_image_rotate_ccw(struct Iron_Image img);
struct Iron_Image Iron_image_color_tint(struct Iron_Image img, struct Iron_Color color);
struct Iron_Image Iron_image_color_invert(struct Iron_Image img);
struct Iron_Image Iron_image_color_grayscale(struct Iron_Image img);
struct Iron_Image Iron_image_color_contrast(struct Iron_Image img, float contrast);
struct Iron_Image Iron_image_color_brightness(struct Iron_Image img, int32_t brightness);
struct Iron_Image Iron_image_color_replace(struct Iron_Image img, struct Iron_Color color,
                                            struct Iron_Color replace);
/* ImageKernelConvolution DEFERRED: requires [Float32] FFI runtime support (Pitfall 7 variant) */

/* TEX-07 Image CPU draw (Plan 66-03 Task 2 — 21 shims).
 *
 * Chain-style CPU-side image composition. Every shim uses Pattern 2:
 * memcpy Iron_Image in, call ImageDrawFoo(&dst, ...) so raylib mutates
 * dst in place, memcpy mutated dst out as fresh Iron_Image return.
 * Lets users compose raster scenes without a live GPU context:
 *   canvas.clear_background(BLACK).draw_rectangle(...).draw_text(...).draw_pixel(...)
 *
 * Array-input draws (draw_triangle_fan, draw_triangle_strip) reuse the
 * Iron_List_Iron_Vector2 by-value ABI from Phase 63-03 / 63-04 (the
 * Task 1 probe-GREEN ARRAY_PARAM_LIST mode).
 *
 * ImageDrawTextEx (Phase 66 deferral) CLOSED in Phase 67 Plan 01 —
 * see Iron_image_draw_text_ex prototype below, adjacent to the
 * Image.draw_text non-Ex variant.
 */
struct Iron_Image Iron_image_clear_background(struct Iron_Image img, struct Iron_Color color);
struct Iron_Image Iron_image_draw_pixel(struct Iron_Image img, int32_t pos_x, int32_t pos_y,
                                         struct Iron_Color color);
struct Iron_Image Iron_image_draw_pixel_v(struct Iron_Image img, struct Iron_Vector2 position,
                                           struct Iron_Color color);
struct Iron_Image Iron_image_draw_line(struct Iron_Image img, int32_t start_x, int32_t start_y,
                                        int32_t end_x, int32_t end_y, struct Iron_Color color);
struct Iron_Image Iron_image_draw_line_v(struct Iron_Image img, struct Iron_Vector2 start,
                                          struct Iron_Vector2 finish, struct Iron_Color color);
struct Iron_Image Iron_image_draw_line_ex(struct Iron_Image img, struct Iron_Vector2 start,
                                           struct Iron_Vector2 finish, int32_t thick,
                                           struct Iron_Color color);
struct Iron_Image Iron_image_draw_circle(struct Iron_Image img, int32_t center_x, int32_t center_y,
                                          int32_t radius, struct Iron_Color color);
struct Iron_Image Iron_image_draw_circle_v(struct Iron_Image img, struct Iron_Vector2 center,
                                            int32_t radius, struct Iron_Color color);
struct Iron_Image Iron_image_draw_circle_lines(struct Iron_Image img, int32_t center_x, int32_t center_y,
                                                int32_t radius, struct Iron_Color color);
struct Iron_Image Iron_image_draw_circle_lines_v(struct Iron_Image img, struct Iron_Vector2 center,
                                                  int32_t radius, struct Iron_Color color);
struct Iron_Image Iron_image_draw_rectangle(struct Iron_Image img, int32_t pos_x, int32_t pos_y,
                                             int32_t width, int32_t height, struct Iron_Color color);
struct Iron_Image Iron_image_draw_rectangle_v(struct Iron_Image img, struct Iron_Vector2 position,
                                               struct Iron_Vector2 size, struct Iron_Color color);
struct Iron_Image Iron_image_draw_rectangle_rec(struct Iron_Image img, struct Iron_Rectangle rec,
                                                 struct Iron_Color color);
struct Iron_Image Iron_image_draw_rectangle_lines(struct Iron_Image img, struct Iron_Rectangle rec,
                                                   int32_t thick, struct Iron_Color color);
struct Iron_Image Iron_image_draw_triangle(struct Iron_Image img, struct Iron_Vector2 v1,
                                            struct Iron_Vector2 v2, struct Iron_Vector2 v3,
                                            struct Iron_Color color);
struct Iron_Image Iron_image_draw_triangle_ex(struct Iron_Image img, struct Iron_Vector2 v1,
                                               struct Iron_Vector2 v2, struct Iron_Vector2 v3,
                                               struct Iron_Color c1, struct Iron_Color c2,
                                               struct Iron_Color c3);
struct Iron_Image Iron_image_draw_triangle_lines(struct Iron_Image img, struct Iron_Vector2 v1,
                                                  struct Iron_Vector2 v2, struct Iron_Vector2 v3,
                                                  struct Iron_Color color);
struct Iron_Image Iron_image_draw_triangle_fan(struct Iron_Image img, Iron_List_Iron_Vector2 points,
                                                int32_t point_count, struct Iron_Color color);
struct Iron_Image Iron_image_draw_triangle_strip(struct Iron_Image img, Iron_List_Iron_Vector2 points,
                                                  int32_t point_count, struct Iron_Color color);
struct Iron_Image Iron_image_draw(struct Iron_Image img, struct Iron_Image src_img,
                                   struct Iron_Rectangle src_rec, struct Iron_Rectangle dst_rec,
                                   struct Iron_Color tint);
struct Iron_Image Iron_image_draw_text(struct Iron_Image img, Iron_String text, int32_t pos_x,
                                        int32_t pos_y, int32_t font_size, struct Iron_Color color);
/* ImageDrawTextEx (Phase 66 deferral) CLOSED in Phase 67 Plan 01 — see
 * Iron_image_draw_text_ex prototype adjacent to Image.text_ex below. */

/* TEX-08/09/10/11 Texture + RenderTexture (Plan 66-04 Task 1 — 12 shims).
 *
 * First Texture-by-value INPUT at scale (config methods) and first
 * RenderTexture-by-value RETURN (44 B — under Phase 64's 120 B ceiling,
 * zero `-Wlarge-by-value-copy` warnings). Opaque void* ARG for texture
 * updates extends Plan 66-01's Color.from_pixel_data probe (Int →
 * (void *)(intptr_t) cast). TextureCubemap / RenderTexture2D are plain
 * typedef aliases of Texture / RenderTexture in raylib.h, so the Iron
 * mirror is shared (Pitfall 8).
 *
 * Memory ownership: Texture.unload / RenderTexture.unload hand the
 * Iron_Texture / Iron_RenderTexture struct to raylib by value; raylib
 * frees its own GPU-side handle. Iron does not own the pixel buffer.
 */
struct Iron_Texture Iron_texture_load(Iron_String path);
struct Iron_Texture Iron_image_to_texture(struct Iron_Image img);
void                Iron_texture_unload(struct Iron_Texture tex);
bool                Iron_texture_is_valid(struct Iron_Texture tex);
struct Iron_Texture       Iron_texture_load_cubemap(struct Iron_Image img, int32_t layout);
struct Iron_RenderTexture Iron_rendertexture_load(int32_t width, int32_t height);
void                      Iron_rendertexture_unload(struct Iron_RenderTexture rt);
bool                      Iron_rendertexture_is_valid(struct Iron_RenderTexture rt);
void Iron_texture_update(struct Iron_Texture tex, int64_t pixels);
void Iron_texture_update_rec(struct Iron_Texture tex, struct Iron_Rectangle rec, int64_t pixels);
void                Iron_texture_set_filter(struct Iron_Texture tex, int32_t filter);
void                Iron_texture_set_wrap(struct Iron_Texture tex, int32_t wrap);
struct Iron_Texture Iron_texture_gen_mipmaps(struct Iron_Texture tex);

/* TEX-12 Texture draw variants (Plan 66-04 Task 2 — 6 shims).
 *
 * First Texture-by-value INPUT at scale (Phase 63-01's begin_texture_mode
 * established the ABI with a single consumer; these 6 draws exercise the
 * same memcpy template 6x). First NPatchInfo-by-value INPUT (36 B, under
 * the -Wlarge-by-value-copy 64 B threshold) via draw_n_patch. All shims
 * forward to DrawTexture* family which consumes Texture + Vector2 /
 * Rectangle / Color / NPatchInfo by value.
 */
void Iron_texture_draw(struct Iron_Texture tex, int32_t pos_x, int32_t pos_y,
                        struct Iron_Color tint);
void Iron_texture_draw_v(struct Iron_Texture tex, struct Iron_Vector2 position,
                          struct Iron_Color tint);
void Iron_texture_draw_ex(struct Iron_Texture tex, struct Iron_Vector2 position,
                           float rotation, float scale, struct Iron_Color tint);
void Iron_texture_draw_rec(struct Iron_Texture tex, struct Iron_Rectangle source,
                            struct Iron_Vector2 position, struct Iron_Color tint);
void Iron_texture_draw_pro(struct Iron_Texture tex, struct Iron_Rectangle source,
                            struct Iron_Rectangle dest, struct Iron_Vector2 origin,
                            float rotation, struct Iron_Color tint);
void Iron_texture_draw_n_patch(struct Iron_Texture tex, struct Iron_NPatchInfo n_patch_info,
                                struct Iron_Rectangle dest, struct Iron_Vector2 origin,
                                float rotation, struct Iron_Color tint);

/* ── Text & Fonts (Phase 67) ──────────────────────────────────────── */
/*
 * Font loading surface (TEXT-01..04, TEXT-06). All shims use Phase 66-04
 * Texture/RenderTexture templates (struct-by-value RETURN + INPUT via
 * memcpy). Font is 48 B — under the -Wlarge-by-value-copy 64 B threshold
 * verified by Phase 60-03 _Static_assert grid.
 *
 * Pitfall 1 (Phase 67 RESEARCH): Font.default() requires Window.init() to
 * have bootstrapped the default font via raylib rtext.c LoadFontDefault.
 * Pitfall 5: LoadFont returns Font with baseSize=0 on failure — users must
 * call font.is_valid() before drawing.
 *
 * Font.from_memory DEFERRED: [UInt8] FFI blocker — Iron_List_uint8_t not
 * pre-declared in iron_runtime.h (iron_runtime.h:824-830 lists only
 * int64_t/int32_t/double/bool/Iron_String/Iron_Closure). Unblocks when a
 * runtime task adds IRON_LIST_DECL(uint8_t, uint8_t). See Phase 66
 * Pitfall 9 + 67-RESEARCH.md Open Question 2.
 * Font.load_data DEFERRED: same [UInt8] FFI blocker (raw fileData input).
 */
struct Iron_Font Iron_font_default(void);
struct Iron_Font Iron_font_load(Iron_String file_name);
struct Iron_Font Iron_font_load_ex(Iron_String file_name, int32_t font_size,
                                    Iron_List_int32_t codepoints);
struct Iron_Font Iron_font_from_image(struct Iron_Image image, struct Iron_Color key,
                                       int32_t first_char);
bool             Iron_font_is_valid(struct Iron_Font font);
void             Iron_font_unload(struct Iron_Font font);
bool             Iron_font_export_as_code(struct Iron_Font font, Iron_String file_name);

/* Iron's [GlyphInfo] lowers to Iron_List_Iron_GlyphInfo in C. Guarded the
 * same way as Iron_List_Iron_Color / Iron_List_Iron_Vector2 so
 * `clang -c iron_raylib.c` compiles standalone — the compiler's
 * emit_structs.c Scan B auto-emits an identical typedef in the consumer
 * TU, and the guard prevents double-definition. Same belt-and-suspenders
 * pattern as IRON_LIST_IRON_COLOR_STRUCT_DEFINED / IRON_LIST_IRON_VECTOR2
 * above. */
#ifndef IRON_LIST_IRON_GLYPHINFO_STRUCT_DEFINED
#define IRON_LIST_IRON_GLYPHINFO_STRUCT_DEFINED
typedef struct Iron_List_Iron_GlyphInfo {
    struct Iron_GlyphInfo *items;
    int64_t                count;
    int64_t                capacity;
} Iron_List_Iron_GlyphInfo;
#endif

/* Iron's [Rectangle] lowers to Iron_List_Iron_Rectangle in C. Same guard
 * pattern. */
#ifndef IRON_LIST_IRON_RECTANGLE_STRUCT_DEFINED
#define IRON_LIST_IRON_RECTANGLE_STRUCT_DEFINED
typedef struct Iron_List_Iron_Rectangle {
    struct Iron_Rectangle *items;
    int64_t                count;
    int64_t                capacity;
} Iron_List_Iron_Rectangle;
#endif

/* Tuple typedef for Font.gen_image_atlas -> (Image, [Rectangle]).
 *
 * Mangling trace: ironc's tuple_build_mangled_name (src/analyzer/types.c:170)
 * joins iron_type_to_string of each element, sanitizing non-alnum/_ to '_'.
 *   iron_type_to_string(Image)       = "Image"
 *   iron_type_to_string([Rectangle]) = "[Rectangle]" -> "_Rectangle_"
 *   Final:   Iron_Tuple_Image__Rectangle_
 *
 * Field types use emit_type_to_c (emit_helpers.c:140): Image resolves to
 * struct Iron_Image (header-declared above), [Rectangle] resolves to
 * Iron_List_Iron_Rectangle (typedef above). Guarded so standalone
 * `clang -c iron_raylib.c` compiles. */
#ifndef IRON_TUPLE_IMAGE__RECTANGLE__STRUCT_DEFINED
#define IRON_TUPLE_IMAGE__RECTANGLE__STRUCT_DEFINED
typedef struct {
    struct Iron_Image        v0;
    Iron_List_Iron_Rectangle v1;
} Iron_Tuple_Image__Rectangle_;
#endif

/* TEXT-05 Font data probes.
 * gen_image_atlas bakes an atlas Image + per-glyph source Rectangles;
 * the shim deep-copies the Rectangle out-array into Iron-owned storage
 * then frees raylib's RL_MALLOC'd buffer. unload_data releases a
 * [GlyphInfo] array via raylib's UnloadFontData.
 *
 * Font.load_data + Font.from_memory DEFERRED: both take [UInt8] fileData.
 * Iron_List_uint8_t is not pre-declared in iron_runtime.h:824-830.
 * Unblocks when a runtime task adds IRON_LIST_DECL(uint8_t, uint8_t) —
 * same gate as the 5 existing Phase 66 [UInt8] deferrals
 * (LoadImageRaw, LoadImageFromMemory, LoadImageAnimFromMemory,
 * ExportImageToMemory, ImageKernelConvolution). */
Iron_Tuple_Image__Rectangle_
Iron_font_gen_image_atlas(Iron_List_Iron_GlyphInfo glyphs, int32_t font_size,
                           int32_t padding, int32_t pack_method);
void Iron_font_unload_data(Iron_List_Iron_GlyphInfo glyphs);

/* Phase 66 Font-dependent Image deferrals closed (TEX-05/07 Font variant).
 * ImageTextEx allocates a fresh Image from rendering `text` with the
 * given Font. ImageDrawTextEx mutates an existing Image in place using
 * Phase 66-03 Pattern 2 (mutating-return-by-value).
 *
 * Font lifetime MUST outlive both calls (Pitfall 6 in 67-RESEARCH.md) —
 * raylib reads Font._recs / _glyphs internally to look up glyphs. */
struct Iron_Image Iron_image_text_ex(struct Iron_Font font, Iron_String text,
                                      float font_size, float spacing,
                                      struct Iron_Color tint);
struct Iron_Image Iron_image_draw_text_ex(struct Iron_Image img, struct Iron_Font font,
                                           Iron_String text, struct Iron_Vector2 position,
                                           float font_size, float spacing,
                                           struct Iron_Color tint);

/* ── Plan 67-02 additions (Task 1) — Text.* namespace ──────────────
 *
 * Default-font measure (MeasureText) + SetTextLineSpacing multiline gap.
 * Both are default-font-only; the custom-font `measure_ex` lives on
 * Font.* and lands alongside the Task 2 Font instance methods.
 *
 * MeasureText/SetTextLineSpacing share Pitfall 1 with DrawText/DrawFPS:
 * they require Window.init() to have populated raylib's default atlas.
 */

int32_t Iron_text_measure(Iron_String text, int32_t font_size);
void    Iron_text_set_line_spacing(int32_t spacing);

/* ── Plan 67-02 additions (Task 2) — Font.* instance methods ───────
 *
 * 8 shims across 3 categories:
 *   - TEXT-08 custom-font draws: draw_ex / draw_pro / draw_codepoint /
 *     draw_codepoints. Every one memcpys Iron_Font (48 B) by value
 *     into a raylib Font local. draw_codepoints additionally takes
 *     Iron_List_int32_t by value (second raylib call site after
 *     Plan 67-01 Font.load_ex).
 *   - TEXT-10 custom-font measure: measure_ex — Vector2 RETURN.
 *   - TEXT-11 glyph lookup: get_glyph_index / get_glyph_info /
 *     get_glyph_atlas_rec. get_glyph_info is the FIRST GlyphInfo
 *     struct-by-value RETURN in the raylib binding (40 B with
 *     embedded Image at offset 16 — under clang's default
 *     -Wlarge-by-value-copy 64 B threshold; Phase 60-03 _Static_assert
 *     grid pins byte identity).
 *
 * Pitfall 7 (67-RESEARCH.md): GlyphInfo.image.data aliases the parent
 * Font's heap glyph array. Do NOT use a returned GlyphInfo after the
 * parent Font is unloaded — the Image field becomes dangling.
 * Flagged in raylib.iron above Font.get_glyph_info.
 */

/* Custom-font draws (TEXT-08) — Font by value INPUT, void RETURN */
void Iron_font_draw_ex(struct Iron_Font font, Iron_String text,
                        struct Iron_Vector2 position, float font_size,
                        float spacing, struct Iron_Color tint);
void Iron_font_draw_pro(struct Iron_Font font, Iron_String text,
                         struct Iron_Vector2 position, struct Iron_Vector2 origin,
                         float rotation, float font_size, float spacing,
                         struct Iron_Color tint);
void Iron_font_draw_codepoint(struct Iron_Font font, int32_t codepoint,
                               struct Iron_Vector2 position, float font_size,
                               struct Iron_Color tint);
void Iron_font_draw_codepoints(struct Iron_Font font, Iron_List_int32_t codepoints,
                                struct Iron_Vector2 position, float font_size,
                                float spacing, struct Iron_Color tint);

/* Custom-font measure (TEXT-10) — Vector2 RETURN */
struct Iron_Vector2 Iron_font_measure_ex(struct Iron_Font font, Iron_String text,
                                          float font_size, float spacing);

/* Glyph lookup (TEXT-11) — get_glyph_info is first GlyphInfo RETURN */
int32_t                Iron_font_get_glyph_index(struct Iron_Font font, int32_t codepoint);
struct Iron_GlyphInfo  Iron_font_get_glyph_info(struct Iron_Font font, int32_t codepoint);
struct Iron_Rectangle  Iron_font_get_glyph_atlas_rec(struct Iron_Font font,
                                                      int32_t codepoint);

/* ── UTF-8 / codepoint (TEXT-12) — Task 1 probe: [Int32] RETURN ─── */
Iron_List_int32_t Iron_text_load_codepoints(Iron_String text);

/* ── UTF-8 / codepoint (TEXT-12) — Task 2 probe: Iron_String from raylib char* ─── */
Iron_String Iron_text_codepoint_to_utf8(int32_t codepoint);

/* ── UTF-8 / codepoint (TEXT-12) — Task 3 bulk ─────────────────────
 *
 * 5 remaining TEXT-12 shims:
 *   - Text.load_utf8([Int32]) -> String: caller-must-free variant of
 *     Pattern 5 (LoadUTF8 returns heap char*; shim copies + UnloadUTF8).
 *   - Text.codepoint_count(String) -> Int32: trivial scalar wrap.
 *   - Text.codepoint_{at,next,previous}(String, Int32) -> (Int32, Int32):
 *     first 2-tuple RETURN with primitive elements. Typedef name per
 *     iron_type_to_string (IRON_TYPE_INT32 -> "Int32") ->
 *     Iron_Tuple_Int32_Int32. Guarded below using the Phase 64-01
 *     IRON_TUPLE_BOOL_VECTOR2_STRUCT_DEFINED belt-and-suspenders style.
 */

/* Tuple typedef for Text.codepoint_{at,next,previous} -> (Int32, Int32).
 * ironc auto-emits this into the generated consumer C TU via the
 * emit_helpers.c:260-294 tuple_append_mangled_component path
 * (IRON_TYPE_INT32 -> "Int32" join "Int32" -> "Iron_Tuple_Int32_Int32").
 * Guarded here so `clang -c iron_raylib.c` compiles standalone. */
#ifndef IRON_TUPLE_INT32_INT32_STRUCT_DEFINED
#define IRON_TUPLE_INT32_INT32_STRUCT_DEFINED
typedef struct {
    int32_t v0;
    int32_t v1;
} Iron_Tuple_Int32_Int32;
#endif

Iron_String            Iron_text_load_utf8(Iron_List_int32_t codepoints);
int32_t                Iron_text_codepoint_count(Iron_String text);
Iron_Tuple_Int32_Int32 Iron_text_codepoint_at(Iron_String text, int32_t offset);
Iron_Tuple_Int32_Int32 Iron_text_codepoint_next(Iron_String text, int32_t offset);
Iron_Tuple_Int32_Int32 Iron_text_codepoint_previous(Iron_String text, int32_t offset);

/* ── TEXT-13 string utilities (20 shims; TextAppend OMITTED) ──────────
 *
 * 17 Text.* utilities + 3 TextFormat overloads (Iron has no varargs FFI,
 * so each call site picks a fixed-arity overload for its scalar type).
 *
 * Text.append OMITTED: raylib's TextAppend mutates a caller-provided
 * char buffer and advances int *position. Iron Strings are immutable —
 * use Text.insert instead for the same semantics with a cleaner API.
 * See 67-RESEARCH.md Open Question 3.
 *
 * Iron_text_join takes Iron_List_Iron_String by value (declared at
 * iron_runtime.h:829 via IRON_LIST_DECL(Iron_String, Iron_String)).
 * Iron_text_split returns Iron_List_Iron_String — first raylib-binding
 * RETURN of a String list. Runtime helpers live in iron_string.c:434-491
 * (Iron_List_Iron_String_create/push/get/len).
 *
 * TextReplace + TextInsert return RL_CALLOC'd char* that the caller
 * MUST free (Pitfall 4). Shim copies into Iron_String then frees. */
Iron_String Iron_text_copy(Iron_String source);
bool        Iron_text_is_equal(Iron_String a, Iron_String b);
int32_t     Iron_text_length(Iron_String text);

Iron_String Iron_text_format_i(Iron_String fmt, int32_t value);
Iron_String Iron_text_format_f(Iron_String fmt, float value);
Iron_String Iron_text_format_s(Iron_String fmt, Iron_String value);

Iron_String Iron_text_subtext(Iron_String text, int32_t position, int32_t length);
Iron_String Iron_text_replace(Iron_String text, Iron_String replace, Iron_String by);
Iron_String Iron_text_insert(Iron_String text, Iron_String insert_text, int32_t position);

Iron_String           Iron_text_join(Iron_List_Iron_String parts, Iron_String delimiter);
Iron_List_Iron_String Iron_text_split(Iron_String text, Iron_String delimiter);

int32_t     Iron_text_find_index(Iron_String text, Iron_String find);

Iron_String Iron_text_to_upper(Iron_String text);
Iron_String Iron_text_to_lower(Iron_String text);
Iron_String Iron_text_to_pascal(Iron_String text);
Iron_String Iron_text_to_snake(Iron_String text);
Iron_String Iron_text_to_camel(Iron_String text);
int32_t     Iron_text_to_integer(Iron_String text);
float       Iron_text_to_float(Iron_String text);

/* ── Audio (Phase 68) ─────────────────────────────────────────────── */
/* Plan 68-01 Task 3: ABI-CALLBACK trampoline infrastructure.
 *
 * raylib's AudioCallback = void(*)(void *bufferData, unsigned int frames).
 * Iron's closure ABI is Iron_Closure { void *env; void (*fn)(void *); }.
 * The trampoline bridges them via a 16-slot fixed-size pool — raylib
 * receives a plain function pointer (audio_cb_N) that invokes the
 * corresponding Iron_Closure entry with (env, buffer, frames).  See
 * Plan 68-01 Task 3 implementation in iron_raylib.c.
 *
 * Plan 68-05 will bind the 5 AUDIO-12 callback entries
 * (SetAudioStreamCallback, AttachAudioStreamProcessor,
 * DetachAudioStreamProcessor, AttachAudioMixedProcessor,
 * DetachAudioMixedProcessor) on top of this. */
typedef Iron_Closure Iron_AudioCallback;

/* ── AUDIO-01 Audio device lifecycle (5 shims) ─────────────────────── */
void  Iron_audio_init(void);
void  Iron_audio_close(void);
bool  Iron_audio_is_ready(void);
void  Iron_audio_set_master_volume(float volume);
float Iron_audio_get_master_volume(void);

/* ── AUDIO-02 Wave load/unload + AUDIO-04 export (6 shims) ─────────── */
struct Iron_Wave Iron_wave_load(Iron_String file_name);
struct Iron_Wave Iron_wave_load_from_memory(Iron_String file_type, Iron_List_uint8_t file_data, int32_t data_size);
bool             Iron_wave_is_valid(struct Iron_Wave wave);
void             Iron_wave_unload(struct Iron_Wave wave);
bool             Iron_wave_export(struct Iron_Wave wave, Iron_String file_name);
bool             Iron_wave_export_as_code(struct Iron_Wave wave, Iron_String file_name);

/* ── AUDIO-03 Wave manipulation (4 shims; wave.to_sound lands in 68-03) */
struct Iron_Wave Iron_wave_copy(struct Iron_Wave wave);
struct Iron_Wave Iron_wave_crop(struct Iron_Wave wave, int32_t init_frame, int32_t final_frame);
struct Iron_Wave Iron_wave_format(struct Iron_Wave wave, int32_t sample_rate, int32_t sample_size, int32_t channels);
Iron_List_float  Iron_wave_load_samples(struct Iron_Wave wave);

/* ── AUDIO-05 Sound load/unload + alias (6 shims) ──────────────────── */
/*
 * Alias ownership asymmetry is visible in the shim naming:
 *   Iron_sound_from_wave    = primary constructor (owns sample data
 *                              once raylib internally copies wave's
 *                              samples into the Sound's AudioStream).
 *   Iron_sound_alias        = shared-sample alias (DOES NOT own data).
 *   Iron_sound_unload       = frees samples; ALL aliases dangle after.
 *   Iron_sound_unload_alias = frees only the AudioStream envelope;
 *                              sample data untouched (stays bound to
 *                              the primary Sound until its unload).
 *
 * Iron entry-point aliasing: Sound.from_wave(wave) and wave.to_sound()
 * are spec-language aliases (AUDIO-05 + AUDIO-03 bridge). ironc's
 * name-mangle emits distinct C symbols for each (Iron_sound_from_wave
 * vs Iron_wave_to_sound — Path B), so a forwarding stub below bridges
 * the second entry point to the primary shim.
 */
struct Iron_Sound Iron_sound_load(Iron_String file_name);
struct Iron_Sound Iron_sound_from_wave(struct Iron_Wave wave);
struct Iron_Sound Iron_sound_alias(struct Iron_Sound source);
bool              Iron_sound_is_valid(struct Iron_Sound sound);
void              Iron_sound_unload(struct Iron_Sound sound);
void              Iron_sound_unload_alias(struct Iron_Sound alias);

/* ── AUDIO-03 bridge: wave.to_sound() — forwards to Iron_sound_from_wave */
struct Iron_Sound Iron_wave_to_sound(struct Iron_Wave wave);

/* ── AUDIO-06 Sound management (5 shims) ───────────────────────────── */
void Iron_sound_play(struct Iron_Sound sound);
void Iron_sound_stop(struct Iron_Sound sound);
void Iron_sound_pause(struct Iron_Sound sound);
void Iron_sound_resume(struct Iron_Sound sound);
bool Iron_sound_is_playing(struct Iron_Sound sound);

/* ── AUDIO-07 Sound configure (3 shims) ────────────────────────────── */
void Iron_sound_set_volume(struct Iron_Sound sound, float volume);
void Iron_sound_set_pitch(struct Iron_Sound sound, float pitch);
void Iron_sound_set_pan(struct Iron_Sound sound, float pan);

/* ── AUDIO-08 Sound update (1 shim, ABI-FLOAT32 INPUT) ─────────────── */
/*
 * First live ABI-FLOAT32 INPUT consumer in the raylib binding. Mirror
 * of Plan 68-02's wave.load_samples (ABI-FLOAT32 RETURN). Iron_List
 * element-type suffix is `float` (matches emit_type_to_c output, per
 * Plan 68-02 SUMMARY deviation note — not `Iron_Float32`).
 */
void Iron_sound_update(struct Iron_Sound sound, Iron_List_float data, int32_t sample_count);

/* ── AUDIO-09 Music load/unload (4 shims) ──────────────────────────── */
/*
 * Music is 48 B (embeds AudioStream + frameCount + looping + ctxType +
 * _ctxData opaque pointer). 2nd live ABI-UINT8 INPUT consumer after
 * Plan 68-02 Wave.load_from_memory; same Iron_List_uint8_t pattern.
 *
 * Note: LoadMusicStream returns Music with ctxType=0 (MUSIC_AUDIO_NONE)
 * and _ctxData=NULL on failure + prints TRACELOG warning. Users MUST
 * call music.is_valid() before play/update.
 *
 * Is-predicate asymmetry: raylib names IsMusicValid (no Stream) but
 * IsMusicStreamPlaying (with Stream). Iron flattens to music.is_valid
 * + music.is_playing. The shim bodies call the correctly-named raylib
 * function internally — the Iron surface stays consistent.
 */
struct Iron_Music Iron_music_load(Iron_String file_name);
struct Iron_Music Iron_music_load_from_memory(Iron_String file_type, Iron_List_uint8_t data, int32_t data_size);
bool              Iron_music_is_valid(struct Iron_Music music);
void              Iron_music_unload(struct Iron_Music music);

/* Music.set_looping — Iron val-field write fallback.
 *
 * raylib's `Music` struct exposes `bool looping` as a writable public
 * field. Iron's `object Music` (raylib.iron:921) declares it as a
 * `val` field, which is read-only at the Iron level. This shim
 * provides a raylib-semantically-equivalent setter using the mutating-
 * return-by-value pattern (Phase 66-03 template applied to an audio
 * type): memcpy INPUT → mutate local → memcpy OUTPUT to a fresh Music
 * value. Iron callers use `val m2 = m.set_looping(true)`.
 *
 * This is a defensive fallback. Pre-build ironc source inspection
 * (typecheck.c:2758 assignment check) shows val-immutability is only
 * enforced on IRON_NODE_IDENT targets (local variables), NOT on
 * IRON_NODE_FIELD_ACCESS targets — so `m.looping = true` likely
 * compiles in a future ironc rebuild. The shim lands now to keep
 * Music.looping write semantics reachable regardless of ironc path.
 * See 68-04 SUMMARY for the full rationale.
 */
struct Iron_Music Iron_music_set_looping(struct Iron_Music music, bool looping);

/* ── AUDIO-10 Music management (6 shims) ───────────────────────────── */
/*
 * Naming asymmetry preserved in the shims: raylib has IsMusicValid
 * (no Stream) but IsMusicStreamPlaying (with Stream). Iron flattens
 * both to music.is_valid + music.is_playing. Each shim dispatches
 * to the correctly-named raylib function internally.
 *
 * music.update() must be called every frame by user code in the main
 * loop to feed raylib's audio buffer. raylib is a no-op when the
 * audio device is not ready.
 */
void Iron_music_play(struct Iron_Music music);
bool Iron_music_is_playing(struct Iron_Music music);
void Iron_music_update(struct Iron_Music music);
void Iron_music_stop(struct Iron_Music music);
void Iron_music_pause(struct Iron_Music music);
void Iron_music_resume(struct Iron_Music music);

/* ── 3D Drawing (Phase 69) ────────────────────────────────────────── */
/* ── Models (Phase 70) ────────────────────────────────────────────── */
/* ── Shaders (Phase 71) ───────────────────────────────────────────── */
/* ── File I/O & Utils (Phase 72) ──────────────────────────────────── */

#endif /* IRON_RAYLIB_H */
