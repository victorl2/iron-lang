# Phase 64: Collision (2D + 3D) - Research

**Researched:** 2026-04-16
**Domain:** raylib stdlib binding — 19 collision functions (11 2D + 8 3D) as idiomatic Iron methods on data-carrying receivers (`Rectangle`, `Vector2`, `Ray`, `BoundingBox`) and a shared `object Collision {}` namespace
**Confidence:** HIGH

## Summary

Phase 64 binds raylib's 19 collision-detection functions (11 in `rshapes.c`, 8 in `rmodels.c`) through the established shim-only pattern (empty-body `func Type.method(...) {}` in `raylib.iron` → `Iron_<type>_<method>` in `iron_raylib.c` → raylib C call). The three "unverified mechanisms" flagged in CONTEXT.md all resolve to **HIGH confidence green** after source-level investigation:

1. **Instance methods on data-carrying objects work** — `iron_net.iron`'s `TcpSocket { val fd: Int }` with `func TcpSocket.read(s: TcpSocket, ...)` lowers cleanly to `Iron_tcpsocket_read(Iron_TcpSocket s, ...)` by value (`iron_net.c:586`). The HIR→LIR mangler (`src/hir/hir_to_lir.c:1096-1297`) dispatches on `obj_type->object.decl->name` without caring whether the object has fields. Rectangle/Vector2/Ray/BoundingBox are treated identically to Draw/Keyboard/Collision namespace types.
2. **Tuple returns lower to a packed struct-by-value return, NOT multiple out-pointers** — `iron_net.h:48-66` and `iron_net.c:586-629` demonstrate `-> (Int, NetError)` lowers to `Iron_Result_Int_Error` which is `typedef struct { int64_t v0; Iron_NetError v1; } Iron_Tuple_Int_NetError`. The shim returns the struct by value; the caller destructures. `CheckCollisionLines` fits this mold exactly.
3. **Large struct pass-by-value through the shim is a solved problem** — `memcpy(&rl, &iron, sizeof(...))` is the established template (used for Camera2D/24B, RenderTexture2D/44B, Rectangle/16B in Phase 63). Mesh at 120 bytes and Matrix at 64 bytes ride the same C ABI path — the calling convention (AAPCS64 on ARM64, SysV on x86-64) handles the register-vs-memory decision transparently at the C level since both Iron's generated C and raylib's compiled C agree on the compiler.

