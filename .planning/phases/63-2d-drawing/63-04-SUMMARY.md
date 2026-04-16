---
phase: 63-2d-drawing
plan: 04
subsystem: stdlib-raylib
tags: [raylib, ffi, splines, vector2-return, iron-list, memcpy, shim, compiler-fix, consumer-rewrite]

requires:
  - phase: 60-type-enum-foundation
    provides: "Iron_Vector2 / Iron_Color structs + _Static_assert ABI grid pinning Vector2/Color byte-identity to raylib C types"
  - phase: 63-2d-drawing (Plan 01)
    provides: "13 frame + stack-mode Draw.* bindings + Vector2 RETURN precedent pattern (Iron_window_get_window_position at iron_raylib.c:137-142)"
  - phase: 63-2d-drawing (Plan 02)
    provides: "17 pixel/line/circle/ellipse/ring Draw.* bindings; Vector2-by-value INPUT ABI proven"
  - phase: 63-2d-drawing (Plan 03)
    provides: "19 rectangle/triangle/polygon Draw.* bindings; Iron_List_Iron_Vector2 typedef in iron_raylib.h (guarded); Iron [Vector2] array ABI probe outcome A — struct BY VALUE"

provides:
  - "5 spline-segment Iron_draw_* shims (linear/basis/catmull_rom/bezier_quadratic/bezier_cubic)"
  - "5 spline-evaluator Iron_draw_* shims — FIRST Vector2 RETURN path exercised in the Draw namespace (5 call sites via memcpy-out pattern)"
  - "6 array-input shims (DrawLineStrip + 5 whole-splines) using Branch B signature (Iron_List_Iron_Vector2 struct by-value) — proven by 63-03 probe"
  - "16 matching Iron `func Draw.*` stubs in raylib.iron (10 fixed-point + 6 array-input)"
  - "3 consumer files (pong.iron / game_raylib.iron / hello_raylib.iron) re-enabled with real Draw.* calls — zero `-- PHASE 63:` markers remain"
  - "2 mislabeled DrawText markers in pong.iron re-labeled to `-- PHASE 67:` (TEXT-07/08 is Phase 67, not Phase 63)"
  - "Compiler fix in src/lir/emit_structs.c — emit_mono_list_decls() now scans foreign-method-stub and extern-decl parameter/return types for Iron_List_<T> typedef emission"
  - "Canonical end-to-end ironc build of pong.iron: 2,658,464-byte Mach-O arm64 executable that initializes raylib on macOS"

affects:
  - 64-collision (Phase 63 COMPLETE — unblocks)
  - 66-textures (Phase 63 COMPLETE — unblocks; texture-draw uses Vector2 source-rect + begin_texture_mode stack-mode from 63-01)
  - 67-text-fonts (receives 2 DrawText markers re-labeled from PHASE 63; TEXT-07/08 already mapped to Phase 67 per REQUIREMENTS.md)
  - 69-3d-drawing (reuses Vector2-RETURN memcpy-out pattern for Vector3/BoundingBox returns)
  - 73-api-polish (receives any post-63 ergonomic gaps: Float32 literal verbosity, spline helper factories)
  - src/lir/emit_structs.c (compiler-wide improvement — ALL future stdlib modules with [T] foreign-method params now emit typedefs automatically)

tech-stack:
  added:
    - "Spline-segment + spline-evaluator C shims (10 new symbols with 5 Vector2-RETURN paths)"
    - "Whole-spline + DrawLineStrip array shims (6 new symbols; Iron_List_Iron_Vector2 by-value)"
  patterns:
    - "Vector2-RETURN via memcpy-out: `Vector2 rl = GetSplinePoint<T>(...); struct Iron_Vector2 out; memcpy(&out, &rl, sizeof(Vector2)); return out;` — matches iron_raylib.c:137-142 precedent; now exercised 5x in the Draw namespace (first time)"
    - "Spline-segment shim: 2–4 Vector2 memcpys + 1 Color memcpy into stack locals; forward to DrawSplineSegment<T>"
    - "Whole-spline shim (Branch B): accept Iron_List_Iron_Vector2 by value; forward points.items as `(const Vector2 *)`; trust caller count (matches 63-03 triangle_fan/strip template)"
    - "Float32 `t` parameter: raylib expects float not double; Iron users must write `Float32(0.5)` at call sites (documented in the stub docstring)"
    - "Consumer-file rewrite (placeholder frame): replace `-- PHASE 63:` markers with a single-frame Draw.begin/clear/line/rectangle/end block using hardcoded screen coordinates — real game logic awaits Phase 73 showcase"
    - "emit_mono_list_decls() scan B: iterate module->funcs where fn->is_extern && !fn->extern_c_name (foreign-method stubs). For IRON_TYPE_ARRAY params/returns with IRON_TYPE_OBJECT element, emit the Iron_List_Iron_<T> typedef. Dedup via the same stb_ds map as the prior two scans."

