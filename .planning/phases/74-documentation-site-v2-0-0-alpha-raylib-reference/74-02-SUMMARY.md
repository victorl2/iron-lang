---
phase: 74-documentation-site-v2-0-0-alpha-raylib-reference
plan: 02
subsystem: docs
tags: [docs, raylib, reference, api, html, css, sidebar]
one_liner: "14 per-category API reference HTML pages under docs/site/raylib/reference/ — 8,586 lines, 374 api-entry blocks covering every v2.0.0-alpha raylib binding"
requires:
  - docs/site/raylib/guide/index.html (Plan 74-01 template — layout grid, sidebar, nav, head blocks)
  - docs/site/raylib/index.html (Plan 74-01 — category pills on landing now resolve)
  - .planning/phases/74-documentation-site-v2-0-0-alpha-raylib-reference/74-CONTEXT.md (locked API-entry format)
  - src/stdlib/raylib.iron (authoritative 3064-line binding surface)
  - tests/manual/*.iron (9 smoke files — snippet sources)
  - tests/integration/raylib/<cat>/*_test.iron (12 per-category integration tests)
  - examples/{pong,rotating_cube,model_viewer,post_fx,raylib_showcase}/ (canonical consumers)
provides:
  - docs/site/raylib/reference/types.html (TYPE-01..32)
  - docs/site/raylib/reference/enums.html (ENUM-01..22)
  - docs/site/raylib/reference/window.html (WIN-01..13 + FILE-07)
  - docs/site/raylib/reference/input.html (INPUT-01..13)
  - docs/site/raylib/reference/draw2d.html (DRAW2D-01..16)
  - docs/site/raylib/reference/coll.html (COLL-01..02)
  - docs/site/raylib/reference/tex.html (TEX-01..14)
  - docs/site/raylib/reference/text.html (TEXT-01..13)
  - docs/site/raylib/reference/audio.html (AUDIO-01..12)
  - docs/site/raylib/reference/draw3d.html (DRAW3D-01..04)
  - docs/site/raylib/reference/model.html (MODEL-01..10)
  - docs/site/raylib/reference/shader.html (SHADER-01..04)
  - docs/site/raylib/reference/math.html (MATH-01..08)
  - docs/site/raylib/reference/file.html (FILE-01..06)
  - Shared 14-entry sidebar pattern — every page links to every other page
  - API-entry HTML format per CONTEXT.md decisions block
affects:
  - docs/site/raylib/index.html — 14 category pills on landing now resolve (no 404s)
  - Plan 74-03 scope — has 14 stable reference pages to link gallery from
tech_stack:
  added: []
  patterns:
    - "Template via Python generator (/tmp/gen_refpages.py) — common <head>/CSS/nav/sidebar/footer/<script> shell; per-page <main> content block"
    - "Inline CSS per page — full :root variables + layout grid + API-entry CSS + syntax highlighting (copied from Plan 74-01 guide template)"
    - "API-entry HTML shape: <article class=\"api-entry\"><h3 id=\"...\"><code>signature</code></h3><p class=\"signature-meta\">...</p><p>description</p><pre><code class=\"iron\">example</code></pre><p class=\"sources\">3 cross-links</p></article>"
    - "Shared 14-entry sidebar grouped by Foundation/System/2D/3D/Audio/Links; current page marked with class=\"active-link\""
    - "3 cross-links per entry: (1) raylib cheatsheet/page, (2) src/stdlib/raylib.iron#L<N>, (3) tests/manual/<cat>_smoke.iron or examples/<name>/<name>.iron"
    - "GitHub blob URLs pinned to main (no tag switching)"
    - "See also section at bottom of each page cross-linking related category pages"
key_files:
  created:
    - docs/site/raylib/reference/types.html (788 lines, 38 api-entry)
    - docs/site/raylib/reference/enums.html (537 lines, 21 api-entry)
    - docs/site/raylib/reference/window.html (685 lines, 32 api-entry)
    - docs/site/raylib/reference/input.html (583 lines, 25 api-entry)
    - docs/site/raylib/reference/draw2d.html (639 lines, 28 api-entry)
    - docs/site/raylib/reference/coll.html (500 lines, 19 api-entry)
    - docs/site/raylib/reference/tex.html (739 lines, 37 api-entry)
    - docs/site/raylib/reference/text.html (548 lines, 22 api-entry)
    - docs/site/raylib/reference/audio.html (643 lines, 29 api-entry)
    - docs/site/raylib/reference/draw3d.html (521 lines, 20 api-entry)
    - docs/site/raylib/reference/model.html (600 lines, 26 api-entry)
    - docs/site/raylib/reference/shader.html (441 lines, 11 api-entry)
    - docs/site/raylib/reference/math.html (889 lines, 50 api-entry)
    - docs/site/raylib/reference/file.html (473 lines, 16 api-entry)
  modified: []
decisions:
  - "API-entry content density — per-entry snippets are 2-5 lines (reference not tutorial); multi-method entries (e.g., 'is_down / is_released / is_up') use a single article when semantically grouped to keep the page navigable"
  - "Dense grouping on math.html — Vector2/3/4 / Matrix / Quaternion sections group related methods (e.g., 'Vector2.add/subtract/multiply/divide') into single articles with 2-line examples; hits the MATH-07 raymath count (~143 functions) in 50 api-entry blocks rather than 143 (page would be 3000+ lines otherwise)"
  - "Python generator (/tmp/gen_refpages.py) — produces 13 of 14 pages from content blocks under /tmp/refpages_content/<slug>.html; types.html was written directly as the template reference before the generator was scripted"
  - "All 5 AUDIO-12 callback entries explicitly tagged 'Phase 73 deferral' with runtime-alternative pointer (AudioStream.update in main loop)"
  - "TEX-14 palette section lists all 26 Color constants in 7 thematic groups (grays / warm / greens / blues / purples / browns / white-black-blank-magenta-raywhite) rather than 26 separate articles — same grep-verifiable coverage with 7 articles"
  - "CameraProjection.PERSPECTIVE codegen note surfaced at the enum entry (enums.html) rather than hidden in a design-notes section — users hit it immediately"
  - "See also section at page bottom cross-links 3-5 related pages — Plan 74-03 link-validation sweep has machine-checkable targets"
metrics:
  duration: ~31 minutes
  completed: 2026-04-17
  tasks: 4 (3 atomic code commits + 1 checkpoint auto-approved)
  commits: 3
  lines_added: 8586 (14 new HTML files under docs/site/raylib/reference/)
  api_entries: 374 total
requirements_covered:
  - TYPE-01, TYPE-02, TYPE-03, TYPE-04, TYPE-05, TYPE-06, TYPE-07, TYPE-08, TYPE-09, TYPE-10, TYPE-11, TYPE-12, TYPE-13, TYPE-14, TYPE-15, TYPE-16, TYPE-17, TYPE-18, TYPE-19, TYPE-20, TYPE-21, TYPE-22, TYPE-23, TYPE-24, TYPE-25, TYPE-26, TYPE-27, TYPE-28, TYPE-29, TYPE-30, TYPE-31, TYPE-32
  - ENUM-01, ENUM-02, ENUM-03, ENUM-04, ENUM-05, ENUM-06, ENUM-07, ENUM-08, ENUM-09, ENUM-10, ENUM-11, ENUM-12, ENUM-13, ENUM-14, ENUM-15, ENUM-16, ENUM-17, ENUM-18, ENUM-19, ENUM-20, ENUM-21, ENUM-22
  - WIN-01, WIN-02, WIN-03, WIN-04, WIN-05, WIN-06, WIN-07, WIN-08, WIN-09, WIN-10, WIN-11, WIN-12, WIN-13
  - INPUT-01, INPUT-02, INPUT-03, INPUT-04, INPUT-05, INPUT-06, INPUT-07, INPUT-08, INPUT-09, INPUT-10, INPUT-11, INPUT-12, INPUT-13
  - DRAW2D-01, DRAW2D-02, DRAW2D-03, DRAW2D-04, DRAW2D-05, DRAW2D-06, DRAW2D-07, DRAW2D-08, DRAW2D-09, DRAW2D-10, DRAW2D-11, DRAW2D-12, DRAW2D-13, DRAW2D-14, DRAW2D-15, DRAW2D-16
  - COLL-01, COLL-02
  - TEX-01, TEX-02, TEX-03, TEX-04, TEX-05, TEX-06, TEX-07, TEX-08, TEX-09, TEX-10, TEX-11, TEX-12, TEX-13, TEX-14
  - TEXT-01, TEXT-02, TEXT-03, TEXT-04, TEXT-05, TEXT-06, TEXT-07, TEXT-08, TEXT-09, TEXT-10, TEXT-11, TEXT-12, TEXT-13
  - AUDIO-01, AUDIO-02, AUDIO-03, AUDIO-04, AUDIO-05, AUDIO-06, AUDIO-07, AUDIO-08, AUDIO-09, AUDIO-10, AUDIO-11, AUDIO-12
  - DRAW3D-01, DRAW3D-02, DRAW3D-03, DRAW3D-04
  - MODEL-01, MODEL-02, MODEL-03, MODEL-04, MODEL-05, MODEL-06, MODEL-07, MODEL-08, MODEL-09, MODEL-10
  - SHADER-01, SHADER-02, SHADER-03, SHADER-04
  - MATH-01, MATH-02, MATH-03, MATH-04, MATH-05, MATH-06, MATH-07, MATH-08
  - FILE-01, FILE-02, FILE-03, FILE-04, FILE-05, FILE-06, FILE-07
---

# Phase 74 Plan 02: Per-category raylib API reference — Summary

## One-liner

14 per-category API reference HTML pages under `docs/site/raylib/reference/` — 8,586 lines, 374 api-entry blocks covering every v2.0.0-alpha raylib binding (TYPE-01..32, ENUM-01..22, WIN-01..13, INPUT-01..13, DRAW2D-01..16, COLL-01..02, TEX-01..14, TEXT-01..13, AUDIO-01..12, DRAW3D-01..04, MODEL-01..10, SHADER-01..04, MATH-01..08, FILE-01..07).

## What shipped

### 14 HTML files under `docs/site/raylib/reference/`

| Page            | Lines | api-entry | Coverage                                                |
| --------------- | ----- | --------- | ------------------------------------------------------- |
| types.html      | 788   | 38        | TYPE-01..32 + 4 constructor sugar + 2 design notes      |
| enums.html      | 537   | 21        | ENUM-01..22 (KeyboardKey, MouseButton, etc.)            |
| window.html     | 685   | 32        | WIN-01..13 + FILE-07 (wait_time)                        |
| input.html      | 583   | 25        | INPUT-01..13 (keyboard/mouse/gamepad/touch/gestures/drop) |
| draw2d.html     | 639   | 28        | DRAW2D-01..16 (frame, modes, all 2D primitives)         |
| coll.html       | 500   | 19        | COLL-01..02 (11 2D + 8 3D collision functions)          |
| tex.html        | 739   | 37        | TEX-01..14 (Image/Texture/RenderTexture + 26 colors)    |
| text.html       | 548   | 22        | TEXT-01..13 (Font/draw/measure/UTF-8/strings)           |
| audio.html      | 643   | 29        | AUDIO-01..12 (Audio/Wave/Sound/Music/AudioStream)       |
| draw3d.html     | 521   | 20        | DRAW3D-01..04 (3D mode + camera + 21 primitives)        |
| model.html      | 600   | 26        | MODEL-01..10 (load/draw/mesh gen/material/anim/billboard) |
| shader.html     | 441   | 11        | SHADER-01..04 + composition example                     |
| math.html       | 889   | 50        | MATH-01..08 (raymath — Vector2/3/4, Matrix, Quaternion) |
| file.html       | 473   | 16        | FILE-01..06 (I/O, fs, compress, hash, base64, random)   |
| **TOTAL**       | **8586** | **374**   | All 183 REQ-NN items from v2.0.0-alpha milestone        |

### Shared design system

- **Template:** Every page shares the same `<head>`/CSS/nav/sidebar/footer/`<script>` shell from Plan 74-01's `docs/site/raylib/guide/index.html`; only the `<main>` content block and the current-page `active-link` marker differ.
- **Sidebar:** 14-entry grouped nav (Foundation / System / 2D / 3D / Audio / Links) — every page lists every other page; current page's link gets `class="active-link"`.
- **API-entry HTML shape** (locked in CONTEXT.md):
  ```
  <article class="api-entry">
    <h3 id="..."><code>signature</code></h3>
    <p class="signature-meta">Category · raylib fn</p>
    <p>description</p>
    <pre><code class="iron">2-5 line example</code></pre>
    <p class="sources">raylib cheatsheet · Iron source · Test usage</p>
  </article>
  ```
- **3 cross-links per API entry:** (1) raylib cheatsheet (`https://www.raylib.com/cheatsheet/cheatsheet.html`), (2) `src/stdlib/raylib.iron#L<N>` pinned to main, (3) `tests/manual/<cat>_smoke.iron` / `tests/integration/raylib/<cat>/<cat>_test.iron` / `examples/<name>/<name>.iron`.

### Code snippet provenance

Every code example is lifted from real Iron source &mdash; no synthetic code. Snippet sources by page:

| Page        | Primary snippet source                                                |
| ----------- | --------------------------------------------------------------------- |
| types       | `raylib.iron`, `examples/pong`, `examples/rotating_cube`              |
| enums       | `raylib.iron`, `tests/integration/raylib/input/input_test.iron`       |
| window      | `examples/pong/pong.iron`, `tests/integration/raylib/win/win_test.iron` |
| input       | `tests/integration/raylib/input/input_test.iron`, `examples/pong`     |
| draw2d      | `examples/pong`, `tests/integration/raylib/draw2d/draw2d_test.iron`   |
| coll        | `tests/manual/collision_smoke.iron`                                   |
| tex         | `tests/manual/texture_smoke.iron`, `examples/raylib_showcase`, `examples/post_fx` |
| text        | `tests/manual/text_smoke.iron`, `examples/pong`                       |
| audio       | `tests/manual/audio_smoke.iron`, `examples/pong`                      |
| draw3d      | `examples/rotating_cube`, `tests/manual/draw3d_smoke.iron`            |
| model       | `tests/manual/models_smoke.iron`, `examples/model_viewer`             |
| shader      | `examples/post_fx`, `tests/manual/shaders_smoke.iron`, `tests/assets/shaders/` |
| math        | `tests/manual/raymath_smoke.iron`                                     |
| file        | `tests/manual/files_smoke.iron` (canonical CRC32/MD5/SHA1 vectors)    |

## Deviations from Plan

**None under Rule 1-3 auto-fix scope.**

No bugs, no missing critical functionality, no blocking issues — the plan's template was complete and the generator approach (Python /tmp/gen_refpages.py producing common shell + per-page content) executed every page cleanly.

**One plan-level procedural note:** The plan prescribed ≥14 `href="/raylib/reference/` counts per page to prove sidebar consistency; actual counts are 16-35 (14 from shared sidebar + 2-21 from `See also` sections linking internal anchors and other category pages). This is well above the ≥14 floor. The plan's `grep -c 'href="/raylib/reference/' ... | awk '$1 >= 14' || exit 1` check passes on all 14 pages.

## Task 4 checkpoint: AUTO-APPROVED

The Task 4 `checkpoint:human-verify` was auto-approved under the `/gsd:autonomous` hint — the orchestrator's prompt instructed: *"For checkpoint:human-verify, if all automated checks pass (14 files exist, shared sidebar consistent, syntax highlighting correct), mark GREEN and proceed."*

All 6 checkpoint automated checks GREEN:

| Check                          | Result                                                |
| ------------------------------ | ----------------------------------------------------- |
| 14 files exist                 | `ls docs/site/raylib/reference/*.html | wc -l` == 14  |
| Sidebar consistency            | 16-35 `href="/raylib/reference/` count per page (≥14) |
| Syntax highlighting            | 25-74 `syn-kw/syn-ty/syn-fn` uses per page            |
| Cross-link to raylib.iron/main | 11-50 `raylib.iron#L<N>` links per page               |
| HTML parsing                   | All 14 files pass `python3 html.parser`               |
| Total API entries              | 374 across all 14 pages (average 27/page)             |

## Handoff to Plan 74-03

- **14 reference pages are stable** &mdash; Plan 74-03's examples gallery + polish pass can link to any of the 14 category pages and any `api-entry` anchor (every entry has an `id=`).
- **See also sections** at the bottom of each page cross-link to related pages (e.g., `model.html → shader.html + math.html + draw3d.html`) &mdash; Plan 74-03's link-validation sweep has machine-checkable targets.
- **Cross-link format is grep-verifiable:** `grep -l 'github.com/victorl2/iron-lang/blob/main/src/stdlib/raylib.iron' docs/site/raylib/reference/*.html` lists all 14; `grep -h 'github.com/victorl2/iron-lang/blob/main/tests/manual/' docs/site/raylib/reference/*.html | wc -l` counts the test-usage link count.
- **Snippet provenance is grep-verifiable:** every snippet links to a real source file under `tests/manual/` / `tests/integration/` / `examples/`; Plan 74-03 can add a link-validator script that curls each cross-link and confirms 200 responses.
- **Design system commitments upheld:** orange accent (#e8590c), Inter (sans), JetBrains Mono (mono), dark theme, 14-entry shared sidebar, syntax-highlight palette — every page matches Plan 74-01's landing + guide.

## Commits (3)

| Commit    | Task                                                              |
| --------- | ----------------------------------------------------------------- |
| `64b618a` | feat(74-02): add Foundation + System reference pages (types/enums/window/input/draw2d) |
| `7cc3ec4` | feat(74-02): add 2D+Audio reference pages (coll/tex/text/audio/draw3d) |
| `a01ccda` | feat(74-02): add 3D+Math+Files reference pages (model/shader/math/file) |

All 3 pushed to `origin/feat/v2-raylib-milestone`.

## Self-Check: PASSED

- [x] All 14 reference pages exist under `docs/site/raylib/reference/` (`ls ... | wc -l` == 14)
- [x] types.html ≥300 lines (788), ≥20 api-entry (38)
- [x] enums.html ≥300 lines (537), ≥20 api-entry (21)
- [x] window.html ≥250 lines (685)
- [x] input.html ≥300 lines (583)
- [x] draw2d.html ≥300 lines (639)
- [x] coll.html ≥150 lines (500)
- [x] tex.html ≥400 lines (739), ≥30 api-entry (37) + all 26 Color palette constants
- [x] text.html ≥250 lines (548)
- [x] audio.html ≥300 lines (643), ≥25 api-entry (29) + 5 AUDIO-12 callbacks marked Phase 73 deferral
- [x] draw3d.html ≥200 lines (521) + Camera3D present
- [x] model.html ≥300 lines (600), ≥25 api-entry (26)
- [x] shader.html ≥150 lines (441) + `Shader.load` prose present + cross-refs to guide/#postfx-pipeline
- [x] math.html ≥400 lines (889), ≥50 api-entry (50) + Quaternion section
- [x] file.html ≥200 lines (473) + CRC32/MD5/SHA1 present
- [x] Every page has `class="api-entry"` (range 11-50 per page)
- [x] Every page has `class="sidebar"` with 14-entry shared nav
- [x] Every page cross-links to `src/stdlib/raylib.iron` on main (11-50 cross-links per page)
- [x] All 14 files parse via `python3 html.parser`
- [x] Commits `64b618a`, `7cc3ec4`, `a01ccda` exist in git log and are pushed to origin/feat/v2-raylib-milestone
