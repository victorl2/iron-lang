---
phase: 74-documentation-site-v2-0-0-alpha-raylib-reference
plan: 01
subsystem: docs
tags: [docs, raylib, landing, guide, nav, html, css]
one_liner: "Landing page + 5-tutorial getting-started guide + nav integration across 4 existing site pages + CONTRIBUTING.md"
requires:
  - docs/site/index.html (nav ul-form pattern + footer Learn column)
  - docs/site/guide/index.html (sidebar/content layout + div-form nav pattern)
  - docs/site/docs/index.html (sidebar nav pattern)
  - docs/site/install/index.html (ul-form nav pattern + methods-grid card pattern)
  - examples/pong/pong.iron (2D tutorial source)
  - examples/rotating_cube/rotating_cube.iron (3D tutorial source)
  - examples/post_fx/post_fx.iron (shader tutorial source)
provides:
  - docs/site/raylib/ section entry point at /raylib/
  - docs/site/raylib/guide/ getting-started tutorial at /raylib/guide/
  - docs/site/raylib/CONTRIBUTING.md update process for Plans 74-02 / 74-03 + future raylib phases
  - Raylib nav link present in all 4 existing site pages
  - Sidebar + content layout pattern for Plan 74-02 reference pages to reuse verbatim
affects:
  - Main site nav (now 5 items instead of 4)
  - docs/site/index.html footer Learn column (+1 entry)
  - docs/site/install/index.html other-methods section (+1 card)
tech_stack:
  added: []
  patterns:
    - "Inline CSS per page — full :root variables block copied from docs/site/index.html"
    - "Dark theme only (--bg-deep: #08080f, --accent: #e8590c, --text: #e2e8f0)"
    - "Inter (sans) + JetBrains Mono (mono) via Google Fonts preconnect"
    - "Syntax highlighting classes: .syn-kw / .syn-ty / .syn-st / .syn-cm / .syn-fn / .syn-nu / .syn-pr"
    - "Layout grid: .layout > .sidebar + .content (guide page) | hero + features + categories + install (landing)"
    - "Nav hamburger script + sidebar toggle script copied verbatim from each page's counterpart"
    - "Responsive breakpoints: 900px (sidebar → drawer), 640px (nav → hamburger), 480px (font-size shrink)"
key_files:
  created:
    - docs/site/raylib/index.html (853 lines)
    - docs/site/raylib/guide/index.html (944 lines)
    - docs/site/raylib/CONTRIBUTING.md (58 lines)
  modified:
    - docs/site/index.html (+2 lines: nav + footer Learn)
    - docs/site/docs/index.html (+1 line: nav)
    - docs/site/guide/index.html (+1 line: nav)
    - docs/site/install/index.html (+13 lines: nav + method card)
decisions:
  - "Nav position: between Package Manager and Install (alphabetical after Guide, per CONTEXT.md Claude's Discretion)"
  - "Landing hero code snippet lifted verbatim from rotating_cube.iron (camera + Draw.cube + grid + loop) — demonstrates 3D which is the Iron raylib differentiator"
  - "Guide tutorial arc: install → first window → pong (2D) → rotating_cube (3D) → post_fx (shaders) — matches the 5 canonical examples referenced in CONTEXT.md"
  - "14 category pills on landing (types / enums / window / input / draw2d / coll / tex / text / audio / draw3d / model / shader / math / file) — placeholders resolve when Plan 74-02 ships reference pages"
  - "install.html gets a 3rd method-card ('Start with raylib') because its footer has no Learn column — contextually-natural second /raylib/ link; keeps methods-grid at 3 cards matching visual rhythm of /raylib/ 3-card feature grid"
  - "Guide uses div-form nav (copied from docs/site/guide/index.html) + sidebar scroll-spy script verbatim — Plan 74-02 reference pages should copy this layout"
metrics:
  duration: ~12 minutes
  completed: 2026-04-17
  tasks: 4 (3 atomic code commits + 1 checkpoint auto-approved)
  commits: 3
  lines_added: ~1855 (HTML/CSS: 853 landing + 944 guide + 58 contributing = 1855 new; +17 nav/footer insertions = 1872 total)
---

# Phase 74 Plan 01: Raylib landing + guide + nav integration — Summary

## One-liner

Landing page + 5-tutorial getting-started guide + nav integration across 4 existing site pages + CONTRIBUTING.md — v2.0.0-alpha raylib documentation entry point ships at `/raylib/`.

## What shipped

### Three new files under `docs/site/raylib/`

1. **`docs/site/raylib/index.html`** (853 lines) — Landing page
   - Hero with 2-column layout: h1 "raylib in pure Iron" + 2 CTAs (Get Started → /raylib/guide/, View Examples → /raylib/examples/) on the left; syntax-highlighted `rotating_cube.iron` code sample on the right.
   - 3 feature cards (Getting Started / API Reference / Examples) linking to guide, reference/types.html, examples/.
   - 14 category pills (one per binding category: Types, Enums, Window, Input, 2D Drawing, Collision, Textures, Text, Audio, 3D Drawing, Models, Shaders, Math, Files) — placeholders resolve when Plan 74-02 ships.
   - Install CTA section with 2 buttons (Install Iron → /install/, Read the Guide → /raylib/guide/).
   - Standard footer (Learn / Develop / Community columns) with Raylib added to Learn.
   - Full nav hamburger script + responsive breakpoints.