key-files:
  created:
    - .planning/phases/63-2d-drawing/63-04-SUMMARY.md
  modified:
    - src/stdlib/iron_raylib.h
    - src/stdlib/iron_raylib.c
    - src/stdlib/raylib.iron
    - src/lir/emit_structs.c
    - examples/pong/pong.iron
    - tests/manual/game_raylib.iron
    - tests/integration/web/hello_raylib.iron

key-decisions:
  - "Task 2 ran Branch B (Iron_List_Iron_Vector2 struct by-value), NOT Branch A (const Iron_Vector2 *). Rationale: 63-03 Task 1 probe landed outcome A / ARRAY_PARAM_LIST mode (struct-wrapper by value). The plan's literal Branch A signature would have mismatched ironc's emitted call-site code. Plan explicitly anticipated this: 'BRANCH B: substitute const struct Iron_Vector2 * with whatever struct-wrapper signature the 63-03 probe revealed.' Applied verbatim."
  - "Vector2 RETURN path uses memcpy-out into a struct-local: `struct Iron_Vector2 out; memcpy(&out, &rl, sizeof(Vector2)); return out;`. Matches Iron_window_get_window_position pattern from Phase 61. 5 evaluators exercise this 5 times — first Vector2 RETURN in the Draw namespace. Byte-identity guaranteed by Phase 60-02 _Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2))."
  - "Iron method name `bezier_quadratic` normalizes raylib's C name `GetSplinePointBezierQuad` (short `Quad`) for symmetry with the draw side (`DrawSplineBezierQuadratic` uses the long form). Shim body casts to the short C symbol at the raylib call; users see `bezier_quadratic` on both sides."
  - "Float32 `t` parameter decision explicit in docstring. Iron's default Float is double (IEEE 754 binary64); raylib's float is IEEE 754 binary32. A bare `0.5` literal at a call site would be double and would silently truncate to Float32 inside the stub — or worse, produce a type-check error. Comment directs users to write `Float32(0.5)`."
  - "Compiler fix landed INSIDE this plan (src/lir/emit_structs.c) as a Rule 3 blocking-issue auto-fix. Rationale: without the fix, the canonical pong.iron end-to-end build FAILS with 'unknown type name Iron_List_Iron_Vector2'. The failure is directly caused by 63-03's triangle_fan/strip prototypes + 63-04's 6 whole-spline/line_strip prototypes ALL referencing the Iron_List_Iron_Vector2 typedef that ironc never emitted because pong.iron never literals a [Vector2]. Fix: extend emit_mono_list_decls() with two new scans (extern_decls + foreign-method-stub funcs). Benefit: compiler-wide — every future stdlib module with a [T] foreign-method-stub param auto-emits the typedef. No plan-scope creep: the fix is <100 lines in one file, addresses a gap directly created by this phase's bindings."
  - "Acceptance criterion 'grep -c PHASE 63 returns 0' conflicted with the plan's literal rewrite text at lines 692-693 which itself contained the substring 'PHASE 63' ('re-marked from PHASE 63'). Rule 1 deviation: inline annotations rephrased to 'was mislabeled' without the substring. Behavior and semantic content preserved; grep now returns 0 as required."
  - "Deleted `val _unused_3 = fg` / `val _unused_4 = bg` in game_raylib.iron (fg/bg now consumed by Draw.clear + Draw.rectangle), renumbered with gaps. Similar in hello_raylib.iron. pong.iron's paddle-coord hardcoding (400/0/600/250/10/100) matches Phase 60-08's 800×600 screen intent — Phase 73 showcase will replace with real game-state variables."
  - "Canonical pong.iron runs end-to-end: initializes raylib 5.5 on macOS (DESKTOP/GLFW backend, 1728x1117 display), binds to 800x600 window, loads all raylib modules. This is the FIRST Phase 63 end-to-end validation on a real user file — matches Phase 60-08 / 61-04 / 62-01 canonical pattern. ironc invoked 2 times (pong + game_raylib); per HANDOFF.md memory discipline (~10 GB/invocation), kept within budget."

