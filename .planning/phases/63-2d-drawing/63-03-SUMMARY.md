---
phase: 63-2d-drawing
plan: 03
subsystem: stdlib-raylib
tags: [raylib, ffi, rectangle, triangle, polygon, array-abi, iron-list, memcpy, shim]

requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Rectangle / Iron_Vector2 / Iron_Color structs + _Static_assert ABI grid pinning Rectangle/Vector2/Color byte-identity to raylib C types"
  - phase: 63-2d-drawing (Plan 01)
    provides: "13 frame + stack-mode Draw.* bindings; proven memcpy-for-Color-by-value template"
  - phase: 63-2d-drawing (Plan 02)
    provides: "17 pixel/line/circle/ellipse/ring Draw.* bindings; Vector2-by-value INPUT ABI proven across 10 shims; two-Color variant (DrawCircleGradient) proven"

provides:
  - "19 new Iron_draw_* C prototypes + shim implementations for 12 rectangle, 2 fixed-point triangle, 3 polygon, 2 array-triangle primitives"
  - "19 new Iron func Draw.* stubs in raylib.iron covering DRAW2D-12 / 13 / 14"
  - "First validated Rectangle struct-by-value INPUT ABI across 7 shim sites (rec/pro/gradient_ex/lines_ex/rounded/rounded_lines/rounded_lines_ex)"
  - "Validated 4-color shim variant (DrawRectangleGradientEx, quadruple memcpy)"
  - "Validated Iron [Vector2] array ABI — ironc lowers to Iron_List_Iron_Vector2 struct-BY-VALUE (items/count/capacity wrapper)"
  - "Iron_List_Iron_Vector2 typedef landed in iron_raylib.h under IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED guard (mirrors iron_net.h's Iron_List_Iron_Address pattern)"

affects:
  - 63-04 (consumer re-enablement of pong/hello_raylib/game_raylib; DrawLineStrip + 5× DrawSpline<T> can now use proven Iron_List_Iron_Vector2 pattern)
  - 64-collision
  - 66-textures (reuses memcpy template across Rectangle for source-rect draws)
  - 67-text-fonts (reuses Rectangle memcpy template for text-rect bounds)
  - 69-3d-drawing (reuses memcpy template with Vector3/Matrix)

tech-stack:
  added:
    - "Iron_List_Iron_Vector2 typedef (guarded by IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED) — items: Iron_Vector2 *, count: int64_t, capacity: int64_t"
  patterns:
    - "Rectangle struct-by-value INPUT: memcpy(&r, &rec, sizeof(Rectangle)) for 7 Rectangle-taking shims"
    - "4-color quadruple memcpy: DrawRectangleGradientEx takes topLeft/bottomLeft/topRight/bottomRight — 4 Color stack locals fed by 4 memcpys"
    - "Triple-Vector2 memcpy: triangle/triangle_lines memcpy 3 Vector2 params into a/b/c locals + 1 Color into col"
    - "Iron_List_Iron_Vector2 BY VALUE: shim receives the full items/count/capacity wrapper (24 bytes) directly; reinterprets .items as const Vector2 * for raylib forwarding"

key-files:
  created: []
  modified:
    - src/stdlib/iron_raylib.h
    - src/stdlib/iron_raylib.c
    - src/stdlib/raylib.iron

key-decisions:
  - "Task 1 probe outcome A confirmed: ironc emits `void Iron_draw_probe_array(Iron_List_Iron_Vector2 points, int32_t count);` — the Iron_List_<T> struct is passed BY VALUE, not by pointer. Plan's Variant 2 speculation (pointer form) was wrong direction; the actual lowering matches ARRAY_PARAM_LIST struct-by-value."
  - "Added Iron_List_Iron_Vector2 typedef to iron_raylib.h under IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED include guard — mirrors iron_net.h's Iron_List_Iron_Address block (lines 269-276). The guard prevents double-definition conflict when emit_c.c's Phase 3 struct emission also declares the same type in generated user code."
  - "DrawTriangleFan / DrawTriangleStrip shim signature: `void Iron_draw_triangle_fan(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color);` — points BY VALUE (matches ironc's lowering). Forwards points.items as (const Vector2 *) leveraging Phase 60-02's _Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2)) byte-identity proof."
  - "Plan's acceptance criteria specified total func Draw.* count of '46 or 48' based on a 29-stub prior state; actual prior state was 30 (Plan 63-02's 17 + Plan 63-01's 13 = 30). Final count is 30 + 19 = 49. The plan's arithmetic was off-by-one; the per-function grep list is authoritative (12 rect + 2 triangle + 3 poly + 2 array triangle = 19)."
  - "Task 1 produced zero net file changes (probe added to raylib.iron, ironc observed, probe removed). No commit for Task 1 — observation result documented here in SUMMARY instead."
  - "ironc invoked ONCE (the probe build, Task 1) per HANDOFF.md memory discipline (ironc is ~10 GB/run). Tasks 2 and 3 used `clang -c` verification only. End-to-end pong.iron validation is Plan 63-04's territory."

