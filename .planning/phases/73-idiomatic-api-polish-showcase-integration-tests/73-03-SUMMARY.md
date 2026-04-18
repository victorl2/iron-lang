---
phase: 73-idiomatic-api-polish-showcase-integration-tests
plan: 03
subsystem: showcase
tags: [raylib, showcase, api-10, native, web-deferred]

# Dependency graph
requires:
  - phase: 73-02
    provides: 5 constructor sugars (Color.rgb / Vector2.of / Vector3.of / Rectangle.of) + API surface locked at 698 stubs / 577 prototypes
  - phase: 71-02
    provides: grayscale.fs shader asset at tests/assets/shaders/grayscale.fs
  - phase: 70-04
    provides: cube.obj model asset at tests/assets/models/cube.obj + Model.load fallback pattern
  - phase: 68-05
    provides: bounce.wav audio asset at tests/assets/bounce.wav + Audio.init/Sound.load/play pattern
  - phase: 69-04
    provides: Camera3D orbital update pattern + Int32(0) PERSPECTIVE workaround idiom
provides:
  - examples/raylib_showcase/raylib_showcase.iron — canonical single-file 12-category demonstration
  - API-10 deliverable (v2.0.0-alpha showcase example)
  - Native build artifact ./raylib_showcase (2,745,552 B arm64 Mach-O)
  - Pong regression GREEN at 2,745,656 B (unchanged from Plan 73-02)
affects: [phase-73-04-integration-tests, v2.0.0-alpha-milestone-declaration]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Column-0 `-- ── <CAT>: <purpose> ───` tag per category (12 total) — Phase 71-02 smoke-file audit convention applied to examples/ directory"
    - "Constructor sugar adoption in showcases — Vector2.of / Vector3.of / Color.rgb / Rectangle.of replace nested Float32(...) / UInt8(...) literal wrappers"
    - "Procedural TEX (Image.checked → to_texture → unload image) — no PNG vendoring, asset-strategy decision from Plan 73-03 RESEARCH.md"
    - "Model.load with procedural Mesh.cube fallback guarded by Model.is_valid — inherited from examples/model_viewer/"

key-files:
  created:
    - examples/raylib_showcase/raylib_showcase.iron
    - .planning/phases/73-idiomatic-api-polish-showcase-integration-tests/73-03-SUMMARY.md
  modified:
    - .gitignore (+2 lines: /raylib_showcase + /raylib_showcase.c root-anchored per convention)

key-decisions:
  - "Renamed showcase.iron → raylib_showcase.iron inside the raylib_showcase/ directory (Rule-3 deviation from plan text) so ironc's output-binary-name derivation produces ./raylib_showcase — matches rotating_cube/rotating_cube.iron, model_viewer/model_viewer.iron, post_fx/post_fx.iron convention"
  - "Used /raylib_showcase (root-anchored) in .gitignore (Rule-1 deviation from plan text's `^raylib_showcase$`) — unanchored form would inadvertently ignore the examples/raylib_showcase/ directory itself (no !examples/ re-include pattern exists in .gitignore); root-anchored form matches /rotating_cube /post_fx /model_viewer convention and prevents accidental directory shadowing"
  - "Web build deferred per Plan 73-01 D3 (emsdk not installed in execution environment) — native build exit 0 is the Phase 73-03 acceptance gate per autonomous-mode hint; web build verification gets deferred to Phase 73-04 integration test matrix which requires emsdk-equipped environment regardless"
  - "Used Int32(0) workaround for CameraProjection.PERSPECTIVE in Camera3D() initializer (Plan 73-01 D5 residual) — Phase 73 must not become open-ended compiler-work vehicle per CONTEXT.md:151 escape hatch"
  - "Used Rectangle.collides(r1, r2) for COLL category (not Shapes.check_collision_rec_rec as plan text suggested) — grep against src/stdlib/raylib.iron showed Shapes namespace doesn't exist; Rectangle.collides is the receiver-first form shipped from Phase 64-01"
  - "Adopted 73-02 constructor sugar throughout (Color.rgb at palette sites; Vector3.of / Vector2.of at Camera3D + origin; Rectangle.of at collision rects) — demonstrates API-03 ergonomic improvement in live code"

patterns-established:
  - "Showcase naming: directory-name matches .iron-file-basename matches produced-binary-name (examples/raylib_showcase/raylib_showcase.iron → ./raylib_showcase)"
  - "Constructor sugar adoption in examples/ directory (Plan 73-02 deliverable now has a live consumer beyond raylib.iron stubs)"
  - "12-category coverage map with reproducible grep verification (per-category regex + count ≥1 assertion)"

