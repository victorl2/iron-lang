# Contributing to raylib documentation

This directory (`docs/site/raylib/`) documents the current raylib 6.0 binding. Follow this process when the binding surface changes.

## When you add a new raylib function or method

1. Update `src/stdlib/raylib.iron` (Iron stub) and `src/stdlib/iron_raylib.c` + `.h` (C shim) per the GSD phase workflow.
2. Find the matching category page under `docs/site/raylib/reference/` (one of: types, enums, window, input, draw2d, coll, tex, text, audio, draw3d, model, shader, math, file) and append an `<article class="api-entry">` block with signature, 1-2 line description, 3-5 line example, and 3 cross-links (raylib.h upstream, Iron source line, test usage).
3. If the new function deserves a tutorial, add a section to `docs/site/raylib/guide/index.html`.
4. If it's a showcase-level feature (camera mode, shader pipeline, asset type), add a card to `docs/site/raylib/examples/index.html` and a screenshot under `docs/site/raylib/examples/screenshots/`.

## When you add a new category

1. Create `docs/site/raylib/reference/<new-category>.html` by copying an existing category page as a template.
2. Add a nav entry to the shared sidebar in all 14 reference pages.
3. Add a category pill to `docs/site/raylib/index.html`.
4. Update this CONTRIBUTING.md's category list above.

## Style rules (locked)

- **Inline CSS** per page. Do not extract an external stylesheet &mdash; matches the rest of `docs/site/`.
- **Dark theme only.** No toggle.
- **Code snippets must be real** &mdash; lifted verbatim from `examples/<name>/<name>.iron` or `tests/manual/<category>_smoke.iron`. Do not synthesize.
- **Cross-link every API entry** with 3 links: raylib.h upstream, `src/stdlib/raylib.iron#L<line>`, and a test usage.
- **GitHub blob URLs** pin to `https://github.com/victorl2/iron-lang/blob/main/` (or to a release tag when one is cut).
- **Syntax classes** reuse the shared palette: `.syn-kw` (#e8590c), `.syn-ty` (#60a5fa), `.syn-st` (#4ade80), `.syn-cm` (#4a5568), `.syn-fn` (#fbbf24), `.syn-nu` (#c084fc), `.syn-pr` (#67e8f9).
- **Typography** locked to Inter (sans) + JetBrains Mono (mono). No exceptions.

## Local preview

The `docs/site/` tree is served by GitHub Pages with no build step. Preview locally with:

```
python3 -m http.server 8000 --directory docs/site
```

Then visit `http://localhost:8000/raylib/`.

## Categories mapped to phase requirements

| Category    | File                        | Requirement prefix |
| ----------- | --------------------------- | ------------------ |
| Types       | `reference/types.html`      | `TYPE-*`           |
| Enums       | `reference/enums.html`      | `ENUM-*`           |
| Window      | `reference/window.html`     | `WIN-*`            |
| Input       | `reference/input.html`      | `INPUT-*`          |
| 2D Drawing  | `reference/draw2d.html`     | `DRAW2D-*`         |
| Collision   | `reference/coll.html`       | `COLL-*`           |
| Textures    | `reference/tex.html`        | `TEX-*`            |
| Text        | `reference/text.html`       | `TEXT-*`           |
| Audio       | `reference/audio.html`      | `AUDIO-*`          |
| 3D Drawing  | `reference/draw3d.html`     | `DRAW3D-*`         |
| Models      | `reference/model.html`      | `MODEL-*`          |
| Shaders     | `reference/shader.html`     | `SHADER-*`         |
| Math        | `reference/math.html`       | `MATH-*`           |
| Files       | `reference/file.html`       | `FILE-*`           |

The category list intentionally mirrors the 14 phase groupings in `.planning/REQUIREMENTS.md` so future phases have a fixed slot to extend.
