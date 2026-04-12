---
phase: 11
plan: 1
subsystem: web-build
tags: [web, emcc, sourcemap, output-layout, ci]
dependency_graph:
  requires: [phase-7-build-web-c, phase-10-asset-preload]
  provides: [wasm-sourcemap-debug, output-layout-complete]
  affects: [web.yml, build_web.c]
tech_stack:
  added: []
  patterns: [emcc -gsource-map flag, debug/release flag branch]
key_files:
  modified:
    - src/cli/build_web.c
    - .github/workflows/web.yml
    - .planning/REQUIREMENTS.md
    - .planning/ROADMAP.md
decisions:
  - "-gsource-map added only to debug branch; release builds omit it by design"
  - "WEB-OUT-01/02/03/05 were already satisfied by Phase 7 and Phase 10; only WEB-OUT-04 required a code change"
metrics:
  duration: "~15 minutes"
  completed: "2026-04-12"
  tasks_completed: 5
  files_modified: 4
---

# Phase 11: dist/web/ Output Layout Summary

**One-liner:** Debug web builds emit `index.wasm.map` via emcc `-gsource-map`; release builds omit it; CI asserts both invariants.

## What Was Done

Phase 11 completes the `dist/web/` output layout requirements. WEB-OUT-01 (output in `dist/web/`), WEB-OUT-02 (`index.html` naming), WEB-OUT-03 (`index.js` + `index.wasm` siblings), and WEB-OUT-05 (`mkdir_p` on fresh clone) were all already satisfied by Phase 7's `iron_build_web_link` implementation. The single missing piece was WEB-OUT-04: debug builds emitting `index.wasm.map`.

### Change 1: `-gsource-map` in `src/cli/build_web.c`

Added `"-gsource-map"` to the `else` (debug) branch of `iron_build_web_link`'s release/debug flag block. The flag instructs emcc to produce `dist/web/index.wasm.map` alongside the normal output — a DWARF-style source map that browser DevTools can consume for C-level debugging. The release branch (`-Oz -flto -sASSERTIONS=0`) is untouched.

The argv layout comment was updated to reflect that the debug branch now occupies 4 slots instead of 3. Total argv usage stays well within the 64-slot budget.

### Change 2: CI assertions in `.github/workflows/web.yml`

Two assertions added to the `emsdk-smoke` job:

1. **Debug smoke (Phase 7 step extended):** After the existing three-artifact assertions, added `test -s dist/web/index.wasm.map` to verify the sourcemap is present and non-empty for debug builds.

2. **Release smoke (new step):** A new step `End-to-end smoke — iron build --target=web --release (Phase 11, WEB-OUT-04 release absence)` runs a fresh `--release` build and asserts `test ! -f dist/web/index.wasm.map` — confirming the map is absent when optimization flags replace `-gsource-map`.

### Change 3: REQUIREMENTS.md

WEB-OUT-01 through WEB-OUT-05 all marked `[x]` in the checklist and updated to `Complete` in the traceability table.

### Change 4: ROADMAP.md

Phase 11 line updated from `[ ]` to `[x]` with completion date `2026-04-12`.

## Commits

- `4d1f64c` feat(11): add -gsource-map to debug builds (WEB-OUT-04)
- `5264259` ci(11): assert wasm.map present for debug, absent for release (WEB-OUT-04)

## Deviations from Plan

None — plan execution was straightforward. WEB-OUT-01/02/03/05 needed no code changes (already satisfied by earlier phases); only WEB-OUT-04 required adding a single flag.

## Requirements Satisfied

| Requirement | Status | Notes |
|-------------|--------|-------|
| WEB-OUT-01 | Complete | `dist/web/` output — satisfied by Phase 7's `mkdir_p` + `-o dist/web/index.html` |
| WEB-OUT-02 | Complete | `index.html` naming — hardcoded in Phase 7's emcc argv |
| WEB-OUT-03 | Complete | `index.js` + `index.wasm` siblings — produced by emcc from `-o index.html` |
| WEB-OUT-04 | Complete | `index.wasm.map` in debug — added `-gsource-map` in this phase |
| WEB-OUT-05 | Complete | Fresh-clone `mkdir_p` — satisfied by Phase 7's `iron_mkdir_p("dist/web")` |