patterns-established:
  - "Foreign-method-stub typedef auto-emission — stdlib modules authoring `func <Namespace>.<method>(<arg>: [T], ...)` no longer need manual IRON_LIST_<T>_STRUCT_DEFINED guards in per-module .h files (iron_net.h + iron_raylib.h already have them for historical reasons; new modules can skip). Compiler-wide improvement."
  - "Vector2-RETURN via memcpy-out applies to any struct-return from raylib — Vector3/Vector4/Quaternion/Rectangle/Matrix returns in Phases 65/69/70 can all reuse this exact template. byte-identity via Phase 60-02 _Static_assert grid is the invariant that makes the reinterpret safe across all types."
  - "Consumer-file placeholder-frame: 3 canonical single-frame bodies (800×600 pong / 800×600 game_raylib rectangle / 800×600 hello_raylib rectangle) that exercise Draw.begin/clear/rectangle/end. Phase 73 showcase will replace with real game logic, but these placeholders will remain the canonical 'are the bindings wired?' smoke test."

requirements-completed: [DRAW2D-08, DRAW2D-15, DRAW2D-16]

duration: 10min
completed: 2026-04-16
---

# Phase 63 Plan 04: Splines + Consumer Re-enablement Summary

**16 new Draw.\* bindings (10 fixed-point spline surfaces + 6 array-input) closing DRAW2D-08/15/16, compiler fix for foreign-method-stub array typedefs, and canonical end-to-end pong.iron build — Phase 63 is now COMPLETE**

Plan 63-04 closed the last open Phase 63 requirements (DRAW2D-08 DrawLineStrip, DRAW2D-15 splines, DRAW2D-16 evaluators), re-enabled all three consumer files (pong/game_raylib/hello_raylib), and validated the entire Phase 63 surface end-to-end via `./build/ironc build examples/pong/pong.iron`. During the canonical build, a dormant compiler gap surfaced: ironc's emit_mono_list_decls() didn't emit Iron_List_Iron_Vector2 when the typedef was referenced ONLY by foreign-method-stub prototypes (not by user ARRAY_LIT). Fixed in-plan as a Rule 3 auto-fix with a compiler-wide improvement (two new scan passes over extern_decls and is_extern funcs).

## Files Modified

- `src/stdlib/iron_raylib.h` — +29 lines (16 new prototypes: 5 segment + 5 evaluator + 6 array)
- `src/stdlib/iron_raylib.c` — +154 lines (16 shim implementations; first Vector2 RETURN × 5)
- `src/stdlib/raylib.iron` — +60 lines (16 `func Draw.*` empty stubs with Float32 `t` doc)
- `src/lir/emit_structs.c` — +74 lines (Rule 3 compiler fix for foreign-method-stub typedefs)
- `examples/pong/pong.iron` — 7 PHASE 63 markers rewritten (5→Draw.* calls, 2→PHASE 67 re-labels)
- `tests/manual/game_raylib.iron` — 1 PHASE 63 marker → Draw.begin/clear/rectangle/end frame
- `tests/integration/web/hello_raylib.iron` — 1 PHASE 63 marker → Draw.begin/clear/rectangle/end frame

## Task 1 — 5 Spline-Segment + 5 Spline-Evaluator Shims

10 fixed-point-count shims land. 5 segment shims use the proven Vector2-by-value INPUT + Color-by-value INPUT template (Plans 63-01/02/03 established):