**Primary recommendation:** Proceed with the CONTEXT.md design verbatim. Natural-receiver methods on `Rectangle`/`Vector2`/`Ray`/`BoundingBox` plus `object Collision {}` for pair-based tests. Tuple-return `(Bool, Vector2)` for `CheckCollisionLines`. Execute 2 plans (64-01 2D, 64-02 3D). No probe task is strictly necessary because all three mechanisms have clear source-level precedent; a 1-function smoke test as the first commit in Plan 64-01 is still prudent belt-and-suspenders.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Inherited from Phase 60/61/62/63:**
- **Binding architecture:** shim-only. Every raylib call goes through `src/stdlib/iron_raylib.c` via empty-body foreign-method stubs. Iron `func Rectangle.collides(other: Rectangle) -> Bool {}` lowers to `Iron_rectangle_collides(...)` in C.
- **Method casing:** `snake_case`. Implementation follows Phase 62 precedent: `rect_a.collides(rect_b)`, `ray.hit_sphere(center, radius)`, `point.inside_rect(rect)`, etc. ROADMAP.md camelCase examples (`rectA.collides`, `ray.hitSphere`) are narrative prose, not API spec.
- **Struct-by-value argument passing:** validated across Phases 61–63. `memcpy(&rl, &iron, sizeof(...))` inside each shim per Phase 63's template.
- **Struct-by-value return:** validated in Phase 61 (Vector2) and Phase 63 (spline evaluator Vector2 memcpy-out). Phase 64 extends to `Rectangle` (GetCollisionRec) and `RayCollision` (5 `GetRayCollision*`).
- **Array input:** reuse Phase 63's `Iron_List_Iron_Vector2` by-value ABI for `CheckCollisionPointPoly(points: [Vector2])`. `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` guard in `iron_raylib.h:589-596` already present. `emit_structs.c` Phase 63-04 fix auto-emits the typedef.
- **Float32 vs Float:** raylib uses `float` everywhere. All such params are Iron `Float32`.
- **Enum parameters:** none in Phase 64.
- **Layout verification:** no new types. The 392 existing `_Static_assert` entries in `iron_raylib_layout.c` already pin every Phase 64 input/output struct layout.
- **Section marker:** `/* ── Collision (Phase 64) ─────────── */` in `iron_raylib.c` and `iron_raylib.h` (already scaffolded at `iron_raylib.h:637`).
- **Auto-generated prototypes** (commit e3e5eee, 2026-04-14): `emit_c` auto-generates stdlib foreign-method prototypes. Planner does NOT hand-maintain prototype sync.
- **Clangd false-positive workaround:** verify with `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra`, not clangd.
- **Memory discipline:** ironc ~10 GB per invocation. Canonical validation = `clang -c` round-trip + a standalone smoke test. Phase 64 does NOT add consumer-file markers (pong/game_raylib/hello_raylib don't use collision).

**Phase 64 specific — Method receiver choice (natural receivers + `Collision` namespace):**

*2D natural receivers:*
- `Rectangle.collides(other: Rectangle) -> Bool` — `CheckCollisionRecs`
- `Rectangle.intersection(other: Rectangle) -> Rectangle` — `GetCollisionRec`
- `Rectangle.contains_point(point: Vector2) -> Bool` — `CheckCollisionPointRec`
- `Rectangle.collides_circle(center: Vector2, radius: Float32) -> Bool` — `CheckCollisionCircleRec`
- `Vector2.inside_triangle(p1: Vector2, p2: Vector2, p3: Vector2) -> Bool` — `CheckCollisionPointTriangle`
- `Vector2.inside_polygon(points: [Vector2]) -> Bool` — `CheckCollisionPointPoly`
- `Vector2.on_line(p1: Vector2, p2: Vector2, threshold: Int32) -> Bool` — `CheckCollisionPointLine`

*2D `Collision` namespace:*
- `Collision.circles(c1: Vector2, r1: Float32, c2: Vector2, r2: Float32) -> Bool` — `CheckCollisionCircles`
- `Collision.circle_line(center: Vector2, radius: Float32, p1: Vector2, p2: Vector2) -> Bool` — `CheckCollisionCircleLine`
- `Collision.point_circle(point: Vector2, center: Vector2, radius: Float32) -> Bool` — `CheckCollisionPointCircle`
- `Collision.lines(start_a: Vector2, end_a: Vector2, start_b: Vector2, end_b: Vector2) -> (Bool, Vector2)` — `CheckCollisionLines` (tuple return)

*3D natural receivers:*
- `BoundingBox.collides(other: BoundingBox) -> Bool` — `CheckCollisionBoxes`
- `BoundingBox.collides_sphere(center: Vector3, radius: Float32) -> Bool` — `CheckCollisionBoxSphere`
- `Ray.hit_sphere(center: Vector3, radius: Float32) -> RayCollision` — `GetRayCollisionSphere`
- `Ray.hit_box(box: BoundingBox) -> RayCollision` — `GetRayCollisionBox`
- `Ray.hit_mesh(mesh: Mesh, transform: Matrix) -> RayCollision` — `GetRayCollisionMesh`
- `Ray.hit_triangle(p1: Vector3, p2: Vector3, p3: Vector3) -> RayCollision` — `GetRayCollisionTriangle`
- `Ray.hit_quad(p1: Vector3, p2: Vector3, p3: Vector3, p4: Vector3) -> RayCollision` — `GetRayCollisionQuad`

*3D `Collision` namespace:*
- `Collision.spheres(c1: Vector3, r1: Float32, c2: Vector3, r2: Float32) -> Bool` — `CheckCollisionSpheres`

**Plan slicing — 2 plans:**
- **Plan 64-01 (2D, COLL-01)** — 11 functions. Validates methods-on-data-types dispatch, `object Collision {}` creation, tuple-return on foreign-method-stub, `Iron_List_Iron_Vector2` reuse outside Draw. Wave 1.
- **Plan 64-02 (3D, COLL-02)** — 8 functions. Validates `RayCollision` struct-by-value return, `Mesh` pass-by-value (first time), `Matrix` pass-by-value (first time as function arg). Wave 2 (depends on 64-01 dispatch verification). Sequential execution per `parallelization: false`.

### Claude's Discretion

- **Exact shim signatures** — whether `Iron_rectangle_collides` takes `(Iron_Rectangle, Iron_Rectangle)` directly or by const-ref. Default: by value, per Phase 63 pattern (all Draw.* shims take structs by value).
- **Whether `Vector2.inside_circle(center, radius)` replaces `Collision.point_circle(point, center, radius)`** — pick one for minimal surface; default to namespace form.
- **Smoke-test file location/content** — Phase 63 didn't add a new test file. Default for Phase 64: minimal `tests/manual/collision_smoke.iron` that calls one function per receiver. Planner may skip if `clang -c iron_raylib.c` + `ironc check raylib.iron` round-trip is sufficient.
- **Parameter naming** — `other` vs `b` vs `rec` for the second Rectangle; `center`/`radius` pair order; etc. Pick the form that reads most naturally at call site.
- **Method placement order in `raylib.iron`** — by raylib-function order, by receiver, or by category. Default: group by receiver, Collision namespace at the end.

### Deferred Ideas (OUT OF SCOPE)

- **Swept collision (continuous collision detection).** raylib 5.5 public API has no swept-collision functions.
- **Spatial partitioning (BVH, quadtree, spatial hash).** Not part of raylib's public API; physics-library territory.
- **Convex-hull / SAT collisions.** Beyond raylib's exposed surface.
- **`GetRayCollisionModel(Ray, Model)` convenience.** raylib has no single-function Ray↔Model test. A helper `Ray.hit_model(model)` could land in Phase 70 (Models) or Phase 73 (API polish).
- **Batched collision (array-of-rects vs array-of-rects).** User-level loop; not exposed by raylib.
- **Collision mask/layer system.** Game-engine abstraction above raylib.
- **Distance/nearest-point queries on shapes.** raymath has vector distance helpers; shape-specific math lives in Phase 65 or user code.
- **Documentation tile in ROADMAP.md for how `Collision.*` vs `Rectangle.*` dispatch differs.** Nice-to-have; planner's discretion during Plan 64-01.

</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| COLL-01 | User can test 2D rectangle/circle/point collisions — `checkCollisionRecs, checkCollisionCircles, checkCollisionCircleRec, checkCollisionCircleLine, checkCollisionPointRec, checkCollisionPointCircle, checkCollisionPointTriangle, checkCollisionPointLine, checkCollisionPointPoly, checkCollisionLines, getCollisionRec` — invoked as methods on `Rectangle`/`Vector2` where idiomatic | 11 functions in `raylib.h:1305-1315`. 4 Rectangle-receiver methods, 3 Vector2-receiver methods, 4 `Collision.*` namespace functions (incl. `lines` with tuple return). Reuses Phase 63 Iron_List_Iron_Vector2 ABI for `CheckCollisionPointPoly`. See "Code Examples" §1–11 below. |
| COLL-02 | User can test 3D collisions — `checkCollisionSpheres, checkCollisionBoxes, checkCollisionBoxSphere, getRayCollisionSphere, getRayCollisionBox, getRayCollisionMesh, getRayCollisionTriangle, getRayCollisionQuad` — invoked as methods on `Ray`/`BoundingBox` where idiomatic | 8 functions in `raylib.h:1611-1619`. 2 BoundingBox-receiver methods, 5 Ray-receiver methods (first `RayCollision` struct-return outside spline evaluator paradigm), 1 `Collision.spheres` namespace. First phase to cross FFI with Mesh (120 B) and Matrix (64 B) by value. See "Code Examples" §12–19 below. |

</phase_requirements>

## Standard Stack

### Core

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| raylib  | 5.5 (vendored at `src/vendor/raylib/`) | 19 collision functions in `rshapes.c` / `rmodels.c` | The only raylib version in scope per PROJECT.md. All collision functions are pure computation — no GPU state, no stdlib dependencies. |
| Iron compiler `emit_c` (e3e5eee) | landed 2026-04-14 | Auto-generates foreign-method prototypes | Planner doesn't hand-sync prototypes between `raylib.iron` stubs and generated C. |
| `emit_structs.c` Rule 3 fix (Phase 63-04) | landed 2026-04-16 | Auto-emits `Iron_List_<T>` typedefs for foreign-method-stub params | `Iron_List_Iron_Vector2` typedef needed by `CheckCollisionPointPoly` auto-emits because `Vector2.inside_polygon(points: [Vector2])` is a foreign-method stub. |

### Supporting

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| clang / clang-cl | system | Direct `clang -c src/stdlib/iron_raylib.c` round-trip | Every iron_raylib.c edit (before ironc end-to-end). Workaround for clangd false positives — iron_raylib.c is not in the CMake `iron_stdlib` target. |
| `_Static_assert` / layout TU | 392 existing asserts | ABI byte-identity for Iron_Rectangle, Iron_Vector2, Iron_Vector3, Iron_Ray, Iron_RayCollision, Iron_BoundingBox, Iron_Mesh, Iron_Matrix | All assertions for Phase 64 input/output types already landed in Plans 60-02 through 60-05. No additions needed. |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Instance methods on `Rectangle`/`Vector2`/`Ray`/`BoundingBox` | Everything under `Collision.*` namespace (fallback) | Preserves the shim-only uniform path, but loses the `rect_a.collides(rect_b)` idiom that ROADMAP.md success criteria imply. Fallback plan if instance-method dispatch proves broken — which it ISN'T per source investigation below. |
| Tuple return `(Bool, Vector2)` for `CheckCollisionLines` | Iron object `LineIntersection { hit: Bool; point: Vector2 }` | Tuple form matches PROJECT.md's v1.2.0 tuple-return feature, matches existing iron_net.c precedent, and keeps raylib surface consistent with `(Sock, Err)` patterns. Object fallback is heavier (named type, separate constructor) with no ergonomic win. |
| Positional `Collision.point_circle(point, center, radius)` | Receiver form `Vector2.inside_circle(center, radius)` | Both work. Default is namespace form to keep Vector2's method surface small (per CONTEXT.md Claude's Discretion). Vector2 already gets 3 methods (inside_triangle, inside_polygon, on_line). Adding inside_circle is plausible; trade-off is API breadth on one receiver vs explicit namespace routing. |

**Installation:** N/A — raylib is vendored. No new packages to add.

**Version verification:** raylib 5.5 vendored at `src/vendor/raylib/` is stable since v2.0 milestone started 2026-04-13. No version bump in v2.0.0-alpha per PROJECT.md constraints.

## Architecture Patterns

### Recommended Project Structure

Phase 64 touches 3 files (no new files):

```
src/stdlib/
├── raylib.iron           # Extend existing `object Rectangle/Vector2/Ray/BoundingBox` with methods; add `object Collision {}`
├── iron_raylib.h         # Add Iron_rectangle_*, Iron_vector2_inside_*, Iron_collision_*,
│                          Iron_boundingbox_*, Iron_ray_hit_* prototypes in the existing
│                          `/* ── Collision (Phase 64) ─────────── */` section (already scaffolded at line 637)
└── iron_raylib.c         # Mirror section with shim bodies
```