patterns-established:
  - "Rectangle struct-by-value INPUT: 7 shims follow `Rectangle r; memcpy(&r, &rec, sizeof(Rectangle));` — pattern generalizes to any raylib function taking `Rectangle` by value (collision in Phase 64, texture source-rects in Phase 66, text bounds in Phase 67)."
  - "Iron_List_<T> BY VALUE at FFI: the 24-byte items/count/capacity wrapper crosses the boundary directly. Shim's forwarding pattern is `SomeRaylibFunc((const T *)points.items, (int)count, ...)`. Applies to all future [T] parameters — DrawLineStrip, 5× DrawSpline<T> in Plan 63-04."
  - "4-color shim variant: 4 Color stack locals + 4 memcpys for gradient primitives with tl/bl/tr/br. Pattern extends Plan 63-02's two-Color template (DrawCircleGradient) to 4-corner gradients."
  - "Triple-Vector2 shim variant: 3 Vector2 stack locals + 3 memcpys for primitives with 3 vertex parameters (triangle/triangle_lines). Companion to Plan 63-02's double-Vector2 pattern (line_v/line_ex/line_bezier)."

requirements-completed: [DRAW2D-12, DRAW2D-13, DRAW2D-14]

duration: 5min
completed: 2026-04-16
---

# Phase 63 Plan 03: Rectangles, Triangles, Polygons Summary

**19 Draw.\* rectangle/triangle/polygon bindings with first Rectangle struct-by-value INPUT and first Iron [Vector2] array ABI pattern using Iron_List_Iron_Vector2 by-value — zero warnings from clang, DRAW2D-12/13/14 fully closed**

Plan 63-03 bound raylib's rectangle (12 functions), triangle (2 fixed + 2 array = 4), and polygon (3 functions) primitives — 19 total — covering DRAW2D-12, DRAW2D-13, DRAW2D-14. Task 1 was a dedicated Iron `[Vector2]` array ABI probe (the only genuine unknown in Phase 63 per 63-RESEARCH.md Pattern 4 + Pitfall 5). The probe PASSED with outcome A: ironc lowers `[Vector2]` parameters to `Iron_List_Iron_Vector2` struct-by-value, and the triangle_fan/triangle_strip shims forward `points.items` as `const Vector2 *`. This unblocks all 9 array-taking raylib functions (DrawLineStrip + 2× triangle array + 5× DrawSpline<T> + 1× DrawLineStripConcave) for Plan 63-04's fallback section.

## Files Modified

- `src/stdlib/iron_raylib.h` — +49 lines (19 prototypes + Iron_List_Iron_Vector2 typedef + guards)
- `src/stdlib/iron_raylib.c` — +173 lines (19 shim implementations)
- `src/stdlib/raylib.iron` — +62 lines (19 `func Draw.*` empty stubs)

## Task 1 — Array ABI Probe Result (outcome A — ironc lowers `[Vector2]` cleanly)

**Probe stub landed temporarily:**
```iron
func Draw.probe_array(points: [Vector2], count: Int32) {}
```

**Probe caller (`iron_array_probe.iron` at project root):**
```iron
import raylib
func main() {
    val p1 = Vector2(Float32(0.0), Float32(0.0))
    val p2 = Vector2(Float32(10.0), Float32(10.0))
    val p3 = Vector2(Float32(20.0), Float32(0.0))
    val pts: [Vector2] = [p1, p2, p3]
    Draw.probe_array(pts, 3)
}
```

**ironc build result:**
```
Undefined symbols for architecture arm64:
  "_Iron_draw_probe_array", referenced from:
      _Iron_main in iron_array_probe-609d65.o
      _main in iron_array_probe-609d65.o
ld: symbol(s) not found for architecture arm64
```

ironc COMPILED TO C SUCCESSFULLY (outcome A). Link failure on `Iron_draw_probe_array` is expected — the C body was intentionally absent. The emitted C file (`.iron-build/iron_array_probe.c`) reveals the exact lowering:

**Exact prototype ironc generated:**
```c
void Iron_draw_probe_array(Iron_List_Iron_Vector2 points, int32_t count);
```