```c
void Iron_draw_spline_segment_linear(struct Iron_Vector2 p1, struct Iron_Vector2 p2,
                                      float thick, struct Iron_Color color) {
    Vector2 a, b;
    Color c;
    memcpy(&a, &p1,    sizeof(Vector2));
    memcpy(&b, &p2,    sizeof(Vector2));
    memcpy(&c, &color, sizeof(Color));
    DrawSplineSegmentLinear(a, b, thick, c);
}
```

5 evaluator shims exercise the **FIRST Vector2 RETURN path in the Draw namespace** — matches Iron_window_get_window_position at iron_raylib.c:137-142:

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

Byte-identity guaranteed by Phase 60-02 `_Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2))`. `bezier_quadratic` normalizes raylib's short C name `BezierQuad`.

### Iron stubs (raylib.iron):

```iron
func Draw.spline_segment_linear(p1: Vector2, p2: Vector2, thick: Float32, color: Color) {}
func Draw.spline_segment_basis(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, thick: Float32, color: Color) {}
func Draw.spline_segment_catmull_rom(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, thick: Float32, color: Color) {}
func Draw.spline_segment_bezier_quadratic(p1: Vector2, c2: Vector2, p3: Vector2, thick: Float32, color: Color) {}
func Draw.spline_segment_bezier_cubic(p1: Vector2, c2: Vector2, c3: Vector2, p4: Vector2, thick: Float32, color: Color) {}

func Draw.get_spline_point_linear(start: Vector2, end: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_basis(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_catmull_rom(p1: Vector2, p2: Vector2, p3: Vector2, p4: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_bezier_quadratic(p1: Vector2, c2: Vector2, p3: Vector2, t: Float32) -> Vector2 {}
func Draw.get_spline_point_bezier_cubic(p1: Vector2, c2: Vector2, c3: Vector2, p4: Vector2, t: Float32) -> Vector2 {}
```

Commit: `d2bbf93`.

## Task 2 — Whole-Spline Array Variants (Branch B)

Ran **Branch B** (Iron_List_Iron_Vector2 struct by-value), cross-referenced with 63-03-SUMMARY.md's outcome-A probe result (ARRAY_PARAM_LIST mode). The plan's literal Branch A signature (`const struct Iron_Vector2 *points`) would have mismatched ironc's call-site emission; Branch B matches the actual ABI ironc produces.

6 array-input shims bound:

```c
void Iron_draw_line_strip(Iron_List_Iron_Vector2 points, int32_t count, struct Iron_Color color);
void Iron_draw_spline_linear(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_basis(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_catmull_rom(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_bezier_quadratic(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
void Iron_draw_spline_bezier_cubic(Iron_List_Iron_Vector2 points, int32_t count, float thick, struct Iron_Color color);
```

Shim body pattern (all 6 identical modulo raylib function name):

```c
void Iron_draw_spline_linear(Iron_List_Iron_Vector2 points, int32_t count,
                              float thick, struct Iron_Color color) {
    Color c;
    memcpy(&c, &color, sizeof(Color));
    DrawSplineLinear((const Vector2 *)points.items, (int)count, thick, c);
}
```

DrawTriangleFan/DrawTriangleStrip were already bound in 63-03 — skipped here per 63-03-SUMMARY.md § Plan 63-04 Handoff.

Iron-side stubs:

```iron
func Draw.line_strip(points: [Vector2], count: Int32, color: Color) {}
func Draw.spline_linear(points: [Vector2], count: Int32, thick: Float32, color: Color) {}
func Draw.spline_basis(points: [Vector2], count: Int32, thick: Float32, color: Color) {}
func Draw.spline_catmull_rom(points: [Vector2], count: Int32, thick: Float32, color: Color) {}
func Draw.spline_bezier_quadratic(points: [Vector2], count: Int32, thick: Float32, color: Color) {}
func Draw.spline_bezier_cubic(points: [Vector2], count: Int32, thick: Float32, color: Color) {}
```

**No descope to Phase 73** — Branch B covered all 6 array-input functions with zero fallback needed.

Commit: `47ebf92`.

## Task 3 — Consumer-File Rewrites

### pong.iron (5 real + 2 re-labeled)

Before (lines 95-101):