requirements-completed: [API-10]

# Metrics
duration: ~4 min
completed: 2026-04-18
---

# Phase 73 Plan 03: raylib_showcase Summary

**Created examples/raylib_showcase/raylib_showcase.iron — single-file 161-line canonical demonstration covering all 12 in-scope v2.0.0-alpha raylib categories (WIN/INPUT/DRAW2D/COLL/TEX/TEXT/AUDIO/DRAW3D/MODEL/SHADER/MATH/FILE) with one column-0 tag per category. Native build ./raylib_showcase = 2,745,552 B arm64 Mach-O (exit 0). Adopts all 5 Plan 73-02 constructor sugars in live example code. Pong regression GREEN at 2,745,656 B. API-10 closed. Web build deferred per D3 (emsdk not in environment).**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-04-18T14:23:44Z
- **Completed:** 2026-04-18T14:27:41Z
- **Tasks:** 2 (Task 1 showcase authorship + Task 2 SUMMARY checkpoint)
- **Files modified:** 2 source + 1 SUMMARY = 3

## Accomplishments

- **API-10 closed** — `examples/raylib_showcase/raylib_showcase.iron` shipped as the canonical single-file demonstration of Iron's v2.0.0-alpha raylib binding. 161 lines, 12 categories, ~1–2 API calls per category. Builds to `./raylib_showcase` (2,745,552 B arm64 Mach-O). This is the last user-facing deliverable before Phase 73-04 integration tests.
- **Constructor sugar adopted in production example** — Color.rgb / Vector2.of / Vector3.of / Rectangle.of from Plan 73-02 now have a live consumer in `examples/` (previously only referenced in raylib.iron stubs + smoke-file audits). API-03 sugar is end-to-end validated.
- **12-category coverage verified via reproducible grep** — per-category regex + count ≥1 assertion passes for WIN/INPUT/DRAW2D/COLL/TEX/TEXT/AUDIO/DRAW3D/MODEL/SHADER/MATH/FILE.
- **Pure-superset guard holds** — pong = 2,745,656 B (identical to Plan 73-02 close; no delta because 73-03 adds only a new example file + .gitignore entries, touching zero shared code). Within ±5% tolerance band.
- **Post-alpha residuals unchanged** — no new items added to deferred-items.md. D3 (emsdk) and D5 (CameraProjection) both already documented.

## Task Commits

Each task was committed atomically and pushed to `origin/feat/v2-raylib-milestone`:

1. **Task 1: raylib_showcase.iron + .gitignore** — `33de5f7` (feat) — examples/raylib_showcase/raylib_showcase.iron (new, 161 lines) + .gitignore (+2 lines)
2. **Task 2: SUMMARY + checkpoint** — plan metadata commit (pending)

## Files Created/Modified

### Created
- `examples/raylib_showcase/raylib_showcase.iron` — 161-line canonical single-file showcase. Structure: header comment block → `import raylib` → `func main() -> Int32` → window/audio init → TEX procedural → MODEL load-with-fallback → SHADER load → MATH/FILE pre-loop → main loop with 12-category tagged sections → cleanup (reverse load order).
- `.planning/phases/73-idiomatic-api-polish-showcase-integration-tests/73-03-SUMMARY.md` — this file.

### Modified
- `.gitignore` — +2 lines inserted after `/post_fx.c` (line 38): `/raylib_showcase` + `/raylib_showcase.c`. Root-anchored form matches `/rotating_cube` / `/post_fx` / `/model_viewer` convention.

## 12-Category Coverage Map

Column-0 `-- ── <CAT>: ───` tag per category; per-category reproducible grep + exact API call site.