**Optional 4th file:** `tests/manual/collision_smoke.iron` — minimal smoke test calling one function per receiver. Planner discretion; Phase 63 validated without new test files.

### Pattern 1: Instance method on data-carrying `object` — Iron → C symbol mangling

**What:** Iron `func Rectangle.collides(other: Rectangle) -> Bool {}` lowers to C `bool Iron_rectangle_collides(struct Iron_Rectangle self_arg, struct Iron_Rectangle other)`. The mangling is `Iron_<lowercase_type_name>_<method>` regardless of whether the receiver has fields.

**When to use:** Every method in Phase 64 where a natural receiver exists.

**Source precedent (`src/hir/hir_to_lir.c:1096-1297`):**
```c
// Iron HIR method-call lowering — obj_type->object.decl->name is the type-name source for mangling.
// Same code path for data-carrying Rectangle and for empty namespace Draw.
if (obj_type->kind == IRON_TYPE_OBJECT && obj_type->object.decl) {
    type_name = obj_type->object.decl->name;
    // ...
}
// Later: snprintf(mangled, mlen, "%s_%s", type_name, expr->method_call.method);
// Then lowercase the type portion: "Rectangle_collides" → "rectangle_collides"
// Then emit_mangle_func_name prepends "Iron_" → "Iron_rectangle_collides"
```

**Verified by existing stdlib (`src/stdlib/iron_net.c:586`):**
```c
// Data-carrying TcpSocket { val fd: Int } → func TcpSocket.read(s: TcpSocket, buf: String, timeout: Int)
Iron_Result_Int_Error Iron_tcpsocket_read(Iron_TcpSocket s, Iron_String buf, int64_t timeout) {
    /* shim body — first param `s` IS the self-receiver, passed by value */
    // ...
}
```

### Pattern 2: Struct-by-value argument via `memcpy(&rl, &iron, sizeof(<raylib_type>))`

**What:** Every Iron `<struct>` argument crosses the FFI by copying into a stack local of raylib's C type, avoiding strict-aliasing warnings under `-Wall -Wextra`. Works uniformly because Phase 60 `_Static_assert` grid proves byte-identity.

**When to use:** Every non-scalar input to every Phase 64 shim.

**Template (from `src/stdlib/iron_raylib.c:585-589`):**
```c
void Iron_draw_begin_mode_2d(struct Iron_Camera camera) {
    Camera2D rl;
    memcpy(&rl, &camera, sizeof(Camera2D));
    BeginMode2D(rl);
}
```

**Applied to `Rectangle.collides`:**
```c
bool Iron_rectangle_collides(struct Iron_Rectangle self, struct Iron_Rectangle other) {
    Rectangle a, b;
    memcpy(&a, &self,  sizeof(Rectangle));
    memcpy(&b, &other, sizeof(Rectangle));
    return CheckCollisionRecs(a, b);
}
```

### Pattern 3: Struct-by-value return via memcpy-out

**What:** Capture raylib's returned struct in a stack local, memcpy into an Iron-side struct, return. Matches Phase 61 `Iron_window_get_window_position` and Phase 63 spline evaluators.

**When to use:** `GetCollisionRec` (returns `Rectangle`) and all 5 `GetRayCollision*` (return `RayCollision`).

**Template (from `src/stdlib/iron_raylib.c:1002-1011`):**
```c
struct Iron_Vector2 Iron_draw_get_spline_point_linear(
        struct Iron_Vector2 start, struct Iron_Vector2 end, float t) {
    Vector2 s, e;
    memcpy(&s, &start, sizeof(Vector2));
    memcpy(&e, &end,   sizeof(Vector2));
    Vector2 rl = GetSplinePointLinear(s, e, t);
    struct Iron_Vector2 out;
    memcpy(&out, &rl, sizeof(Vector2));
    return out;
}
```

**Applied to `Rectangle.intersection`:**
```c
struct Iron_Rectangle Iron_rectangle_intersection(
        struct Iron_Rectangle self, struct Iron_Rectangle other) {
    Rectangle a, b;
    memcpy(&a, &self,  sizeof(Rectangle));
    memcpy(&b, &other, sizeof(Rectangle));
    Rectangle rl = GetCollisionRec(a, b);
    struct Iron_Rectangle out;
    memcpy(&out, &rl, sizeof(Rectangle));
    return out;
}
```

### Pattern 4: Tuple return lowers to packed struct-by-value return

**What:** Iron `-> (A, B)` lowers to a compiler-emitted `Iron_Tuple_A_B` struct with fields `v0: A; v1: B`. The shim **returns the struct by value** — no multiple out-pointers, no sret-style reference args in the Iron API. The compiler's `emit_ensure_tuple` logic drops the typedef; Phase 59's manual registration in `emit_c.c:6333-6361` for net tuples gives a template if the auto-generator doesn't fire.

**When to use:** `Collision.lines(...) -> (Bool, Vector2)` (1 function in Phase 64).

**Verified by existing stdlib (`src/stdlib/iron_net.h:48-66`, `iron_net.c:586-620`):**
```c
// iron_net.h tuple typedef (auto-emitted by compiler OR manually ensured in emit_c.c)
typedef struct {
    int64_t       v0;
    Iron_NetError v1;
} Iron_Tuple_Int_NetError;

// iron_net.c shim — returns the struct by value
Iron_Result_Int_Error Iron_tcpsocket_read(Iron_TcpSocket s, Iron_String buf, int64_t timeout) {
    // ... work ...
    Iron_Result_Int_Error out;
    out.v0 = bytes_read;
    out.v1 = (Iron_NetError){ .code = 0 };
    return out;
}
```

**Applied to `Collision.lines`:**
```c
// Compiler auto-emits (or stub-registers in emit_c.c if needed):
typedef struct {
    bool                 v0;
    struct Iron_Vector2  v1;
} Iron_Tuple_Bool_Vector2;

Iron_Tuple_Bool_Vector2 Iron_collision_lines(
        struct Iron_Vector2 start_a, struct Iron_Vector2 end_a,
        struct Iron_Vector2 start_b, struct Iron_Vector2 end_b) {
    Vector2 s1, e1, s2, e2, pt;
    memcpy(&s1, &start_a, sizeof(Vector2));
    memcpy(&e1, &end_a,   sizeof(Vector2));
    memcpy(&s2, &start_b, sizeof(Vector2));
    memcpy(&e2, &end_b,   sizeof(Vector2));
    bool hit = CheckCollisionLines(s1, e1, s2, e2, &pt);

    Iron_Tuple_Bool_Vector2 out;
    out.v0 = hit;
    memcpy(&out.v1, &pt, sizeof(Vector2));
    return out;
}
```

**IMPORTANT** — the CONTEXT.md proposed shim signature with `bool *out_hit, Iron_Vector2 *out_point` output pointers is **NOT** the correct pattern for Iron's tuple-return lowering. Iron returns tuples by value as a packed struct (matching iron_net.c precedent). The planner should write the shim to RETURN a struct, not take output pointers. If the compiler doesn't auto-emit the `Iron_Tuple_Bool_Vector2` typedef, the fallback is to manually register it in `emit_c.c` following the net-tuple pattern at line 6333-6361; a simpler first-move is to check in the probe task whether `ironc check raylib.iron` alone triggers auto-emission.

### Pattern 5: Array-input via `Iron_List_Iron_Vector2` by value

**What:** Iron `[Vector2]` parameter lowers to `Iron_List_Iron_Vector2 { items: Iron_Vector2*; count: int64; capacity: int64 }` passed by value. Shim forwards `(const Vector2 *)points.items` + cast count to raylib.

**When to use:** `Vector2.inside_polygon(points: [Vector2]) -> Bool` (1 function in Phase 64).