```iron
-- PHASE 63: Draw.begin()
-- PHASE 63: Draw.clear(bg)
-- PHASE 63: DrawLine(SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H, divider)
-- PHASE 63: DrawRectangle(0, left_y, PADDLE_W, PADDLE_H, fg)
-- PHASE 63: DrawText(score_str, SCREEN_W / 4, 20, 40, fg)
-- PHASE 63: DrawText("GAME OVER", ..., accent)
-- PHASE 63: Draw.end()
```

After:

```iron
-- Phase 63: render a single frame to exercise the Draw namespace.
-- [...comment block about placeholder coordinates...]
Draw.begin()
Draw.clear(bg)
Draw.line(400, 0, 400, 600, divider)
Draw.rectangle(0, 250, 10, 100, fg)
-- PHASE 67: DrawText(score_str, 200, 20, 40, fg)      -- was mislabeled (TEXT-07 is Phase 67 territory)
-- PHASE 67: DrawText("GAME OVER", 300, 280, 40, accent) -- was mislabeled (TEXT-08 is Phase 67 territory)
Draw.end()
```

- 5 markers → Draw.begin / Draw.clear / Draw.line / Draw.rectangle / Draw.end
- 2 mislabeled DrawText markers → re-labeled to `-- PHASE 67:` (TEXT-07/08 belongs to Phase 67 per REQUIREMENTS.md)

### game_raylib.iron (1 marker → 4-call frame)

Before: `-- PHASE 63: Draw.begin() / Draw.clear(bg) / Draw.end()`

After:
```iron
-- Phase 63: render a single frame to exercise the Draw namespace.
Draw.begin()
Draw.clear(bg)
Draw.rectangle(100, 100, 50, 50, fg)
Draw.end()
```

`_unused_3 = fg` / `_unused_4 = bg` removed (now consumed by Draw.* calls).

### hello_raylib.iron (1 marker → 4-call frame)

Same shape as game_raylib. `_unused_1 = bg` / `_unused_2 = fg` removed.

### Verification

```
$ grep -c 'PHASE 63' examples/pong/pong.iron tests/manual/game_raylib.iron tests/integration/web/hello_raylib.iron
0 / 0 / 0
$ grep -c 'PHASE 67:' examples/pong/pong.iron
2
```

Commit: `9fe20da`.

### Rule 1 deviation: plan text vs acceptance criterion

The plan's literal rewrite text at lines 692-693 was:
```
-- PHASE 67: DrawText(score_str, 200, 20, 40, fg)      -- re-marked from PHASE 63 (mislabel: TEXT-07 belongs to Phase 67)
```

This INLINE annotation contains the substring `PHASE 63`, which would fail the plan's own acceptance criterion `grep -c 'PHASE 63' examples/pong/pong.iron returns exactly 0`. Rule 1 auto-fix: inline annotation rephrased to `was mislabeled (TEXT-07 is Phase 67 territory)` — same semantic, no `PHASE 63` substring. Acceptance now passes.

## Task 4 — Canonical End-to-End pong.iron Build

**Build output (initial attempt):**

```
/tmp/iron_ZrdtSg.c:972:29: error: unknown type name 'Iron_List_Iron_Vector2';
  did you mean 'Iron_List_Iron_Closure'?
  972 | void Iron_draw_triangle_fan(Iron_List_Iron_Vector2 points, int32_t count, Iron_Color color);
```

8 errors. Root cause: ironc's `emit_mono_list_decls()` only emitted `Iron_List_Iron_Vector2` typedef when user code created an `ARRAY_LIT` of Vector2. pong.iron doesn't — so the typedef was missing, yet 8 forward-declared prototypes (2 from 63-03, 6 from 63-04) referenced it.

### Rule 3 compiler fix (src/lir/emit_structs.c)

Added two new scans to `emit_mono_list_decls()`:

- **Scan A: extern_decls** — iterate `ctx->module->extern_decls`; for each `IronLIR_ExternDecl`, check every `param_types[pi]` and `return_type`. When `kind == IRON_TYPE_ARRAY` with `array.elem->kind == IRON_TYPE_OBJECT`, emit the typedef + IRON_LIST_DECL + IRON_LIST_IMPL for the concrete element type.
- **Scan B: foreign-method-stub funcs** — iterate `ctx->module->funcs` where `fn->is_extern && !fn->extern_c_name`. Same param/return scan as Scan A. **This is the scan that catches `Draw.triangle_fan(points: [Vector2], ...)` and friends** because Iron lowers these as regular LIR funcs (empty-body with is_extern=true), NOT as extern_decls.