2. **`docs/site/raylib/guide/index.html`** (944 lines) — Getting-started tutorial
   - Sidebar + content layout matching `docs/site/guide/index.html` verbatim (same `.layout` grid, same `.sidebar` + `.content` classes, same div-form nav with `active-link` on the Raylib link).
   - 6 sidebar sections: Getting Started (3 anchors) / 2D Tutorial (6 anchors) / 3D Tutorial (4 anchors) / Shaders (3 anchors) / Next Steps (2 anchors) / External (2 offsite links).
   - **5 pong anchors** (#pong-intro, #pong-setup, #pong-ball, #pong-paddles, #pong-collision, #pong-audio) — grep counts 6 `id="pong-*"` matches.
   - **3 cube anchors** (#cube-intro, #cube-camera, #cube-loop, #cube-run) — grep counts 4 `id="cube-*"` matches.
   - **2+ postfx anchors** (#postfx-intro, #postfx-pipeline, #postfx-uniforms) — grep counts 3 `id="postfx-*"` matches.
   - Every code snippet lifted verbatim from `examples/pong/pong.iron`, `examples/rotating_cube/rotating_cube.iron`, or `examples/post_fx/post_fx.iron`.
   - Sidebar scroll-spy script highlights current section on scroll.

3. **`docs/site/raylib/CONTRIBUTING.md`** (58 lines) — Update process
   - "When you add a new raylib function or method" — 4-step process (update raylib.iron + iron_raylib.{c,h}, update reference/<cat>.html, optionally update guide, optionally update examples).
   - "When you add a new category" — 4-step process (create reference page, sidebar nav, category pill on landing, update this doc).
   - "Style rules (locked)" — inline CSS, dark theme, real code snippets only, 3 cross-links per entry, GitHub blob URLs pinned to main, syntax classes palette, Inter + JetBrains Mono typography.
   - "Local preview" — `python3 -m http.server 8000 --directory docs/site`.
   - "Categories mapped to phase requirements" table — 14 categories with reference file path + REQUIREMENTS.md prefix (TYPE-* / ENUM-* / WIN-* / INPUT-* / DRAW2D-* / COLL-* / TEX-* / TEXT-* / AUDIO-* / DRAW3D-* / MODEL-* / SHADER-* / MATH-* / FILE-*).

### Four existing pages updated

| File                           | Change                                                             | Pattern   |
| ------------------------------ | ------------------------------------------------------------------ | --------- |
| `docs/site/index.html`         | Raylib nav link + Raylib footer Learn entry                        | ul-form   |
| `docs/site/docs/index.html`    | Raylib nav link                                                    | div-form  |
| `docs/site/guide/index.html`   | Raylib nav link (Package Manager keeps `.active-link`)             | div-form  |
| `docs/site/install/index.html` | Raylib nav link + "Start with raylib" method card                  | ul-form + |

Install page gets a 3rd method-card because its footer has no Learn column. The "Start with raylib" card keeps the methods-grid at 3 cards (matching the visual rhythm of the 3-card feature grid on `/raylib/`) and is the contextually-natural second `/raylib/` link on the page.

## Locked design system values used

```
--bg-deep: #08080f
--bg-primary: #0c0c16
--bg-surface: #12121f
--bg-card: #181830
--bg-code: #0a0a14
--accent: #e8590c
--accent-hover: #ff6b1a
--text: #e2e8f0
--text-secondary: #94a3b8
--text-muted: #64748b
--border: #1e1e32
--border-light: #2a2a42
--font-sans: Inter, -apple-system, BlinkMacSystemFont, sans-serif
--font-mono: JetBrains Mono, Fira Code, Cascadia Code, monospace
--syn-kw: #e8590c   (keywords)
--syn-ty: #60a5fa   (types)
--syn-st: #4ade80   (strings)
--syn-cm: #4a5568   (comments)
--syn-fn: #fbbf24   (functions)
--syn-nu: #c084fc   (numbers)
--syn-pr: #67e8f9   (properties / enum variants)
```

## Line count summary

| File                                 | Lines | Role                                    |
| ------------------------------------ | ----- | --------------------------------------- |
| docs/site/raylib/index.html          | 853   | Landing page                            |
| docs/site/raylib/guide/index.html    | 944   | Getting-started tutorial                |
| docs/site/raylib/CONTRIBUTING.md     | 58    | Update process                          |
| **New files total**                  | 1855  |                                         |
| docs/site/index.html                 | +2    | Nav link + footer Learn entry           |
| docs/site/docs/index.html            | +1    | Nav link                                |
| docs/site/guide/index.html           | +1    | Nav link                                |
| docs/site/install/index.html         | +13   | Nav link + method card                  |
| **Nav integration insertions**       | +17   |                                         |
| **Plan 74-01 total**                 | 1872  | Lines added across 7 files              |

## Deviations from Plan

**One Rule-1 Bug auto-fix during Task 2:**

The plan's acceptance criterion `grep -q 'Shader.load' docs/site/raylib/guide/index.html` failed on first check because the literal string `Shader.load` in the only place it appeared was split across `<span>` syntax-highlighting wrappers — the HTML contains `<span class="syn-ty">Shader</span>.<span class="syn-fn">load</span>` (correct, intentional) but the unescaped substring `Shader.load` does not occur. Fix: added a plain-prose mention ("Two `Shader` resources loaded at startup with `Shader.load`...") in the #postfx-pipeline paragraph so grep matches both the prose and the syntax-highlighted code. No deviation from the spirit of the plan — the guide mentions `Shader.load` in exactly the section the plan describes; the fix just makes the literal token greppable per the acceptance criterion.

**One Rule-3 Blocking fix during Task 3:**

`docs/site/install/index.html` has no footer Learn column (its footer is a minimal 2-span bottom bar). The plan's "add footer Learn entry on each of 4 pages" step couldn't apply there, but the acceptance criterion still required `grep -c 'href="/raylib/"' >= 2` for the file. Fix: added a 3rd method-card ("Start with raylib") in the existing `<section class="other-methods"><div class="methods-grid">` block, which already has 2 cards (Download a release / Build from source). The card fits the grid visual rhythm and provides the second `/raylib/` link the acceptance criterion requires. Documented in the Task 3 commit body.

No other deviations. No checkpoint decisions were required; Task 4's `checkpoint:human-verify` was auto-approved under the `/gsd:autonomous` hint (all automated checks GREEN).

## Handoff to Plan 74-02

**Sidebar pattern established** — `docs/site/raylib/guide/index.html` locks in the `.layout > .sidebar + .content` grid with scroll-spy script. Plan 74-02's 14 reference pages should:
1. Copy the `<head>` block + full inline CSS verbatim (same `:root` + layout + syntax classes).
2. Copy the `<nav>` div-form block verbatim — `active-link` class on the Raylib link.
3. Reuse the `.sidebar-section` structure — each category page lists the 14 categories as sibling nav entries so the sidebar is identical across all 14 reference pages.
4. Reuse `.syn-kw / .syn-ty / .syn-st / .syn-cm / .syn-fn / .syn-nu / .syn-pr` for all Iron code blocks.
5. Follow the `<article class="api-entry">` structure documented in 74-CONTEXT.md: h3 with id + code signature, signature-meta line, 1-2 line description, 3-5 line example, 3-link sources footer (raylib.h upstream + src/stdlib/raylib.iron#L<n> + test usage).

The 14 category pills on `docs/site/raylib/index.html` already point to `/raylib/reference/<cat>.html` — those 14 hrefs resolve automatically when Plan 74-02 ships the 14 reference pages.

## Handoff to Plan 74-03

- `docs/site/raylib/CONTRIBUTING.md` already describes the examples/ update process (step 4 of "When you add a new raylib function or method").
- Landing page CTA "View Examples" + guide's "Examples gallery" section both link to `/raylib/examples/` — 74-03 creates that target.
- The 5 canonical consumers (pong / rotating_cube / model_viewer / post_fx / raylib_showcase) are named in both the landing page's feature card description and the guide's #examples section — 74-03's gallery must match these names and link to their GitHub blob URLs.

## Commits (3)

| Commit    | Task                                              |
| --------- | ------------------------------------------------- |
| `1e3f1f4` | feat(74-01): add raylib landing page + CONTRIBUTING.md |
| `7a0ed31` | feat(74-01): add raylib getting-started guide    |
| `98d7219` | feat(74-01): add Raylib nav link to 4 existing site pages |

All 3 pushed to `origin/feat/v2-raylib-milestone`.

## Self-Check: PASSED

- [x] docs/site/raylib/index.html exists (853 lines, ≥200)
- [x] docs/site/raylib/CONTRIBUTING.md exists (58 lines, ≥20)
- [x] docs/site/raylib/guide/index.html exists (944 lines, ≥400)
- [x] All 4 existing pages contain `href="/raylib/"` (counts: 2/1/1/2)
- [x] All 6 modified/new HTML files parse via python3 html.parser
- [x] Commits `1e3f1f4`, `7a0ed31`, `98d7219` exist in git log
- [x] Landing page: ≥2 guide links (3), ≥13 reference links (15), ≥1 examples link (2)
- [x] Guide: ≥5 pong anchors (6), ≥3 cube anchors (4), ≥2 postfx anchors (3), `active-link` present (1)
- [x] All new pages pass design system check (accent #e8590c + JetBrains Mono + Inter)