**Template (from `src/stdlib/iron_raylib.c:1081-1085`):**
```c
void Iron_draw_spline_linear(Iron_List_Iron_Vector2 points, int32_t count,
                              float thick, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawSplineLinear((const Vector2 *)points.items, (int)count, thick, c);
}
```

**Applied to `Vector2.inside_polygon` — raylib's signature takes the point first, then polygon array:**
```c
bool Iron_vector2_inside_polygon(struct Iron_Vector2 self,
                                  Iron_List_Iron_Vector2 points,
                                  int32_t point_count) {
    Vector2 p;
    memcpy(&p, &self, sizeof(Vector2));
    return CheckCollisionPointPoly(p, (const Vector2 *)points.items, (int)point_count);
}
```

**Iron stub note:** Iron's `[Vector2]` carries its own count (part of the list struct). Whether to expose `point_count` as an explicit Iron-side parameter or derive from `points.count` is a discretion call. Recommendation: follow Phase 63's `Draw.triangle_fan(points: [Vector2], count: Int32, color: Color)` precedent — keep the explicit count for ABI simplicity, document that users can pass `points.count` (Int32 cast) at the call site. This deviates slightly from raylib's implicit-count style but matches Phase 63's established Iron-surface convention.

### Pattern 6: Namespace-type registration (`object Collision {}`)

**What:** New empty `object` declaration in `raylib.iron` near existing `object Window {}` / `Draw {}` / `Audio {}` / etc. (lines 931-954). Pure static method receiver; never instantiated.

**When to use:** Once, at the start of Plan 64-01. Holds 5 2D namespace functions + 1 3D namespace function (`Collision.spheres` added in Plan 64-02).

**Template (from `raylib.iron:931-934, 950-954`):**
```iron
-- ── Phase 64 namespace type ───────────────────────────────────────────
object Collision {}
```

**Placement recommendation:** add alongside the existing namespace block at the end of raylib.iron, after `object Gestures {}`. Keep data-types (Rectangle/Vector2/Ray/BoundingBox) untouched structurally — just append `func <Type>.method()` stubs at the appropriate spot in the file. CONTEXT.md Claude's Discretion suggests grouping by receiver; precedent in the file so far groups methods under each namespace immediately after its `object` declaration (all Draw methods under `object Draw {}`). Natural receivers break that pattern because `object Rectangle` already has fields — the methods have to come later in the file than the declaration. Accepted practice: add a new section `-- Collision (Phase 64) --` near the bottom with all 19 methods grouped by receiver.

### Anti-Patterns to Avoid

- **Do not attempt to detect collision from Iron code.** Every function must go through the raylib C call — no Iron-side math.
- **Do not add a `LineIntersection` object type as the default for `CheckCollisionLines`.** Iron's tuple returns are the established v1.2.0 feature; the object fallback is only a probe-task contingency.
- **Do not hand-roll the `Iron_Tuple_Bool_Vector2` typedef without first checking the compiler's auto-emit behavior.** Phase 63-04 landed auto-emission for `Iron_List_<T>`; tuple auto-emission is already in place per `emit_ensure_tuple`. Manual registration only if that path is broken for the Bool/Vector2 pair.
- **Do not forget that `Iron_List_Iron_Vector2` typedef is already in `iron_raylib.h:589-596`.** Re-declaring it anywhere else causes double-definition errors.
- **Do not rename `bezier_quadratic` / `BoundingBox` or other Phase 60-locked names.** They're fixed by `_Static_assert`.
- **Do not add layout asserts for Phase 64 types.** The 392 existing asserts cover every Phase 64 type. Adding duplicates makes the layout TU noisier without catching new issues.
- **Do not instantiate `Collision` as a value** (e.g., `val c = Collision()` — namespace objects are never constructed).
- **Do not pass `Mesh` pointers to shims.** Iron passes it by value; the shim copies. Raylib reads mesh data through the opaque pointer fields INSIDE the struct (vertices, indices) — those pointers point to allocations made by `LoadModel` in Phase 70. Phase 64's shim doesn't manage them.
- **Do not add a `-- PHASE 64:` marker to pong.iron or other consumer files.** Collision is unused by all current consumers; Phase 64 completes without consumer-file changes.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Rectangle-rectangle intersection | Custom axis-aligned box-overlap math | `CheckCollisionRecs` → `Rectangle.collides` | raylib's implementation handles edge cases (zero-size rects, negative dimensions, coincident edges) tested across thousands of games. |
| Point-in-triangle test | Custom barycentric-coordinate math | `CheckCollisionPointTriangle` → `Vector2.inside_triangle` | raylib uses sign-of-cross-products; edge-on-line cases are consistently handled. |
| Point-in-polygon test | Custom ray-casting or winding-number algorithm | `CheckCollisionPointPoly` → `Vector2.inside_polygon` | Handles non-convex polygons, handles points exactly on edges consistently, tested for arbitrary vertex counts. |
| Ray-sphere / ray-box / ray-triangle / ray-quad intersection | Custom quadratic-equation / slab-test / Möller-Trumbore | `GetRayCollisionSphere/Box/Triangle/Quad` → `Ray.hit_*` | Numerical-stability edge cases (parallel rays, tangent rays, degenerate triangles) are raylib's problem. |
| Ray-mesh intersection | Iterate all triangles by hand | `GetRayCollisionMesh` → `Ray.hit_mesh` | Walks the mesh's vertex/index arrays through raylib's internal vertex-format handling. Phase 64 gets this free. |
| Line-line intersection point | Custom parametric-equation solver | `CheckCollisionLines` → `Collision.lines` | Handles parallel lines, coincident lines, endpoint-on-line cases. |
| Spatial acceleration (BVH, quadtree) | Custom partitioning structure | **Not raylib's scope — OUT of Phase 64** | Every call is a direct raylib C call. CONTEXT.md explicitly defers this. |
| Layer/mask filtering | Iron-side bitmap | **Not raylib's scope — OUT of Phase 64** | Game-engine abstraction; users implement per-game. |

**Key insight:** raylib's collision functions are *pure, stateless, and well-specified*. Every function takes its operands by value and returns a result — no GPU state, no thread-state, no global mutation. Phase 64's shim is a thin byte-copying forwarder. This is the lowest-risk phase of v2.0.0-alpha so far.

## Common Pitfalls

### Pitfall 1: Forgetting the `Int32` (not `Int`) type on `CheckCollisionPointLine` threshold
**What goes wrong:** Iron's default `Int` is int64_t; raylib's `int threshold` is 32-bit. Declaring `Vector2.on_line(..., threshold: Int)` in Iron lowers to a 64-bit param, but the shim casts to `int` and this works silently. However, `_Wall -Wextra -Wconversion` flags the silent narrowing.
**Why it happens:** Iron's Int is pre-1.2.0 idiomatic default; Phases 62-63 already standardized on `Int32` for raylib int params.
**How to avoid:** Declare `threshold: Int32`; shim casts to `(int)threshold` inline.
**Warning signs:** Any `clang -c` warning mentioning `-Wconversion` in the threshold param; pong.iron smoke test ordinal-comparison mismatches.

### Pitfall 2: Assuming Iron's tuple-return auto-emits `Iron_Tuple_Bool_Vector2` with that exact name
**What goes wrong:** The compiler's tuple-name-generation may use a different mangling for compound element names than expected. Iron_Tuple_Bool_Vector2 vs Iron_Tuple_bool_Iron_Vector2 vs Iron_Tuple_Bool_IronVector2 — all plausible without source verification.
**Why it happens:** Iron's tuple-typedef emitter (`emit_ensure_tuple` in `src/lir/emit_c.c`) is driven by a naming convention the planner hasn't inspected end-to-end for Bool + Object pairs.
**How to avoid:** Plan 64-01 Task 1 (probe) should:
  1. Declare `func Collision.lines(...) -> (Bool, Vector2) {}` in raylib.iron.
  2. Run `ironc check src/stdlib/raylib.iron` or run the full pong build to force emit.
  3. Inspect the generated C (in a tmp file) for the emitted tuple typedef name.
  4. Write the shim with that exact name.