Dedup via the existing `emitted_mono_list_types` stb_ds map. Both scans share a macro `PLAN_63_04_EMIT_LIST_FOR` for consistency.

### Second build attempt — success

```
$ ./build/ironc build examples/pong/pong.iron -o /tmp/iron_pong_63_04
Built: /tmp/iron_pong_63_04

$ file /tmp/iron_pong_63_04
/tmp/iron_pong_63_04: Mach-O 64-bit executable arm64

$ wc -c < /tmp/iron_pong_63_04
2658464
```

**Binary details:**
- **Size:** 2,658,464 bytes
- **Platform:** macOS arm64 (Mach-O 64-bit executable arm64)
- **Runtime exec attempted:** Yes — pong initialized raylib 5.5 successfully, loaded DESKTOP/GLFW backend, detected 1728×1117 display, bound to 800×600 window. Confirms the full pipeline (ironc → emit_c → clang → link against raylib vendored + iron_raylib.c → Mach-O → dynamic loader → raylib runtime init → GLFW window) is end-to-end sound.

### game_raylib.iron bonus build

```
$ ./build/ironc build tests/manual/game_raylib.iron -o /tmp/iron_game_raylib_63_04
Built: /tmp/iron_game_raylib_63_04

$ file /tmp/iron_game_raylib_63_04
/tmp/iron_game_raylib_63_04: Mach-O 64-bit executable arm64

$ wc -c < /tmp/iron_game_raylib_63_04
2658376
```

Also compiles cleanly. ironc invoked 2 times (pong + game_raylib) per HANDOFF.md memory discipline — kept within budget.

**hello_raylib.iron SKIPPED** — web target requires emcc tooling not currently installed on this host. Native compile is validated by pong; web build has no Phase 63–specific risk over native.

Commit: `6dbd5d9`.

## Final Plan 63-04 Counts

| Metric                           | Before 63-04 | After 63-04 |
| -------------------------------- | ------------ | ----------- |
| `func Draw.*` in raylib.iron     | 49           | **65**      |
| `Iron_draw_*` in iron_raylib.h   | 49           | **65**      |
| `Iron_draw_*` shims in iron_raylib.c | 49        | **65**      |
| PHASE 63 markers across repo     | 7 (pong) + 1 (game) + 1 (hello) = 9 | **0** |
| PHASE 67 markers in pong.iron    | 0            | 2           |

Parity between Iron and C sides verified. All 65 bound and paired.

## Phase 63 Completion (across all 4 plans)

| Plan  | Draw.* added | Requirements closed                     | Commits                             |
| ----- | ------------ | --------------------------------------- | ----------------------------------- |
| 63-01 | 13           | DRAW2D-01..06                           | e2739f6, ec632e5, 7768040           |
| 63-02 | 17           | DRAW2D-07, 08 (4/5), 09, 10, 11         | 20b4f53, cc494a2                    |
| 63-03 | 19           | DRAW2D-12, 13, 14                       | 4875f67, 10184c3                    |
| 63-04 | 16           | DRAW2D-08 (5/5), 15, 16                 | d2bbf93, 47ebf92, 9fe20da, 6dbd5d9  |
| Total | **65**       | **DRAW2D-01..16 — ALL 16 CLOSED**       |                                     |

**Phase 63 is COMPLETE.** All 16 DRAW2D-XX requirements closed. Phases 64 (Collision), 66 (Textures), and 67 (Text & Fonts) are now unblocked and can start in parallel (Phase 64 depends only on 63; Phase 66 depends on 63's begin_texture_mode/Vector2 types; Phase 67 depends on 63's Draw namespace and re-inherits the 2 DrawText markers from pong.iron).

## Verification

**All overall plan checks pass:**