| Category | Tag                                                            | Exact API call                                                                  | Line(s) | Purpose                               |
|----------|----------------------------------------------------------------|---------------------------------------------------------------------------------|---------|---------------------------------------|
| WIN      | `-- ── WIN: Window + target-fps lifecycle ───`                 | `Window.init(Int32(800), Int32(600), "...")` + `Window.set_target_fps(Int32(60))` | 48–49   | Init + fps cap                        |
| AUDIO    | `-- ── AUDIO: Audio init + Sound.load from ... bounce.wav ───` | `Audio.init()` + `Sound.load("tests/assets/bounce.wav")` + `Sound.play(blip)` in loop | 52–54, 91 | Vendored wav load + SPACE-gated play |
| TEX      | `-- ── TEX: procedural checker via Image.checked → to_texture ───` | `Image.checked(...)` + `Image.to_texture(checker_img)` + `Image.unload`         | 58–62   | Procedural 64x64 red/blue checker     |
| MODEL    | `-- ── MODEL: Model.load with procedural-cube fallback ───`    | `Model.load("tests/assets/models/cube.obj")` + `Model.from_mesh(Mesh.cube(...))` fallback + `Model.bounding_box` | 65–70   | Asset load with is_valid guard        |
| SHADER   | `-- ── SHADER: Shader.load grayscale.fs (default VS) ───`      | `Shader.load("", "tests/assets/shaders/grayscale.fs")`                          | 73      | Default VS + grayscale FS             |
| MATH     | `-- ── MATH: raymath Vector3.normalize pre-loop ───`           | `Vector3.normalize(Vector3.of(0.0, 0.0, 1.0))`                                  | 76      | Pre-loop idempotent eval              |
| FILE     | `-- ── FILE: Files.exists asset probe pre-loop ───`            | `Files.exists("tests/assets/bounce.wav")`                                       | 79      | Filesystem probe                      |
| INPUT    | `-- ── INPUT: Keyboard.is_pressed gated SPACE → Sound.play ───` | `Keyboard.is_pressed(KeyboardKey.SPACE)`                                        | 90      | SPACE-gated audio trigger             |
| COLL     | `-- ── COLL: Rectangle.collides checks ... each frame ───`     | `Rectangle.collides(r1, r2)`                                                    | 96      | Two overlapping rects                 |
| DRAW2D   | `-- ── DRAW2D: Draw.begin/clear/rectangle primitives ───`      | `Draw.begin` / `Draw.clear(RAYWHITE)` / `Draw.rectangle` x2 + `Texture.draw`    | 102–106 | 2D primitives + texture draw          |
| DRAW3D   | `-- ── DRAW3D: Camera3D mode + Model.draw + grid wrapping ───` | `Draw.begin_mode_3d(cam)` + `Draw.grid(Int32(10), Float32(1.0))` + `Draw.end_mode_3d` | 109–113 | 3D pipeline entry + grid              |
| TEXT     | `-- ── TEXT: Draw.text status line ───`                        | `Draw.text("...", x, y, fontSize, DARKGRAY)` ×3 (status + diagnostics)          | 123–130 | Implicit Font.default usage           |

**Reproducible grep verification (all counts ≥1):**

```bash
grep -c "^-- ── " examples/raylib_showcase/raylib_showcase.iron                            # 12
grep -cE "Window\.init|Window\.should_close|Window\.close" examples/raylib_showcase/raylib_showcase.iron   # 3
grep -cE "Keyboard\.is_|Mouse\.is_" examples/raylib_showcase/raylib_showcase.iron                           # 2
grep -cE "Draw\.begin|Draw\.rectangle|Draw\.end" examples/raylib_showcase/raylib_showcase.iron             # 9
grep -cE "check_collision|\.collides" examples/raylib_showcase/raylib_showcase.iron                         # 2
grep -cE "Image\.checked|Image\.load|Texture\.load|\.to_texture" examples/raylib_showcase/raylib_showcase.iron  # 4
grep -c "Draw\.text" examples/raylib_showcase/raylib_showcase.iron                                          # 5
grep -cE "Audio\.init|Sound\.load|Sound\.play" examples/raylib_showcase/raylib_showcase.iron               # 5
grep -cE "Draw\.begin_mode_3d|Draw\.grid|Draw\.cube|Draw\.end_mode_3d" examples/raylib_showcase/raylib_showcase.iron  # 3
grep -cE "Model\.load|Model\.draw" examples/raylib_showcase/raylib_showcase.iron                            # 5
grep -cE "Shader\.load|Draw\.begin_shader_mode" examples/raylib_showcase/raylib_showcase.iron              # 3
grep -cE "\.normalize|\.length|\.dot_product|Vector3\.|Vector2\." examples/raylib_showcase/raylib_showcase.iron  # 7
grep -cE "Files\.exists|Files\.load" examples/raylib_showcase/raylib_showcase.iron                          # 2
```

All 12 per-category greps return ≥1 — full 12-category coverage verified.

## Build Artifacts

### Native Build (GREEN)

```bash
$ ./build/ironc build examples/raylib_showcase/raylib_showcase.iron
Built: raylib_showcase
$ stat -f%z ./raylib_showcase
2745552
```

- **Target:** arm64 Mach-O (darwin / macOS)
- **Binary size:** 2,745,552 B (≈ 2.68 MB)
- **Delta vs pong:** −104 B (raylib_showcase uses fewer raylib call sites than pong which is a fully implemented game; stdlib objects dominate the binary size, so the −104 B delta reflects DCE difference on non-shared code paths)
- **Exit code:** 0

### Web Build (DEFERRED — D3 residual)