**Warning signs:** "unknown type name 'Iron_Tuple_*'" clang error at the shim site — means the typedef isn't being emitted with the expected name.
**Fallback if auto-emission fails:** Register the tuple manually in `src/lir/emit_c.c` mirroring the `if (need_tcp)` block at line 6333-6361. Scope: ~15 lines. Compiler-wide-beneficial (future phases with `(Bool, <StructType>)` returns would benefit).

### Pitfall 3: Mesh pass-by-value might trigger large-struct ABI paths the planner hasn't seen before
**What goes wrong:** Mesh is 120 bytes. On ARM64 (AAPCS64), structs > 16 bytes are passed by pointer to a caller-allocated copy; the indirect pass is transparent at the C level but the generated LIR might not match what the shim declares.
**Why it happens:** Iron's own pass-by-value lowering assumes C compiler handles the calling convention. This is TRUE — both Iron's emitted C and raylib's vendored C are compiled by the same clang/emcc, so they agree on the ABI. But a probe is warranted because the Phase 63 max was 44 bytes (RenderTexture2D); Mesh nearly triples that.
**How to avoid:** Plan 64-02 Task 1 should include a 1-function Mesh probe (e.g., `Ray.hit_mesh` with a zero-initialized Iron Mesh literal). Run the full pong-build pipeline; if it compiles and the static asserts hold, Mesh pass-by-value works.
**Warning signs:** `_Static_assert(sizeof(struct Iron_Mesh) == sizeof(Mesh))` firing (layout drift) or clang warnings about "passing struct through ... may not match target ABI." Neither is expected given Phase 60-04 already locked Mesh layout with 18 asserts.

### Pitfall 4: `CheckCollisionPointPoly` point-count argument is `int`, not implicit from array
**What goes wrong:** raylib's signature is `bool CheckCollisionPointPoly(Vector2 point, const Vector2 *points, int pointCount)`. Iron's `[Vector2]` carries count internally (`points.count`). Users might expect `Vector2.inside_polygon(points)` to pass `points.count` automatically, but Iron has no implicit-cast path.
**Why it happens:** Phase 63 established the explicit-count convention (`Draw.triangle_fan(points, count, color)`), so users who've seen Phase 63 will expect the same pattern. But if the API surface design chooses to hide `count` (pulling from `points.count` inside the shim), there's an ergonomic win.
**How to avoid:** The shim has access to `points.count`, so it can forward `(int)points.count` without requiring an explicit Iron-side `count` param. Recommend this variant: `Vector2.inside_polygon(points: [Vector2]) -> Bool`. Shim body calls `CheckCollisionPointPoly(p, (const Vector2 *)points.items, (int)points.count)`. Matches raymath/math namespace idioms.
**Warning signs:** Call sites doing `vec.inside_polygon(pts, pts.count)` are verbose; dropping the explicit count makes the API cleaner. Phase 63 used explicit count for uniformity with non-[Vector2] array params, but collision has only one such function — not enough to justify the convention's carrying cost.

### Pitfall 5: Bool field in `RayCollision` crosses FFI — already validated in Phase 60-05 but worth re-noting
**What goes wrong:** Iron's `Bool` maps to C `bool` (1 byte) per the Phase 60-05 finding (hit's 3-byte padding before `distance` at offset 4). Any future Iron-compiler change to `Bool` lowering would break RayCollision return-by-value.
**Why it happens:** Iron didn't always guarantee 1-byte bool; Phase 60-05 was the first phase to put Bool in a struct field.
**How to avoid:** Nothing — Plan 60-05's `_Static_assert(offsetof(RayCollision, distance) == 4)` already guards against drift. Phase 64 does NOT need to re-verify.
**Warning signs:** `_Static_assert(offsetof(RayCollision, distance) == 4, "...")` firing at build time would catch any future drift.

### Pitfall 6: `bezier_quadratic` vs raylib's `BezierQuad` naming drift in `Collision` namespace
**What goes wrong:** N/A for Phase 64 — the naming drift was Phase 63-specific (spline evaluators). No Phase 64 function has a long-vs-short-name split. Noted here only to prevent confusion.

### Pitfall 7: `RayCollision` 32-byte size exceeds SysV 16-byte register-return threshold
**What goes wrong:** On x86-64 Linux (SysV AMD64), structs > 16 bytes are returned via hidden sret pointer. On ARM64, the Xn indirect-result register x8 holds the address. Iron's generated C and raylib's compiled C agree on this automatically because both go through the same compiler — no manual sret handling needed in the shim.
**Why it happens:** N/A — this is a calling-convention detail the compiler handles. Mentioning for completeness.
**How to avoid:** Nothing — the C compiler handles this.
**Warning signs:** `-Wabi` warnings during clang compile. None expected.

## Code Examples

Verified patterns from source precedent. Planner can paste these directly into the Phase 64 plan.

### §1 — `Rectangle.collides(other: Rectangle) -> Bool` (CheckCollisionRecs)

```iron
-- raylib.iron
func Rectangle.collides(self: Rectangle, other: Rectangle) -> Bool {}
```

```c
/* iron_raylib.h — under /* ── Collision (Phase 64) ─────────── */ */
bool Iron_rectangle_collides(struct Iron_Rectangle self, struct Iron_Rectangle other);

/* iron_raylib.c */
bool Iron_rectangle_collides(struct Iron_Rectangle self, struct Iron_Rectangle other) {
    Rectangle a, b;
    memcpy(&a, &self,  sizeof(Rectangle));
    memcpy(&b, &other, sizeof(Rectangle));
    return CheckCollisionRecs(a, b);
}
```

### §2 — `Rectangle.intersection(other: Rectangle) -> Rectangle` (GetCollisionRec)

```iron
func Rectangle.intersection(self: Rectangle, other: Rectangle) -> Rectangle {}
```

```c
struct Iron_Rectangle Iron_rectangle_intersection(
        struct Iron_Rectangle self, struct Iron_Rectangle other);

struct Iron_Rectangle Iron_rectangle_intersection(
        struct Iron_Rectangle self, struct Iron_Rectangle other) {
    Rectangle a, b;
    memcpy(&a, &self,  sizeof(Rectangle));
    memcpy(&b, &other, sizeof(Rectangle));
    Rectangle rl = GetCollisionRec(a, b);
    struct Iron_Rectangle out;
    memcpy(&out, &rl, sizeof(Rectangle));
    return out;
}
```

### §3 — `Rectangle.contains_point(point: Vector2) -> Bool` (CheckCollisionPointRec)

```iron
func Rectangle.contains_point(self: Rectangle, point: Vector2) -> Bool {}
```

```c
bool Iron_rectangle_contains_point(struct Iron_Rectangle self, struct Iron_Vector2 point) {
    Rectangle r;
    Vector2 p;
    memcpy(&r, &self,  sizeof(Rectangle));
    memcpy(&p, &point, sizeof(Vector2));
    return CheckCollisionPointRec(p, r);
}
```

### §4 — `Rectangle.collides_circle(center: Vector2, radius: Float32) -> Bool` (CheckCollisionCircleRec)

```iron
func Rectangle.collides_circle(self: Rectangle, center: Vector2, radius: Float32) -> Bool {}
```

```c
bool Iron_rectangle_collides_circle(struct Iron_Rectangle self,
                                      struct Iron_Vector2 center, float radius) {
    Rectangle r;
    Vector2 c;
    memcpy(&r, &self,   sizeof(Rectangle));
    memcpy(&c, &center, sizeof(Vector2));
    return CheckCollisionCircleRec(c, radius, r);
}
```

