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

/* Math namespace — scalar helpers (MATH-01, raymath.h lines 178-228) */
float Iron_math_clamp(float value, float min, float max);
float Iron_math_lerp(float start, float end, float amount);
float Iron_math_normalize(float value, float start, float end);
float Iron_math_wrap(float value, float min, float max);
float Iron_math_remap(float value, float in_start, float in_end, float out_start, float out_end);
bool  Iron_math_float_equals(float x, float y);

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

/* ── Textures & Images (Phase 66) ─────────────────────────────────── */
/* ── Text & Fonts (Phase 67) ──────────────────────────────────────── */
/* ── Audio (Phase 68) ─────────────────────────────────────────────── */
/* ── 3D Drawing (Phase 69) ────────────────────────────────────────── */
/* ── Models (Phase 70) ────────────────────────────────────────────── */
/* ── Shaders (Phase 71) ───────────────────────────────────────────── */
/* ── File I/O & Utils (Phase 72) ──────────────────────────────────── */

#endif /* IRON_RAYLIB_H */
