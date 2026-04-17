# Phase 64: Collision (2D + 3D) - Context

**Gathered:** 2026-04-17
**Status:** Ready for planning
**Source:** Smart discuss (autonomous)

<domain>
## Phase Boundary

Bind raylib's 19 collision-detection functions (11 2D + 8 3D) as idiomatic Iron methods on the natural receiver types (`Rectangle`, `Vector2`, `Ray`, `BoundingBox`), with a shared `object Collision {}` namespace for pair-based tests that have no single natural receiver (circle-circle, sphere-sphere). Closes requirements **COLL-01** and **COLL-02**.

**In scope:**
- 2D (`rshapes.c`): `CheckCollisionRecs`, `CheckCollisionCircles`, `CheckCollisionCircleRec`, `CheckCollisionCircleLine`, `CheckCollisionPointRec`, `CheckCollisionPointCircle`, `CheckCollisionPointTriangle`, `CheckCollisionPointLine`, `CheckCollisionPointPoly`, `CheckCollisionLines`, `GetCollisionRec`.
- 3D (`rmodels.c`): `CheckCollisionSpheres`, `CheckCollisionBoxes`, `CheckCollisionBoxSphere`, `GetRayCollisionSphere`, `GetRayCollisionBox`, `GetRayCollisionMesh`, `GetRayCollisionTriangle`, `GetRayCollisionQuad`.

**Out of scope:**
- Any new raylib type/enum definitions — every required type exists from Phase 60 (`Rectangle`, `Vector2`, `Vector3`, `Ray`, `RayCollision`, `BoundingBox`, `Mesh`, `Matrix`).
- New draw primitives (Phase 63 closed), textures/materials (Phase 66+), physics integration (raylib has none; users attach their own).
- Optimized spatial structures (BVH, quadtree). Every call is a direct raylib C call — no Iron-side caching.

</domain>

<decisions>
## Implementation Decisions

### Inherited from Phase 60/61/62/63 (locked — no discussion)