### §5 — `Vector2.inside_triangle(p1, p2, p3) -> Bool` (CheckCollisionPointTriangle)

```iron
func Vector2.inside_triangle(self: Vector2, p1: Vector2, p2: Vector2, p3: Vector2) -> Bool {}
```

```c
bool Iron_vector2_inside_triangle(struct Iron_Vector2 self,
                                    struct Iron_Vector2 p1,
                                    struct Iron_Vector2 p2,
                                    struct Iron_Vector2 p3) {
    Vector2 p, a, b, c;
    memcpy(&p, &self, sizeof(Vector2));
    memcpy(&a, &p1,   sizeof(Vector2));
    memcpy(&b, &p2,   sizeof(Vector2));
    memcpy(&c, &p3,   sizeof(Vector2));
    return CheckCollisionPointTriangle(p, a, b, c);
}
```

### §6 — `Vector2.inside_polygon(points: [Vector2]) -> Bool` (CheckCollisionPointPoly)

```iron
func Vector2.inside_polygon(self: Vector2, points: [Vector2]) -> Bool {}
```

```c
bool Iron_vector2_inside_polygon(struct Iron_Vector2 self,
                                   Iron_List_Iron_Vector2 points) {
    Vector2 p;
    memcpy(&p, &self, sizeof(Vector2));
    return CheckCollisionPointPoly(p, (const Vector2 *)points.items, (int)points.count);
}
```

**Note:** dropping the explicit `count` parameter is a deviation from Phase 63's `Draw.triangle_fan(pts, count, color)` convention. Recommended because (a) only one Phase 64 function has this shape, (b) `points.count` is reliably set by Iron's list lowering, (c) cleaner Iron API. Flag for Phase 73 cross-cutting review.

### §7 — `Vector2.on_line(p1, p2, threshold: Int32) -> Bool` (CheckCollisionPointLine)

```iron
func Vector2.on_line(self: Vector2, p1: Vector2, p2: Vector2, threshold: Int32) -> Bool {}
```

```c
bool Iron_vector2_on_line(struct Iron_Vector2 self,
                            struct Iron_Vector2 p1,
                            struct Iron_Vector2 p2,
                            int32_t threshold) {
    Vector2 p, a, b;
    memcpy(&p, &self, sizeof(Vector2));
    memcpy(&a, &p1,   sizeof(Vector2));
    memcpy(&b, &p2,   sizeof(Vector2));
    return CheckCollisionPointLine(p, a, b, (int)threshold);
}
```

### §8 — `Collision.circles(c1, r1, c2, r2) -> Bool` (CheckCollisionCircles)

```iron
func Collision.circles(c1: Vector2, r1: Float32, c2: Vector2, r2: Float32) -> Bool {}
```

```c
bool Iron_collision_circles(struct Iron_Vector2 c1, float r1,
                              struct Iron_Vector2 c2, float r2) {
    Vector2 a, b;
    memcpy(&a, &c1, sizeof(Vector2));
    memcpy(&b, &c2, sizeof(Vector2));
    return CheckCollisionCircles(a, r1, b, r2);
}
```

### §9 — `Collision.circle_line(center, radius, p1, p2) -> Bool` (CheckCollisionCircleLine)

```iron
func Collision.circle_line(center: Vector2, radius: Float32, p1: Vector2, p2: Vector2) -> Bool {}
```

```c
bool Iron_collision_circle_line(struct Iron_Vector2 center, float radius,
                                  struct Iron_Vector2 p1, struct Iron_Vector2 p2) {
    Vector2 c, a, b;
    memcpy(&c, &center, sizeof(Vector2));
    memcpy(&a, &p1,     sizeof(Vector2));
    memcpy(&b, &p2,     sizeof(Vector2));
    return CheckCollisionCircleLine(c, radius, a, b);
}
```

### §10 — `Collision.point_circle(point, center, radius) -> Bool` (CheckCollisionPointCircle)

```iron
func Collision.point_circle(point: Vector2, center: Vector2, radius: Float32) -> Bool {}
```

```c
bool Iron_collision_point_circle(struct Iron_Vector2 point,
                                   struct Iron_Vector2 center, float radius) {
    Vector2 p, c;
    memcpy(&p, &point,  sizeof(Vector2));
    memcpy(&c, &center, sizeof(Vector2));
    return CheckCollisionPointCircle(p, c, radius);
}
```

### §11 — `Collision.lines(start_a, end_a, start_b, end_b) -> (Bool, Vector2)` (CheckCollisionLines) — TUPLE RETURN

```iron
func Collision.lines(start_a: Vector2, end_a: Vector2, start_b: Vector2, end_b: Vector2) -> (Bool, Vector2) {}
```

```c
/* Compiler auto-emits (or manually register in emit_c.c per net-tuple pattern):
 *   typedef struct { bool v0; struct Iron_Vector2 v1; } Iron_Tuple_Bool_Iron_Vector2;
 * PROBE FIRST — observe exact name ironc emits before writing this shim. */
Iron_Tuple_Bool_Iron_Vector2 Iron_collision_lines(
        struct Iron_Vector2 start_a, struct Iron_Vector2 end_a,
        struct Iron_Vector2 start_b, struct Iron_Vector2 end_b) {
    Vector2 s1, e1, s2, e2, pt;
    memcpy(&s1, &start_a, sizeof(Vector2));
    memcpy(&e1, &end_a,   sizeof(Vector2));
    memcpy(&s2, &start_b, sizeof(Vector2));
    memcpy(&e2, &end_b,   sizeof(Vector2));
    bool hit = CheckCollisionLines(s1, e1, s2, e2, &pt);

    Iron_Tuple_Bool_Iron_Vector2 out;
    out.v0 = hit;
    memcpy(&out.v1, &pt, sizeof(Vector2));
    return out;
}
```

**PROBE TASK:** Before writing this shim, declare the Iron stub empty-body, run `ironc check src/stdlib/raylib.iron` OR the full pong build, inspect `/tmp/iron_*.c` to capture the exact tuple typedef name. Fallback if auto-emission fails: manually register via `emit_c.c` (see Pitfall 2 for the pattern).

### §12 — `BoundingBox.collides(other: BoundingBox) -> Bool` (CheckCollisionBoxes)

```iron
func BoundingBox.collides(self: BoundingBox, other: BoundingBox) -> Bool {}
```

```c
bool Iron_boundingbox_collides(struct Iron_BoundingBox self,
                                 struct Iron_BoundingBox other) {
    BoundingBox a, b;
    memcpy(&a, &self,  sizeof(BoundingBox));
    memcpy(&b, &other, sizeof(BoundingBox));
    return CheckCollisionBoxes(a, b);
}
```

### §13 — `BoundingBox.collides_sphere(center, radius) -> Bool` (CheckCollisionBoxSphere)

```iron
func BoundingBox.collides_sphere(self: BoundingBox, center: Vector3, radius: Float32) -> Bool {}
```

```c
bool Iron_boundingbox_collides_sphere(struct Iron_BoundingBox self,
                                        struct Iron_Vector3 center, float radius) {
    BoundingBox b;
    Vector3 c;
    memcpy(&b, &self,   sizeof(BoundingBox));
    memcpy(&c, &center, sizeof(Vector3));
    return CheckCollisionBoxSphere(b, c, radius);
}
```

### §14 — `Ray.hit_sphere(center, radius) -> RayCollision` (GetRayCollisionSphere)

```iron
func Ray.hit_sphere(self: Ray, center: Vector3, radius: Float32) -> RayCollision {}
```

```c
struct Iron_RayCollision Iron_ray_hit_sphere(struct Iron_Ray self,
                                                struct Iron_Vector3 center, float radius) {
    Ray r;
    Vector3 c;
    memcpy(&r, &self,   sizeof(Ray));
    memcpy(&c, &center, sizeof(Vector3));
    RayCollision rl = GetRayCollisionSphere(r, c, radius);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));  /* 32 bytes */
    return out;
}
```