**Supporting struct typedef ironc emitted for the [Vector2] array type:**
```c
/* Phase 56: Iron_List type for mono-collapsed Iron_Vector2 */
typedef struct Iron_List_Iron_Vector2 {
    Iron_Vector2    *items;
    int64_t count;
    int64_t capacity;
} Iron_List_Iron_Vector2;
IRON_LIST_DECL(Iron_Vector2, Iron_Vector2)
IRON_LIST_IMPL(Iron_Vector2, Iron_Vector2)
```

**Call-site C code ironc generated at the user call site:**
```c
Iron_Vector2 _v5  = { .x = (/* cast */ (float)0),  .y = (/* cast */ (float)0) };
Iron_Vector2 _v10 = { .x = (/* cast */ (float)10), .y = (/* cast */ (float)10) };
Iron_Vector2 _v15 = { .x = (/* cast */ (float)20), .y = (/* cast */ (float)0) };
Iron_List_Iron_Vector2 _v16 = Iron_List_Iron_Vector2_create();
Iron_List_Iron_Vector2_push(&_v16, _v5);
Iron_List_Iron_Vector2_push(&_v16, _v10);
Iron_List_Iron_Vector2_push(&_v16, _v15);
Iron_draw_probe_array(_v16, ((int64_t)3LL));
```

**Four key facts revealed by the probe:**

