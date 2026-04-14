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

/* Phase 61 ABI smoke test — proves Iron's FFI can return a struct by
 * value from a shim. If the smoke test passes, the rest of Phase 61
 * can freely use struct-return wrappers for GetWindowPosition /
 * GetWindowScaleDPI / GetMonitorPosition / GetClipboardImage.
 * DELETE this prototype after Phase 61 verification if not needed. */
struct Iron_Vector2 Iron_window_abi_smoke_test(void);

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