```bash
$ clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra
# EXIT 0, zero warnings

$ clang -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra
# EXIT 0 — Phase 60 _Static_assert grid held (all 392 asserts still fire)

$ grep -c '^func Draw\.' src/stdlib/raylib.iron
65

$ grep -c 'PHASE 63' examples/pong/pong.iron tests/manual/game_raylib.iron tests/integration/web/hello_raylib.iron
examples/pong/pong.iron:0
tests/manual/game_raylib.iron:0
tests/integration/web/hello_raylib.iron:0

$ grep -c 'PHASE 67:' examples/pong/pong.iron
2

$ ./build/ironc build examples/pong/pong.iron -o /tmp/iron_pong_63_04
Built: /tmp/iron_pong_63_04

$ file /tmp/iron_pong_63_04
/tmp/iron_pong_63_04: Mach-O 64-bit executable arm64
```

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking issue] Compiler gap: Iron_List_<T> typedefs not emitted for foreign-method-stub array params**

- **Found during:** Task 4 (canonical ironc build failed)
- **Issue:** `emit_mono_list_decls()` in src/lir/emit_structs.c only emitted `typedef struct Iron_List_Iron_<T>` for types it found via ARRAY_LIT scanning or `ctx->monomorphic_collections`. When `raylib.iron` declares `func Draw.triangle_fan(points: [Vector2], ...)` as a foreign-method stub and pong.iron never creates a `[Vector2]` literal, neither path fired. But ironc still emitted forward-declared prototypes referencing the missing typedef, causing 8 clang errors.
- **Fix:** Added two new scans to `emit_mono_list_decls()`:
  - Scan A iterates `module->extern_decls` for `IRON_TYPE_ARRAY` params/returns
  - Scan B iterates `module->funcs` where `is_extern && !extern_c_name` (foreign-method stubs) for the same
  - Both emit the typedef + IRON_LIST_DECL + IRON_LIST_IMPL via a shared macro; dedup via the existing stb_ds map
- **Scope note:** This IS directly caused by Plan 63-04's bindings (plus 63-03's triangle_fan/strip). Compiler-wide benefit; future stdlib modules with [T] foreign-method params get automatic typedef emission.
- **Files modified:** src/lir/emit_structs.c (+74 lines)
- **Commit:** 6dbd5d9

**2. [Rule 1 - Bug] Plan's literal rewrite text contained substring 'PHASE 63' that violated its own acceptance criterion**

- **Found during:** Task 3 (grep acceptance check after rewrite)
- **Issue:** Plan lines 692-693 specified inline annotation `-- re-marked from PHASE 63 (mislabel: TEXT-07 belongs to Phase 67)`. The substring `PHASE 63` in this annotation caused `grep -c 'PHASE 63' examples/pong/pong.iron` to return 2 instead of the required 0.
- **Fix:** Rephrased both annotations to `-- was mislabeled (TEXT-07 is Phase 67 territory)` — same semantic, no substring collision.
- **Files modified:** examples/pong/pong.iron (2 lines)
- **Commit:** 9fe20da

### Plan arithmetic discrepancies (none this plan)

- 63-04 plan's expected final count of 65 `func Draw.*` matched exactly. No arithmetic drift like 63-03.
- Plan's Branch B prediction matched observed ironc lowering (`Iron_List_Iron_Vector2` struct by value, not pointer). No signature surprises.

## Authentication Gates

None.

## Post-Phase Polish Items for Phase 73

- **Float32 literal ergonomics:** `Draw.get_spline_point_linear(start, end, Float32(0.5))` is verbose. Phase 73 should consider either an Iron language change (literal suffix `0.5f`) or a typecheck relaxation (auto-narrow Float-literal → Float32 when call site demands Float32).
- **Spline helper factories:** `Draw.catmull_rom_from_points(pts)` convenience wrapper that calls `Draw.spline_catmull_rom(pts, pts.count, Float32(1.0), color)` — Phase 73 API-polish territory.
- **Spline evaluator vectorization:** Currently `Draw.get_spline_point_*` evaluates one point at a time. A `Draw.get_spline_points_*(pts, count, samples) -> [Vector2]` batch variant would be faster than Iron-level looping. Phase 73 API polish.

## Self-Check: PASSED