- **Binding architecture:** shim-only. Every raylib call goes through `src/stdlib/iron_raylib.c` via empty-body foreign-method stubs. Iron `func Rectangle.collides(other: Rectangle) -> Bool {}` lowers to `Iron_rectangle_collides(...)` in C, which forwards to raylib's `CheckCollisionRecs(a, b)`.
- **Method casing:** `snake_case`. ROADMAP.md success criteria text uses `rectA.collides`, `ray.hitSphere` in camelCase — treat as narrative prose, not API spec. Implementation follows the project-wide Phase 62 precedent: `rect_a.collides(rect_b)`, `ray.hit_sphere(center, radius)`, `point.inside_rect(rect)`, `point.inside_polygon(pts)`, etc.
- **Struct-by-value argument passing:** validated across Phases 61–63. `Rectangle` (16 B), `Vector2` (8 B), `Vector3` (12 B), `Ray` (24 B), `BoundingBox` (24 B), `Mesh` (large — 14+ opaque pointer fields + 2 ints), `Matrix` (64 B). All use `memcpy(&rl, &iron, sizeof(...))` inside each shim per Phase 63's template.
- **Struct-by-value return:** validated in Phase 61 (Vector2 returns) and Phase 63 (spline evaluator Vector2 returns via memcpy-out). Phase 64 extends the pattern to `Rectangle` (GetCollisionRec) and `RayCollision` (5 `GetRayCollision*` functions).
- **Array input:** reuse Phase 63's `Iron_List_Iron_Vector2` by-value ABI for `CheckCollisionPointPoly(points: [Vector2])`. The `IRON_LIST_IRON_VECTOR2_STRUCT_DEFINED` guard in `iron_raylib.h` is already present. `emit_structs.c` (fix landed Phase 63-04) auto-emits the typedef for stdlib foreign-method-stubs with `[Vector2]` params.
- **Float32 vs Float:** raylib uses `float` everywhere for radii, coordinates, thresholds. All such params are Iron `Float32`.
- **Enum parameters:** none. Every Phase 64 function takes pure math/struct args — no typed enum params. No enum cast work.
- **Layout verification:** no new types. The 392 existing `_Static_assert` entries in `iron_raylib_layout.c` already pin every Phase 64 input/output struct layout. No additions needed.
- **Section marker:** `/* ── Collision (Phase 64) ─────────── */` in `iron_raylib.c` and `iron_raylib.h`. Follow Phase 62/63 section-marker scaffolding.
- **Auto-generated prototypes** (commit e3e5eee, 2026-04-14): `emit_c` auto-generates stdlib foreign-method prototypes. Planner does NOT hand-maintain prototype sync.
- **Clangd false-positive workaround:** `iron_raylib.c` is not in CMake `iron_stdlib` target. Verify with `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra`, not clangd.
- **Memory discipline:** ironc ~10 GB per invocation. Canonical build = `./build/ironc build examples/pong/pong.iron`. Phase 64 does NOT add new consumer-file markers (pong/game_raylib/hello_raylib don't need collision), so canonical verification = plain `clang -c` round-trip + a standalone smoke test.

### Phase 64 specific

#### Method receiver choice — **natural receivers + `Collision` namespace**

Phase 64 is the FIRST phase to attach methods to data-carrying `object` types (`Rectangle`, `Vector2`, `Ray`, `BoundingBox`) rather than empty namespace types (`Keyboard`, `Draw`, etc.). Iron's syntax `func Rectangle.collides(other: Rectangle) -> Bool {}` must resolve to an instance method where `self` is the first argument. **PLANNER MUST VERIFY this dispatch shape before writing Plan 64-01.** If Iron's foreign-method-stub mechanism only supports namespace dispatch (no `self`), fallback plan: create a `Collision` namespace and expose every function as `Collision.rectangles(a, b)`, `Collision.circles(c1, r1, c2, r2)`, etc.

Assuming instance methods work (expected — nothing in Phase 60–63 forbids it; Vector2/Rectangle are plain `object` types, same structural kind as the empty namespace types that already take functions):

**2D — natural receivers:**
- `Rectangle.collides(other: Rectangle) -> Bool` — `CheckCollisionRecs`
- `Rectangle.intersection(other: Rectangle) -> Rectangle` — `GetCollisionRec`
- `Rectangle.contains_point(point: Vector2) -> Bool` — `CheckCollisionPointRec` (point-receiver alternative: `Vector2.inside_rect(rect)`; pick one, not both — go with rect-receiver for symmetry with `contains_circle`)
- `Rectangle.collides_circle(center: Vector2, radius: Float32) -> Bool` — `CheckCollisionCircleRec`
- `Vector2.inside_triangle(p1: Vector2, p2: Vector2, p3: Vector2) -> Bool` — `CheckCollisionPointTriangle`
- `Vector2.inside_polygon(points: [Vector2]) -> Bool` — `CheckCollisionPointPoly`
- `Vector2.on_line(p1: Vector2, p2: Vector2, threshold: Int32) -> Bool` — `CheckCollisionPointLine`

**2D — `object Collision {}` namespace (no natural single receiver):**
- `Collision.circles(c1: Vector2, r1: Float32, c2: Vector2, r2: Float32) -> Bool` — `CheckCollisionCircles`
- `Collision.circle_line(center: Vector2, radius: Float32, p1: Vector2, p2: Vector2) -> Bool` — `CheckCollisionCircleLine`
- `Collision.point_circle(point: Vector2, center: Vector2, radius: Float32) -> Bool` — `CheckCollisionPointCircle` (could be `Vector2.inside_circle(center, radius)` — planner picks; go with namespace to keep Vector2's method surface small)
- `Collision.lines(start_a: Vector2, end_a: Vector2, start_b: Vector2, end_b: Vector2) -> (Bool, Vector2)` — `CheckCollisionLines` (tuple return; see "Out-params" below)

**3D — natural receivers:**
- `BoundingBox.collides(other: BoundingBox) -> Bool` — `CheckCollisionBoxes`
- `BoundingBox.collides_sphere(center: Vector3, radius: Float32) -> Bool` — `CheckCollisionBoxSphere`
- `Ray.hit_sphere(center: Vector3, radius: Float32) -> RayCollision` — `GetRayCollisionSphere`
- `Ray.hit_box(box: BoundingBox) -> RayCollision` — `GetRayCollisionBox`
- `Ray.hit_mesh(mesh: Mesh, transform: Matrix) -> RayCollision` — `GetRayCollisionMesh`
- `Ray.hit_triangle(p1: Vector3, p2: Vector3, p3: Vector3) -> RayCollision` — `GetRayCollisionTriangle`
- `Ray.hit_quad(p1: Vector3, p2: Vector3, p3: Vector3, p4: Vector3) -> RayCollision` — `GetRayCollisionQuad`

**3D — `Collision` namespace:**
- `Collision.spheres(c1: Vector3, r1: Float32, c2: Vector3, r2: Float32) -> Bool` — `CheckCollisionSpheres`

This keeps the surface idiomatic where a natural receiver exists and explicit where pair-based math has no clear owner.

#### Out-param handling — **tuple return `(Bool, Vector2)`**

`CheckCollisionLines(Vector2 s1, Vector2 e1, Vector2 s2, Vector2 e2, Vector2 *collisionPoint)` writes the intersection point through a pointer. Iron v1.2.0 supports tuple returns (per PROJECT.md "compiler features already in place"). The shim pattern:

```c
void Iron_collision_lines(
    Iron_Vector2 s1, Iron_Vector2 e1,
    Iron_Vector2 s2, Iron_Vector2 e2,
    bool *out_hit, Iron_Vector2 *out_point
) {
    Vector2 cs1; memcpy(&cs1, &s1, sizeof(Vector2));
    /* … etc. for cs2, ce1, ce2 … */
    Vector2 pt;
    *out_hit = CheckCollisionLines(cs1, ce1, cs2, ce2, &pt);
    memcpy(out_point, &pt, sizeof(Iron_Vector2));
}
```

Iron side: `func Collision.lines(...) -> (Bool, Vector2) {}`. Compiler lowers tuple returns to output-pointer args on the C side. **PLANNER MUST VERIFY** Iron's tuple-return lowering interoperates with foreign-method-stub shims; if not, fallback to a `LineIntersection` struct (Bool + Vector2) or a variant pair where hit=false returns `Vector2(0, 0)`.

#### Plan slicing — **2 plans**

- **Plan 64-01 — 2D collision (COLL-01):** 11 functions. Methods on `Rectangle` (3), `Vector2` (3), `Collision` namespace (5 — incl. tuple-return `lines`). Validates: (a) methods-on-data-types dispatch, (b) `object Collision {}` creation, (c) tuple return on a foreign-method-stub, (d) `Iron_List_Iron_Vector2` reuse outside Draw. Wave 1.
- **Plan 64-02 — 3D collision (COLL-02):** 8 functions. Methods on `BoundingBox` (2), `Ray` (5), `Collision` namespace (1 — `spheres`). Validates: (a) `RayCollision` struct-by-value return (first time outside spline evaluator Vector2), (b) `Mesh` pass-by-value (first time — 14+ opaque pointer fields crossing FFI; pure layout copy, no deep ownership concerns), (c) `Matrix` pass-by-value (first time as a function arg — 64-byte struct). Wave 2 (depends on 64-01's dispatch verification).

Two plans keep COLL-01 and COLL-02 cleanly separable in SUMMARY/VERIFICATION and let Plan 64-02 skip if unforeseen 3D-specific issues arise. Sequential execution (parallelization=false in config).

### Claude's Discretion

- **Exact shim signatures.** E.g., whether `Iron_rectangle_collides` takes `(Iron_Rectangle, Iron_Rectangle)` directly or by const-ref. Default: by value, per Phase 63's established pattern (all Draw.* shims take structs by value).
- **Whether `Vector2.inside_circle(center, radius)` replaces `Collision.point_circle(point, center, radius)`.** Pick one for minimal Iron-side surface; default to namespace form per the listing above.
- **Smoke-test file location/content.** Phase 63 didn't add a new test file (pong.iron served as validation). Phase 64 has no natural consumer file — default is adding a minimal `tests/manual/collision_smoke.iron` that calls one function per receiver and printfs the result. Planner may skip if `ironc check` of raylib.iron plus a `clang -c iron_raylib.c` round-trip is deemed sufficient.
- **Parameter naming.** `other` vs `b` vs `rec` for the second Rectangle; `center`/`radius` pair order; etc. Pick the form that reads most naturally at call site.
- **Method placement order in `raylib.iron`.** By raylib-function order, by receiver, or by category. Default: group by receiver (all `Rectangle.*` together, all `Vector2.*` together, etc.), Collision namespace at the end.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Phase 60/61/62/63 foundation (authoritative design)
- `.planning/phases/60-type-enum-foundation/60-CONTEXT.md` — locked architectural decisions (shim-only, snake_case, opaque-ptr convention, `_`-prefix for opaque fields)
- `.planning/phases/60-type-enum-foundation/60-02-SUMMARY.md` — Rectangle/Vector2/Vector3 layout details
- `.planning/phases/60-03-PLAN.md`-equivalent... actually: `.planning/phases/60-type-enum-foundation/60-05-SUMMARY.md` — Ray/RayCollision/BoundingBox layout details (all three land here)
- `.planning/phases/61-window-system/61-CONTEXT.md` — struct-by-value return validation, section-marker pattern
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-CONTEXT.md` — namespace `object` pattern, snake_case precedent, enum cast convention (Phase 64 needs no enum work but the pattern is the reference)
- `.planning/phases/63-2d-drawing/63-04-SUMMARY.md` — tuple-return mechanism was NOT used here; `[Vector2]` array ABI via `Iron_List_Iron_Vector2`; struct-by-value Vector2-RETURN pattern (memcpy-out) — Phase 64 reuses this for `RayCollision` and `Rectangle` returns

### raylib upstream
- `src/vendor/raylib/raylib.h` lines 1305–1315 — 2D collision C signatures (COLL-01, 11 functions)
- `src/vendor/raylib/raylib.h` lines 1612–1619 — 3D collision C signatures (COLL-02, 8 functions)
- `src/vendor/raylib/raylib.h` lines 280–305 — Ray / RayCollision / BoundingBox struct definitions (authoritative layout)
- `src/vendor/raylib/rshapes.c` — 2D collision implementations (reference, not called directly; raylib builds it into the static lib)
- `src/vendor/raylib/rmodels.c` — 3D collision implementations (reference)

### Iron stdlib precedents
- `src/stdlib/iron_raylib.h` — add `/* ── Collision (Phase 64) ─────────── */` section with `Iron_rectangle_*`, `Iron_vector2_inside_*`, `Iron_collision_*`, `Iron_boundingbox_*`, `Iron_ray_hit_*` prototypes
- `src/stdlib/iron_raylib.c` — mirror section with shim bodies
- `src/stdlib/raylib.iron` — extend existing `object Rectangle`, `object Vector2`, `object Ray`, `object BoundingBox` with methods; add `object Collision {}` near the other namespace objects (`Window`, `Draw`, `Audio`, `Files`, `Keyboard`, etc.)
- `src/stdlib/iron_raylib.c` `Iron_draw_get_spline_point_linear` (et al.) — Vector2 memcpy-out precedent, reuse for `Iron_rectangle_intersection` and the 5 `Iron_ray_hit_*`
- `src/stdlib/iron_raylib.c` `Iron_draw_spline_linear` — `Iron_List_Iron_Vector2` by-value argument precedent, reuse for `Iron_vector2_inside_polygon`

### Project-level specs
- `.planning/REQUIREMENTS.md` lines 125–128 — COLL-01, COLL-02 descriptions
- `.planning/ROADMAP.md` — Phase 64 goal and 2 success-criteria truths
- `.planning/PROJECT.md` — Float32 convention, ABI constraints, shim-allowed principle

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`object Rectangle`, `object Vector2`, `object Vector3`, `object Ray`, `object RayCollision`, `object BoundingBox`, `object Mesh`, `object Matrix`** — all defined in `src/stdlib/raylib.iron` by Phase 60 with `_Static_assert` layout pinning. Phase 64 extends them with methods; no layout changes needed.
- **`object Draw`, `object Window`, `object Keyboard`, `object Files`, etc.** — namespace-type precedent already exists. Phase 64 adds `object Collision {}` next to them.
- **`Iron_List_Iron_Vector2` typedef + guard** — landed in Phase 63-03; `emit_structs.c` auto-emits the typedef for foreign-method-stubs with `[Vector2]` params (Phase 63-04 fix). Phase 64's `vector2_inside_polygon` reuses this directly.
- **Vector2-return memcpy-out pattern** — `Iron_draw_get_spline_point_linear` etc. (5 sites in iron_raylib.c). Phase 64 copies this template for `Iron_rectangle_intersection` (Rectangle return) and the 5 `Iron_ray_hit_*` (RayCollision return).
- **Auto-generated prototypes** — `emit_c` commit e3e5eee means the planner doesn't hand-sync prototypes.
- **Memory discipline** — ironc ~10 GB. Canonical validation = `clang -c src/stdlib/iron_raylib.c -Wall -Wextra` + `clang -c src/stdlib/iron_raylib_layout.c`. No pong.iron rebuild needed (pong has no collision markers).

### Established Patterns
- **Empty-body foreign-method stubs** lower to `Iron_<receiver>_<method>(...)` in C. Phase 64 is the first time the receiver is a data-carrying struct (`Rectangle`, not `Keyboard`). **Planner must confirm this lowering exists** — the most likely outcome is it already works (Iron treats all `object` types structurally the same), but if the stdlib foreign-method mechanism only supports namespace receivers, the planner falls back to `Collision.*` for everything.
- **Struct-by-value argument/return** — validated across Phases 61–63.
- **Tuple returns** — compiler feature since v1.2.0 (per PROJECT.md Context section). Phase 64 is the first time tuple-returns pass through a stdlib foreign-method-stub shim. **Planner must verify** the lowering produces a matching C signature. If Iron's tuple-return lowering emits multiple output-pointer args, the shim signature is `Iron_collision_lines(..., bool *out_hit, Iron_Vector2 *out_pt)`. If Iron emits a single packed return struct, the shim returns that struct type. Empirical test via a 2-minute probe in Plan 64-01 Task 1 resolves it.
- **`_Static_assert` layout grid** — already pins every Phase 64 input/output struct. No new asserts needed.

### Integration Points
- `src/stdlib/iron_raylib.c` / `iron_raylib.h` Collision section (new) — shim wrappers and prototypes
- `src/stdlib/raylib.iron` — extend existing type `object`s with methods; add `object Collision {}`
- (Optional) `tests/manual/collision_smoke.iron` — minimal standalone smoke test. No change to pong.iron / game_raylib.iron / hello_raylib.iron (none use collision).

</code_context>

<specifics>
## Specific Ideas

- **Instance-method dispatch on data-carrying `object` types** is unverified in the stdlib foreign-method-stub mechanism. Planner's first task in Plan 64-01 is a 1-function probe: declare `func Rectangle.collides(other: Rectangle) -> Bool {}`, wire the shim, run `ironc check src/stdlib/raylib.iron`. If it type-checks and lowers correctly, proceed. If it errors with "no such method receiver" or similar, fall back to `Collision.recs(a, b)` etc. for every function.
- **Tuple-return through foreign-method-stub** is also unverified. Planner's Task 1 second probe: declare `func Collision.lines(...) -> (Bool, Vector2) {}` and inspect the C signature `emit_c` generates. If it emits two output pointers (`bool *`, `Iron_Vector2 *`) — write the shim with that shape. If it emits a packed return struct, rewrite accordingly. If tuple return doesn't work at all, fall back: define a `LineIntersection` Iron `object { val hit: Bool; val point: Vector2 }` and return that by value.
- **`CheckCollisionPointLine` threshold parameter** is `int` (pixels), not float. Iron `Int32`.
- **`GetRayCollisionMesh` takes Mesh by value.** Mesh is 112-byte struct with 14 opaque-pointer fields. Iron passes it by value; the shim does `memcpy(&rl_mesh, &iron_mesh, sizeof(Mesh))` and forwards. No ownership transfer; raylib reads through the opaque pointers (vertex/index arrays) that the caller loaded via `LoadModel` (Phase 70's territory). Phase 64 only needs the mesh layout to cross the FFI correctly — layout is pinned by `_Static_assert` from Phase 60-04.
- **`GetRayCollisionMesh` takes Matrix by value.** First time Matrix appears as a function arg (Phase 60-02 defined the type; Phase 60-04 embedded it in Model; no function has consumed it until now). 64 bytes, straightforward memcpy.
- **`RayCollision` return (5 functions)** — 20-byte struct (`bool hit + 3 B padding + float distance + Vector3 point + Vector3 normal + _hitObj pointer` → confirm exact byte layout from raylib.h). `_Static_assert` from Phase 60-05 pins it. Use memcpy-out pattern from spline evaluators.
- **`GetCollisionRec` returns Rectangle.** 16-byte struct (4 × Float32). Memcpy-out.
- **No enum params anywhere.** Phase 64 shim code is pure struct/scalar marshalling — simplest binding surface since Phase 60.
- **Consumer-file impact: none.** Neither `pong.iron`, `game_raylib.iron`, nor `hello_raylib.iron` has a `-- PHASE 64:` marker (Phase 60-08 didn't plant any — collision isn't used in the 2D pong demo or the 3D ad-hoc test). Phase 64 completes without touching those files.

</specifics>

<deferred>
## Deferred Ideas

- **Swept collision (continuous collision detection).** raylib 5.5 public API has no swept-collision functions. Out of scope; users can compose via repeated point samples.
- **Spatial partitioning (BVH, quadtree, spatial hash).** Not part of raylib's public API; physics-library territory. Explicitly out of scope per PROJECT.md "Out of Scope" direction for rlgl / low-level layers.
- **Convex-hull / SAT collisions.** Beyond raylib's exposed surface.
- **`GetRayCollisionModel(Ray, Model)` convenience.** raylib has no single-function Ray↔Model test; users call `GetRayCollisionMesh` per mesh in the model. A helper `Ray.hit_model(model)` that loops across `model.meshes` could land in Phase 70 (Models) or Phase 73 (API polish) — out of Phase 64 scope.
- **Batched collision (array-of-rects vs array-of-rects).** User-level loop; not exposed by raylib.
- **Collision mask/layer system.** Game-engine abstraction above raylib; users implement per-game.
- **Distance/nearest-point queries on shapes.** raymath has vector distance helpers; any shape-specific nearest-point math lives in Phase 65 (raymath) or user code.
- **Documentation tile in ROADMAP.md for how `Collision.*` vs `Rectangle.*` dispatch differs.** Nice-to-have comment block in raylib.iron — planner's discretion during Plan 64-01.

</deferred>

---

*Phase: 64-collision-2d-3d*
*Context gathered: 2026-04-17 via smart discuss (autonomous)*
