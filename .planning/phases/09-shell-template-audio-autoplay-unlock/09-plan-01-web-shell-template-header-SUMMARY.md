---
phase: 09-shell-template-audio-autoplay-unlock
plan: 01
subsystem: web
tags: [emscripten, html, c-string-literal, coop-coep, audio-unlock, webgl]

# Dependency graph
requires:
  - phase: 02-web-config
    provides: IronWebConfig.shell field establishing the custom-shell contract
  - phase: 07-web-link
    provides: iron_build_web_link argv construction point where --shell-file will slot in
provides:
  - IRON_WEB_DEFAULT_SHELL C string literal in src/cli/web_shell_template.h — the full default web shell HTML ready for emcc --shell-file
affects: [09-plan-02-web-shell-wire, phase-10-asset-preload, phase-12-pong-audio]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Header-only C data file: include guard + one static const char[] array; no functions, no extra symbols"
    - "Adjacent-string-literal concatenation for multi-line HTML embedded in C (one string per HTML line, each ending \\n)"
    - "COOP/COEP preflight placed in <head> before any other script so it aborts before emcc glue runs"
    - "Audio unlock via one-shot removeEventListener inside the callback (not once:true) so re-fire is possible before Module loads"

key-files:
  created:
    - src/cli/web_shell_template.h
  modified: []

key-decisions:
  - "Dropped FileSaver.js CDN script and saveFileFromMEMFSToDisk helper from minshell.html — reduces external dependencies; Iron does not use them"
  - "Kept tabindex=-1 on canvas (minshell.html default) because keydown listener attaches to document, not canvas"
  - "COOP/COEP error banner uses document.body.innerHTML assignment (not appendChild) so it fires before <body> children exist"
  - "webglcontextlost script placed after canvas element in document order so getElementById('canvas') is non-null at parse time"

patterns-established:
  - "web_shell_template.h pattern: pure data header, static const char[], include guard only, no includes needed"

requirements-completed: [WEB-SHELL-01, WEB-SHELL-02, WEB-SHELL-03, WEB-SHELL-04, WEB-SHELL-05, WEB-SHELL-07]

# Metrics
duration: 2min
completed: 2026-04-12
---

# Phase 9 Plan 01: Web Shell Template Header Summary

**Default Iron web shell baked into a C header as IRON_WEB_DEFAULT_SHELL: COOP/COEP preflight, audio unlock, and webglcontextlost handler patched into minshell.html scaffolding**

## Performance

- **Duration:** ~2 min
- **Started:** 2026-04-12T02:29:01Z
- **Completed:** 2026-04-12T02:31:00Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- Created `src/cli/web_shell_template.h` with `static const char IRON_WEB_DEFAULT_SHELL[]` containing the complete default Iron web shell HTML as an adjacent-string-literal C string (85 lines)
- Applied all four mandatory patches: COOP/COEP preflight in `<head>`, canvas element preserved from minshell.html, audio unlock one-shot listener, and webglcontextlost reload-prompt handler
- Verified the header compiles cleanly under host `cc -Wall -Werror` with no warnings; string literal is well-formed C with all `"` escaped and the inner JS `"\n"` double-escaped

## Task Commits

Each task was committed atomically:

1. **Task 1+2: Create web_shell_template.h + compile check** - `b37819f` (feat)

**Plan metadata:** (see final commit below)

## Files Created/Modified

- `src/cli/web_shell_template.h` - Default Iron web shell HTML embedded as `IRON_WEB_DEFAULT_SHELL` C string literal; consumed by Plan 02 to materialize a `--shell-file` temp file for emcc

## Decisions Made

- Dropped FileSaver.js CDN script and the `saveFileFromMEMFSToDisk` helper block from minshell.html — they add an external CDN dependency Iron does not use, and removing them keeps the embedded shell lean
- Retained `tabindex=-1` from minshell.html (instead of switching to `tabindex=1`) because the audio unlock keydown listener attaches to `document`, not the canvas element, making tabindex irrelevant for unlock behavior
- Used adjacent-string concatenation (one C string per HTML line) rather than a raw string macro or single concatenated blob — keeps diffs line-local and is compatible with C89/C99

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- `IRON_WEB_DEFAULT_SHELL` is exported and ready for Plan 02 (`build_web.c` wiring) to `#include "web_shell_template.h"` and write the string to a temp file for `--shell-file`
- No blockers; the header compiles cleanly and all required tokens are present

---
*Phase: 09-shell-template-audio-autoplay-unlock*
*Completed: 2026-04-12*