### §15 — `Ray.hit_box(box) -> RayCollision` (GetRayCollisionBox)

```iron
func Ray.hit_box(self: Ray, box: BoundingBox) -> RayCollision {}
```

```c
struct Iron_RayCollision Iron_ray_hit_box(struct Iron_Ray self,
                                            struct Iron_BoundingBox box) {
    Ray r;
    BoundingBox bb;
    memcpy(&r,  &self, sizeof(Ray));
    memcpy(&bb, &box,  sizeof(BoundingBox));
    RayCollision rl = GetRayCollisionBox(r, bb);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}
```

### §16 — `Ray.hit_mesh(mesh, transform) -> RayCollision` (GetRayCollisionMesh) — **FIRST MESH/MATRIX PASS-BY-VALUE**

```iron
func Ray.hit_mesh(self: Ray, mesh: Mesh, transform: Matrix) -> RayCollision {}
```

```c
struct Iron_RayCollision Iron_ray_hit_mesh(struct Iron_Ray self,
                                              struct Iron_Mesh mesh,
                                              struct Iron_Matrix transform) {
    Ray r;
    Mesh m;
    Matrix tx;
    memcpy(&r,  &self,      sizeof(Ray));
    memcpy(&m,  &mesh,      sizeof(Mesh));      /* 120 bytes */
    memcpy(&tx, &transform, sizeof(Matrix));    /* 64 bytes */
    RayCollision rl = GetRayCollisionMesh(r, m, tx);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}
```

**Critical note:** Mesh's 12 opaque pointer fields (`_vertices`, `_indices`, etc.) are NOT owned by Phase 64 — they point to allocations made by `LoadModel`/`LoadModelFromMesh` in Phase 70. Phase 64 just copies the pointers through the struct. Raylib reads the pointed-to vertex/index arrays during the hit test. No ownership transfer, no double-free risk.

### §17 — `Ray.hit_triangle(p1, p2, p3) -> RayCollision` (GetRayCollisionTriangle)

```iron
func Ray.hit_triangle(self: Ray, p1: Vector3, p2: Vector3, p3: Vector3) -> RayCollision {}
```

```c
struct Iron_RayCollision Iron_ray_hit_triangle(struct Iron_Ray self,
                                                  struct Iron_Vector3 p1,
                                                  struct Iron_Vector3 p2,
                                                  struct Iron_Vector3 p3) {
    Ray r;
    Vector3 a, b, c;
    memcpy(&r, &self, sizeof(Ray));
    memcpy(&a, &p1,   sizeof(Vector3));
    memcpy(&b, &p2,   sizeof(Vector3));
    memcpy(&c, &p3,   sizeof(Vector3));
    RayCollision rl = GetRayCollisionTriangle(r, a, b, c);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}
```

### §18 — `Ray.hit_quad(p1, p2, p3, p4) -> RayCollision` (GetRayCollisionQuad)

```iron
func Ray.hit_quad(self: Ray, p1: Vector3, p2: Vector3, p3: Vector3, p4: Vector3) -> RayCollision {}
```

```c
struct Iron_RayCollision Iron_ray_hit_quad(struct Iron_Ray self,
                                              struct Iron_Vector3 p1,
                                              struct Iron_Vector3 p2,
                                              struct Iron_Vector3 p3,
                                              struct Iron_Vector3 p4) {
    Ray r;
    Vector3 a, b, c, d;
    memcpy(&r, &self, sizeof(Ray));
    memcpy(&a, &p1,   sizeof(Vector3));
    memcpy(&b, &p2,   sizeof(Vector3));
    memcpy(&c, &p3,   sizeof(Vector3));
    memcpy(&d, &p4,   sizeof(Vector3));
    RayCollision rl = GetRayCollisionQuad(r, a, b, c, d);
    struct Iron_RayCollision out;
    memcpy(&out, &rl, sizeof(RayCollision));
    return out;
}
```

### §19 — `Collision.spheres(c1, r1, c2, r2) -> Bool` (CheckCollisionSpheres)

```iron
func Collision.spheres(c1: Vector3, r1: Float32, c2: Vector3, r2: Float32) -> Bool {}
```