1. **Outcome A, ARRAY_PARAM_LIST mode:** ironc does NOT use ARRAY_PARAM_CONST_PTR. The `[Vector2]` parameter becomes the full `Iron_List_Iron_Vector2` struct wrapper — NOT a bare `const Iron_Vector2 *`.
2. **Struct passed BY VALUE, not by pointer:** The call site passes `_v16` directly (not `&_v16`). The shim receives the 24-byte items/count/capacity wrapper as a value parameter.
3. **List built via runtime helpers:** ironc emits `Iron_List_Iron_Vector2_create()` + `_push()` calls. The user writes `val pts: [Vector2] = [p1, p2, p3]` — ironc expands to push each element.
4. **Count passed separately (plan's invariant verified):** Even though `.count` is available inside the Iron_List wrapper, ironc preserved the redundant explicit `count: Int32` parameter. The shim's convention (use caller-supplied count, ignore wrapper count) matches raylib's C signature convention.

**Probe removed from all three files after observation.** Zero probe artifacts remain:
```
grep -c 'probe_array' src/stdlib/raylib.iron src/stdlib/iron_raylib.h src/stdlib/iron_raylib.c
src/stdlib/raylib.iron:0
src/stdlib/iron_raylib.h:0
src/stdlib/iron_raylib.c:0
```

## Task 2 — 19 C prototypes + shim implementations

17 baseline shims (12 rectangle + 2 fixed triangle + 3 polygon) + 2 conditional shims (triangle_fan / triangle_strip) land in `src/stdlib/iron_raylib.c`. Prototypes mirror into `src/stdlib/iron_raylib.h`.

**Rectangle struct-by-value INPUT across 7 shims — validated on first compile:**

| # | Shim                                   | Rectangle path                                   |
| - | -------------------------------------- | ------------------------------------------------ |
| 1 | `Iron_draw_rectangle_rec`              | `memcpy(&r, &rec, sizeof(Rectangle))` then call  |
| 2 | `Iron_draw_rectangle_pro`              | Rectangle + Vector2 origin — double memcpy       |
| 3 | `Iron_draw_rectangle_gradient_ex`      | Rectangle + 4× Color — quintuple memcpy          |
| 4 | `Iron_draw_rectangle_lines_ex`         | Rectangle + Color — double memcpy                |
| 5 | `Iron_draw_rectangle_rounded`          | Rectangle + Color — double memcpy                |
| 6 | `Iron_draw_rectangle_rounded_lines`    | Rectangle + Color — double memcpy                |
| 7 | `Iron_draw_rectangle_rounded_lines_ex` | Rectangle + Color — double memcpy                |

Phase 60-02's `_Static_assert(sizeof(struct Iron_Rectangle) == sizeof(Rectangle))` plus 4 per-field offset asserts guaranteed byte-identity — clang compile ZERO warnings on first attempt.

**4-color quadruple memcpy variant validated (DrawRectangleGradientEx):**

```c
void Iron_draw_rectangle_gradient_ex(struct Iron_Rectangle rec,
                                     struct Iron_Color tl, struct Iron_Color bl,
                                     struct Iron_Color tr, struct Iron_Color br) {
    Rectangle r;
    Color a, b, c, d;
    memcpy(&r, &rec, sizeof(Rectangle));
    memcpy(&a, &tl,  sizeof(Color));
    memcpy(&b, &bl,  sizeof(Color));
    memcpy(&c, &tr,  sizeof(Color));
    memcpy(&d, &br,  sizeof(Color));
    DrawRectangleGradientEx(r, a, b, c, d);
}
```

Argument order follows raylib.h:104 — topLeft / bottomLeft / topRight / bottomRight.

**Iron_List_Iron_Vector2 typedef added to iron_raylib.h with include guard:**

```c
#ifndef IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED
#define IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED
typedef struct Iron_List_Iron_Vector2 {
    struct Iron_Vector2 *items;
    int64_t              count;
    int64_t              capacity;
} Iron_List_Iron_Vector2;
#endif
```

Pattern copied from `src/stdlib/iron_net.h:269-276` (the `Iron_List_Iron_Address` block). The `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` guard prevents double-definition when emit_c.c's Phase 3 codegen also declares the same type in generated user code (avoids the "two incompatible definitions of Iron_List_Iron_Vector2" linker error that would otherwise trip on any consumer that both `import raylib` and uses `[Vector2]`).

**Triangle array variant shim signature (struct-by-value, not pointer):**

```c
void Iron_draw_triangle_fan(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawTriangleFan((const Vector2 *)points.items, (int)count, c);
}
```

Phase 60-02's `_Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2))` plus 2 per-field offset asserts make the `(const Vector2 *)points.items` reinterpret byte-safe. Triangle_strip shim follows the same shape.

## Task 3 — 19 Iron-side `func Draw.*` stubs

Appended to `src/stdlib/raylib.iron` after Plan 63-02's ring primitives. Grouped under 4 section banners (Rectangle primitives / Triangle primitives / Polygon primitives / Triangle array variants). Each section carries a top-level comment documenting parameter semantics and raylib C equivalence.

**Total `func Draw.*` count parity with `Iron_draw_*` prototypes:**
- Iron side: 49 `func Draw.*` stubs
- C side:    49 `Iron_draw_*` prototypes in iron_raylib.h
- Parity: verified

## Plan 63-04 Handoff

The array ABI is SOLVED. Plan 63-04 can now:
- **Bind DrawLineStrip** (deferred from Plan 63-02) using `Iron_List_Iron_Vector2 points` by value.
- **Bind all 5 DrawSpline<T> functions** (DRAW2D-15) using the same Iron_List pattern.
- **Skip the Plan 63-04 fallback section** — no functions need to descope to Phase 73.
- **Execute pong.iron / game_raylib.iron / hello_raylib.iron re-enablement** with full knowledge of how `[Vector2]` crosses the FFI.

The Iron_List_Iron_Vector2 typedef in iron_raylib.h is reusable — Plan 63-04's DrawLineStrip shim should use the EXISTING typedef (already present), not redeclare it.

## Verification

**All acceptance criteria passed:**

```bash
$ grep -c '^void Iron_draw_rectangle' src/stdlib/iron_raylib.h
12

$ grep -c '^void Iron_draw_triangle' src/stdlib/iron_raylib.h
4

$ grep -c '^void Iron_draw_poly' src/stdlib/iron_raylib.h
3

$ grep -c '^void Iron_draw_' src/stdlib/iron_raylib.h
49

$ grep -c '^func Draw\.' src/stdlib/raylib.iron
49

$ grep -c 'probe_array' src/stdlib/raylib.iron src/stdlib/iron_raylib.h src/stdlib/iron_raylib.c
src/stdlib/raylib.iron:0
src/stdlib/iron_raylib.h:0
src/stdlib/iron_raylib.c:0

$ clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra
# EXIT 0, zero warnings

$ clang -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra
# EXIT 0, zero warnings
```

## Deviations from Plan

**None for Rules 1-3** (auto-fix).

**Plan arithmetic discrepancies (pre-identified by planner, resolved at execution):**
- Plan's "46 or 48 func Draw.* total" expected count was based on a 29-stub prior baseline; actual prior was 30 (Plan 63-02 landed 17, Plan 63-01 landed 13, 13 + 17 = 30). Final total is 30 + 19 = 49. The plan's per-function grep enumeration (12 + 2 + 3 + 2 = 19 new) was always authoritative; the "46 or 48" prose was an arithmetic typo.
- Plan's Variant 2 array-ABI shim signature speculated `Iron_List_Iron_Vector2 *points` (by pointer). Actual ironc lowering is `Iron_List_Iron_Vector2 points` (by value). Shim signature adjusted to match ironc's output — no impact on correctness, just matches the real ABI.

## Authentication Gates

None.

## Self-Check: PASSED