```bash
$ ./build/ironc build --target=web examples/raylib_showcase/raylib_showcase.iron
error: emcc not found in PATH
...
```

**Root cause:** Emscripten toolchain not installed in current execution environment. This is the same D3 limitation documented in Plan 73-01 deferred-items.md and carried forward in Plan 73-02. No code-level concern — the Plan 73-03 source change is pure Iron + uses only Iron-side stubs already proven on web via Plan 60-08's `tests/integration/web/hello_raylib.iron` + Plan 71-02's `examples/post_fx/post_fx.iron` web build. Web build of raylib_showcase should succeed unchanged once `emsdk install 4.0.23 && emsdk activate 4.0.23 && source emsdk_env.sh` runs in the execution environment.

**Unblock condition:** emsdk 4.0.23 installed in the Phase 73-04 execution environment. Phase 73-04's 12-integration-test × 2-target matrix requires emsdk regardless, so that's the natural checkpoint for web-parity validation of raylib_showcase.

## Pong Byte-Size Delta Table

| Consumer        | Plan 73-02 baseline | Plan 73-03 final | Delta | Tolerance (±5%)         | Status |
|-----------------|---------------------|------------------|-------|-------------------------|--------|
| `./pong`        | 2,745,656 B         | 2,745,656 B      | 0 B   | 2,606,792..2,881,192    | GREEN  |
| `./raylib_showcase` | — (new)        | 2,745,552 B      | n/a   | —                       | new    |

Zero-byte delta on pong — expected, because Plan 73-03 touches only a new example file + 2 .gitignore lines, zero shared code. Pure-superset guarantee trivially holds.

## Deviations from Plan

### Rule-3 deviations (blocking issues, auto-fixed)

**1. [Rule 3 - Blocking] Renamed showcase.iron → raylib_showcase.iron**
- **Found during:** Task 1 Step 3 (after first native build)
- **Issue:** Plan text specified `examples/raylib_showcase/showcase.iron` as the source path, but ironc derives the output binary name from the `.iron` file's basename (not the directory). That produced `./showcase` instead of the `./raylib_showcase` binary required by `<acceptance_criteria>` success Step 3 + the `.gitignore` entry.
- **Fix:** Renamed source file to `examples/raylib_showcase/raylib_showcase.iron` so the directory basename, `.iron` file basename, and produced binary name all align. This matches `rotating_cube/rotating_cube.iron` / `model_viewer/model_viewer.iron` / `post_fx/post_fx.iron` — the established Phase 69-71 showcase naming convention.
- **Files modified:** `examples/raylib_showcase/raylib_showcase.iron` (renamed from showcase.iron + internal header comment updated with naming note).
- **Commit:** `33de5f7` (Task 1 commit — rename captured as part of initial add).

### Rule-1 deviations (spec-correctness fix, auto-fixed)

**2. [Rule 1 - Bug] .gitignore entry uses `/raylib_showcase` (root-anchored) instead of `raylib_showcase` (unanchored)**
- **Found during:** Task 1 Step 5 (.gitignore update)
- **Issue:** Plan text's `grep -q "^raylib_showcase$" .gitignore || echo "raylib_showcase" >> .gitignore` appends an unanchored `raylib_showcase` entry. gitignore rules: an unanchored pattern matches any file/directory with that basename at any depth. The `examples/raylib_showcase/` directory itself has that basename — unanchored form would silently prevent `git add` from tracking the directory. The `.gitignore` has `!tests/` re-include but NO `!examples/` re-include, so this risk is real.
- **Fix:** Used `/raylib_showcase` + `/raylib_showcase.c` (root-anchored, inserted after line 37 `/post_fx.c`). Matches `/rotating_cube`, `/post_fx`, `/model_viewer` convention. Prevents accidental directory shadowing.
- **Verification:** `git check-ignore -v raylib_showcase` returns `.gitignore:39:/raylib_showcase raylib_showcase` (root-only); `git status --short examples/raylib_showcase/` shows `?? examples/raylib_showcase/` (directory tracked, only binary ignored).
- **Files modified:** `.gitignore` (+2 lines: `/raylib_showcase` + `/raylib_showcase.c`).
- **Commit:** `33de5f7` (Task 1 commit).

### Plan-level deviation (documentation, not code)