```c
bool Iron_collision_spheres(struct Iron_Vector3 c1, float r1,
                              struct Iron_Vector3 c2, float r2) {
    Vector3 a, b;
    memcpy(&a, &c1, sizeof(Vector3));
    memcpy(&b, &c2, sizeof(Vector3));
    return CheckCollisionSpheres(a, r1, b, r2);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Bare `extern func CheckCollisionRecs(...)` in raylib.iron (pre–v2.0.0) | Shim-only foreign-method stubs (`func Rectangle.collides(self, other) {}` → `Iron_rectangle_collides`) | Phase 60-01, 2026-04-14 | Every raylib call now routes through `iron_raylib.c`; enables instance methods, tuple returns, out-param translation without Iron-compiler changes. |
| Manual `Iron_List_<T>` guard in each stdlib header (e.g., `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED`) | `emit_structs.c:243-312` auto-emits typedefs by scanning `extern_decls` AND `foreign-method-stub funcs` | Phase 63-04, 2026-04-16 | `Vector2.inside_polygon(points: [Vector2])` auto-emits the typedef. Existing `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` guard at `iron_raylib.h:589` remains as belt-and-suspenders but is redundant. |
| Layout assertions in separate test TU | `_Static_assert` grid in `iron_raylib_layout.c` — 392 entries | Phase 60-02 through 60-07 | Phase 64 gets zero-cost ABI protection for Rectangle/Vector2/Vector3/Ray/RayCollision/BoundingBox/Mesh/Matrix — all pinned. |
| Per-call `extern func` declarations in raylib.iron | `func <Receiver>.<method>(...)` empty-body stubs + shim C | Phase 60-01 | Uniform method-call surface for users. Every raylib function is a method on its natural receiver or a namespace (`Draw`, `Window`, `Keyboard`, `Collision`). |
| Vector2 RETURN only from Phase 61 window surface | Vector2 / Rectangle / RayCollision returns via memcpy-out | Phase 61 → 63 → 64 (progressive) | Phase 64 extends the memcpy-out pattern to Rectangle return (first time for this type) and RayCollision return (first time for 32-byte struct return). |

**Deprecated/outdated:**
- `Vec2`, `Color.rgba(...)` constructor, `Key` enum, extern-based `InitWindow` etc. — all REMOVED by Phase 60-08 clean break. The v1.2.0 raylib.iron surface is gone.
- Any pre-Phase-60 tutorial content showing `extern func CheckCollisionRecs(...)` style — obsolete.

## Open Questions

1. **Will the compiler auto-emit `Iron_Tuple_Bool_Iron_Vector2` (or whatever its canonical name is) for the `Collision.lines` return type?**
   - **What we know:** Net's `Iron_Tuple_Int_NetError`, `Iron_Tuple_TcpSocket_NetError`, `Iron_Tuple_TcpListener_NetError` are auto-emitted by `emit_ensure_tuple` as needed, with a fallback manual-registration block in `emit_c.c:6333-6361` for the three well-known net types.
   - **What's unclear:** Whether the auto-emitter has been exercised for `(Bool, <Object>)` shapes before. All three net tuples have `NetError` as `v1` and `Int` or a data-carrying struct as `v0`. `(Bool, Vector2)` has different element kinds — Bool is a primitive, Vector2 is an Iron_Object. Naming convention and struct-body generation may differ from the net precedent.
   - **Recommendation:** Plan 64-01 Task 1 is a probe — declare the Iron stub, run `ironc check` on a minimal caller, inspect the emitted C for the tuple typedef. If it's auto-emitted, use its exact name. If not, manually register in `emit_c.c` following the net-tuple pattern (expected ~15 lines of code).

2. **Should `Vector2.inside_polygon(points: [Vector2])` take an explicit `count: Int32` argument for consistency with Phase 63's `Draw.triangle_fan` pattern?**
   - **What we know:** `points.count` is reliably populated by Iron's list lowering (the `Iron_List_Iron_Vector2` struct carries it).
   - **What's unclear:** Whether Phase 73 API polish would standardize on the implicit-count form or the explicit-count form. Phase 63 used explicit; but Phase 63 also had many non-`[Vector2]` array shapes where the count was genuinely separate (DrawLineStrip, DrawSplineLinear). Collision has only ONE array function.
   - **Recommendation:** Drop the explicit count. Flag in Plan 64-01 commit message so Phase 73 cross-cutting review has the explicit record.

3. **Should `tests/manual/collision_smoke.iron` be added?**
   - **What we know:** Phase 63 did not add a smoke test file; pong.iron served as validation. Pong does NOT use collision.
   - **What's unclear:** Whether `clang -c iron_raylib.c` + `ironc check raylib.iron` round-trip is sufficient, or whether a runtime smoke test (run one function per receiver) adds value.
   - **Recommendation:** Add a minimal `collision_smoke.iron` in Plan 64-02 final wave. 20-30 lines, one function per receiver (Rectangle.collides + Vector2.inside_triangle + Collision.lines + Collision.spheres + Ray.hit_sphere) with `println` assertions. Validates the full ironc pipeline including the tuple return. Marginal memory cost (1 extra ironc invocation).

4. **Does the compiler's `mangle_func_name` correctly handle `boundingbox` (concatenated) vs `bounding_box` (underscore-separated)?**
   - **What we know:** The lowercase-until-underscore logic (`hir_to_lir.c:1299-1302`) converts `BoundingBox_collides` to `boundingbox_collides`. The existing stdlib has no precedent for a type name with a capital-inside-the-identifier.
   - **What's unclear:** None — the source code clearly converts `A-Z → a-z` char-by-char until the first underscore, so `BoundingBox` → `boundingbox`. Confirmed.
   - **Recommendation:** Shim symbol is `Iron_boundingbox_collides`. No discussion needed.

## Sources

### Primary (HIGH confidence)
- **`src/vendor/raylib/raylib.h` lines 1304-1315** — 2D collision C signatures (11 functions). Authoritative.
- **`src/vendor/raylib/raylib.h` lines 1611-1619** — 3D collision C signatures (8 functions). Authoritative.
- **`src/vendor/raylib/raylib.h` lines 280-305** — Ray / RayCollision / BoundingBox struct definitions. Authoritative.
- **`src/vendor/raylib/raylib.h` lines 345-369** — Mesh struct definition (14 fields, 120 bytes on ARM64). Authoritative.
- **`src/stdlib/raylib.iron` lines 1-954** — Iron stdlib current state (35 objects, 65 Draw.* methods). Authoritative.
- **`src/stdlib/iron_raylib.h` lines 585-636** — `Iron_List_Iron_Vector2` typedef + guard + Phase 63 array-input shim prototypes. Authoritative.
- **`src/stdlib/iron_raylib.c` lines 585-589, 1002-1011, 1081-1085** — Camera2D/Vector2 memcpy patterns and DrawSplineLinear array-by-value pattern. Authoritative.
- **`src/stdlib/iron_net.c` lines 586-630** — TcpSocket.read(s: TcpSocket, ...) lowering to `Iron_tcpsocket_read(Iron_TcpSocket s, ...)` — THE PRECEDENT for instance methods on data-carrying objects. Authoritative.
- **`src/stdlib/iron_net.h` lines 40-100** — Packed tuple struct typedefs (`Iron_Tuple_Int_NetError` etc.) and shim prototypes — THE PRECEDENT for tuple-return-by-packed-struct. Authoritative.
- **`src/hir/hir_to_lir.c` lines 1090-1302** — Method-call lowering to `Iron_<lowercased_type>_<method>` mangled name via `snprintf + lowercase`. Authoritative.
- **`src/lir/emit_helpers.c` lines 52-75** — `emit_mangle_func_name` adds `Iron_` prefix. Authoritative.
- **`src/lir/emit_c.c` lines 6317-6361** — Manual tuple registration pattern (fallback if auto-emit fails). Authoritative.
- **`src/lir/emit_structs.c` lines 243-312** — Phase 63-04 auto-emit of `Iron_List_<T>` typedefs for foreign-method-stub params. Authoritative.
- **`.planning/phases/60-type-enum-foundation/60-CONTEXT.md`** — Locked architectural decisions (shim-only, snake_case, layout verification). Authoritative.
- **`.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-CONTEXT.md`** — Namespace `object` pattern (Gestures-plural to avoid enum Gesture collision); typed-enum casting precedent. Authoritative.
- **`.planning/phases/63-2d-drawing/63-04-SUMMARY.md`** — Iron_List_Iron_Vector2 array-by-value ABI (outcome A, ARRAY_PARAM_LIST); Vector2-return memcpy-out template; compiler-wide Rule 3 auto-emit fix. Authoritative.
- **`.planning/STATE.md`** — Current project state; Phase 63 COMPLETE; Phase 64 ready. Authoritative.
- **`.planning/REQUIREMENTS.md` lines 125-128** — COLL-01 and COLL-02 descriptions. Authoritative.
- **Direct measurement via `clang -I. -Isrc -Isrc/vendor/raylib sizeof_probe.c`** — sizeof(RayCollision)=32, sizeof(Mesh)=120, sizeof(Matrix)=64, sizeof(Ray)=24, sizeof(BoundingBox)=24, sizeof(Rectangle)=16, sizeof(Vector2)=8, sizeof(Vector3)=12. ABI ground truth.

### Secondary (MEDIUM confidence)
- **AAPCS64 procedure call standard** (ARM-software/abi-aa repository) — struct > 16 bytes passed by hidden caller-allocated pointer; struct return > 16 bytes uses x8 indirect-result register. Standardized; universally followed by clang/gcc on ARM64. [ARM-software/abi-aa](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
- **System V AMD64 ABI** (x86-64 SysV) — struct > 16 bytes returned via hidden `sret` first-argument pointer; struct > 16 bytes as argument passed on stack or via pointer depending on fields. Universally followed by clang/gcc on Linux/macOS x86-64. [X86 calling conventions - Wikipedia](https://en.wikipedia.org/wiki/X86_calling_conventions)

### Tertiary (LOW confidence)
- None. Every design decision traces to source-level precedent in the repo or to the published ABI standards.

## Metadata

**Confidence breakdown:**
- **Standard stack:** HIGH — Every library/pattern pre-validated by Phase 60-63 execution. Zero new dependencies.
- **Architecture:** HIGH — All 6 patterns (instance-method dispatch, memcpy arg, memcpy-out return, tuple-return, array-by-value, namespace-type) have direct source precedent in iron_net/iron_raylib. The CONTEXT.md "unverified mechanisms" flags are over-cautious — source investigation shows the mechanisms are already in use.
- **Pitfalls:** HIGH for known pitfalls (Phase 60-05's Bool ABI, Phase 63's Int32 convention, Float32 literal ergonomics — all explicitly documented in prior SUMMARY.md files). MEDIUM for ABI edge cases on large structs (Mesh 120B, Matrix 64B pass-by-value) — compiler handles this automatically but Phase 64 is the first phase to exercise the large-struct path.

**Research date:** 2026-04-16
**Valid until:** Roughly 60 days (until raylib 5.6 / Iron v1.3.0 updates). Stable domain — collision math is well-specified and vendored raylib 5.5 is pinned.

---
*Phase 64 research complete. Planner can proceed with Plan 64-01 (2D collision, 11 functions) and Plan 64-02 (3D collision, 8 functions) using the patterns above.*