**3. [Doc] Plan suggested `Shapes.check_collision_rec_rec` for COLL category — used `Rectangle.collides` instead**
- **Found during:** Task 1 Step 2 (pre-write API surface check)
- **Issue:** Plan `<interfaces>` example uses `Shapes.check_collision_rec_rec(r1, r2)`. Grep against `src/stdlib/raylib.iron` for `^func Shapes\.` returns 0 — the `Shapes` namespace doesn't exist. The COLL API is `Rectangle.collides(rect, other)` (Phase 64-01 receiver-first form) + `Collision.circles` / `Collision.lines` / etc. for non-rectangle collision.
- **Fix:** Used `Rectangle.collides(r1, r2)` — Phase 64-01's idiomatic form for rect-rect collision. Functionally equivalent to the non-existent `Shapes.check_collision_rec_rec`.
- **Files modified:** `examples/raylib_showcase/raylib_showcase.iron` line 96 (COLL section).
- **Commit:** `33de5f7` (Task 1 commit).

## Authentication Gates

None. No auth-requiring services invoked by this plan.

## Issues Encountered

- **Web target build skipped** — `./build/ironc build --target=web ...` exits 1 with "emcc not found in PATH". Same D3 environment limitation documented in Plan 73-01 + Plan 73-02. Native build GREEN. Per autonomous-mode hint in the executor prompt, native-build-exit-0 + pong-regression-GREEN is sufficient for Phase 73-03 acceptance; web-parity validation is deferred to Phase 73-04 which requires emsdk regardless.

## User Setup Required

None — Plan 73-03 added no external services, no env vars, no account-dependent features. Phase 73-04 will inherit Plan 73-01's emsdk deferral (D3); that's documented in `deferred-items.md`.

## Residuals Forwarded to Phase 74+

None new. The 6 post-alpha deferrals tracked in `deferred-items.md` (D1..D7 minus D2 which is raylib-vendor-bump specific) remain unchanged. Plan 73-03 introduced zero new deferrals — the showcase consumes the API surface as shipped through Plan 73-02 without requiring any new workarounds beyond the pre-documented Int32(0) CameraProjection one (D5).

## Next Phase Readiness

- **Plan 73-04 integration tests (API-11/12/13) unblocked at native target.** Showcase proves the 12-category API surface is live and idiomatically consumable. Integration tests just need the same 12 categories exercised in a compile-only per-file scheme under `tests/integration/raylib/<CATEGORY>/`.
- **Web-parity work for Plan 73-04** still needs emsdk-equipped execution environment (D3). Source changes from Plan 73-03 are pure Iron + use only Iron-side API surface — no target-specific code introduced.
- **v2.0.0-alpha milestone posture:** ON TRACK. 3 of 4 Phase 73 plans closed. Plan 73-04 is the last remaining plan before milestone declaration. API-10 closed by this plan — only API-11/12/13 remain (all Plan 73-04 scope).

## Self-Check

All claimed files exist on disk:
- `examples/raylib_showcase/raylib_showcase.iron` — FOUND (161 lines, 12 column-0 tags)
- `.gitignore` — MODIFIED (+2 lines: `/raylib_showcase` + `/raylib_showcase.c` root-anchored)
- `.planning/phases/73-idiomatic-api-polish-showcase-integration-tests/73-03-SUMMARY.md` — FOUND (this file)

Claimed commits on `feat/v2-raylib-milestone`:
- `33de5f7` — Task 1 showcase + .gitignore (feat) — pushed to origin

All acceptance criteria verified:
- `wc -l examples/raylib_showcase/raylib_showcase.iron` returns 161 (≥80) ✓
- `grep -c "^-- ── " examples/raylib_showcase/raylib_showcase.iron` returns 12 (≥12 required) ✓
- `grep -cE "Window\.init|Keyboard\.is_|Draw\.begin|\.collides|Image\.checked|Draw\.text|Audio\.init|Draw\.begin_mode_3d|Model\.load|Shader\.load|\.normalize|Files\.exists" examples/raylib_showcase/raylib_showcase.iron` returns 27 (≥12 required) ✓
- `./build/ironc build examples/raylib_showcase/raylib_showcase.iron` exits 0; binary 2,745,552 B ✓
- `./build/ironc build --target=web examples/raylib_showcase/raylib_showcase.iron` exits 1 (emcc not in PATH) — DEFERRED per D3, per autonomous-mode hint in executor prompt ⚠
- `git check-ignore -v raylib_showcase` hits `.gitignore:39:/raylib_showcase` — binary ignored, directory tracked ✓
- `./build/ironc build examples/pong/pong.iron` exits 0; size 2,745,656 B (in range 2,606,792..2,881,192) ✓
- Human reviewer: AUTO-APPROVED per autonomous-mode hint (native build exit 0 + pong regression GREEN — both conditions GREEN)

## Self-Check: PASSED

---
*Phase: 73-idiomatic-api-polish-showcase-integration-tests*
*Completed: 2026-04-18*
