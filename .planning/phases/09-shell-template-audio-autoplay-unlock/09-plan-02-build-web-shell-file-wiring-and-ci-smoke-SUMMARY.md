---
phase: 09-shell-template-audio-autoplay-unlock
plan: 02
subsystem: web-build
tags: [emcc, shell-file, mkstemp, posix-spawnp, web-shell, coop-coep, audio-unlock]

requires:
  - phase: 09-plan-01-web-shell-template-header
    provides: IRON_WEB_DEFAULT_SHELL C string literal in web_shell_template.h
  - phase: 08-plan-01-build-web-raylib-amalgamation-link
    provides: iron_build_web_link function + argv construction in build_web.c

provides:
  - iron_build_web_link uses --shell-file for every web build (default or custom)
  - Default shell materialized to temp file via mkstemps + write + unlink lifecycle
  - Custom [web].shell validated for {{{ SCRIPT }}} token before emcc spawn (WEB-SHELL-06)
  - CI smoke step greps dist/web/index.html for all 4 shell patches after Phase 8 raylib build
  - src/cli/web_shell_template.h added to paths filter on both push and pull_request

affects:
  - Phase 10 (Asset Preload) — inherits the final argv layout (slot 40 HWM)
  - Phase 11 (Output Layout) — shell-file flow must remain in iron_build_web_link
  - Phase 13 (Integration Tests) — test_shell_subst.c will exercise the custom-shell validation path

tech-stack:
  added: []
  patterns:
    - "mkstemps(/tmp/iron_web_shell_XXXXXX.html) + write-with-EINTR-retry + unlink-on-both-paths for embedded resource materialization"
    - "strstr(contents, \"{{{ SCRIPT }}}\") custom template validation before emcc spawn"
    - "use_temp_shell flag to guard unlink — never unlink user-supplied cfg->shell"

key-files:
  created: []
  modified:
    - src/cli/build_web.c
    - .github/workflows/web.yml
    - .planning/ROADMAP.md
    - .planning/REQUIREMENTS.md

key-decisions:
  - "Use mkstemps (not mkstemp) on Apple/Linux to preserve .html suffix so emcc's file-type heuristic works correctly; POSIX mkstemp fallback retained for other platforms"
  - "Place --shell-file before -o in argv for readability; emcc tolerates shell-file anywhere in linker argv"
  - "Unlink temp shell on ALL exit paths (forbidden-flag audit, spawn failure, waitpid failure, emcc non-zero) to prevent /tmp stragglers"
  - "Read full custom shell into heap buffer for strstr check; free before returning regardless of result"
  - "shell_path_buf[64] on stack — mkstemps template is 31 chars, well within 64 limit; no heap allocation needed for the temp path"

requirements-completed:
  - WEB-SHELL-06

duration: 11min
completed: 2026-04-12
---

# Phase 9 Plan 02: Build Web Shell File Wiring and CI Smoke Summary

**Default shell materialized per-build via mkstemps + unlink, custom [web].shell validated for `{{{ SCRIPT }}}` before emcc spawn, and CI smoke greps generated index.html for all four COOP/audio/WebGL patches**

## Performance

- **Duration:** ~11 min
- **Started:** 2026-04-12T02:32:59Z
- **Completed:** 2026-04-12T02:43:56Z
- **Tasks:** 4 (build_web.c wiring, web.yml CI, ROADMAP, REQUIREMENTS)
- **Files modified:** 4

## Accomplishments

- `iron_build_web_link` now passes `--shell-file` to emcc on every web build; the embedded `IRON_WEB_DEFAULT_SHELL` is written to `/tmp/iron_web_shell_XXXXXX.html` via `mkstemps`, used, and unlinked on both success and failure paths
- Custom `[web].shell` paths are validated via `strstr` for `{{{ SCRIPT }}}` before `posix_spawnp`; missing token produces a clear diagnostic naming the file and token (WEB-SHELL-06)
- CI smoke step added after Phase 8 raylib build: greps `dist/web/index.html` for `crossOriginIsolated`, `unlockAudio|_audio_resume`, `webglcontextlost`, `canvas`, and asserts `{{{ SCRIPT }}}` is absent (replaced by emcc glue loader)
- Phase 9 marked complete in ROADMAP.md; WEB-SHELL-06 flipped `[x]` in REQUIREMENTS.md

## Task Commits

1. **Task 1: shell-file wiring in build_web.c** - `00a9449` (feat)
2. **Task 2: web.yml CI extensions** - `7610557` (ci)
3. **Tasks 3+4: ROADMAP + REQUIREMENTS updates** - `1bae259` (docs)

## Files Created/Modified

- `/Users/victor/code/worker-3/iron-lang/src/cli/build_web.c` - Added `#include "cli/web_shell_template.h"`, shell resolution block (Path A custom + Path B default mkstemp), `--shell-file` argv entries, and `unlink(shell_path_buf)` on all exit paths; removed `(void)cfg` suppressor
- `/Users/victor/code/worker-3/iron-lang/.github/workflows/web.yml` - Added `src/cli/web_shell_template.h` to both push/PR paths filters; added "Shell-patch assertions" step after Phase 8 raylib smoke
- `/Users/victor/code/worker-3/iron-lang/.planning/ROADMAP.md` - Phase 9 `[x]`, completion date, plan 02 `[x]`
- `/Users/victor/code/worker-3/iron-lang/.planning/REQUIREMENTS.md` - WEB-SHELL-06 `[x]` in list + `Complete (09-plan-02)` in coverage table

## Decisions Made

- Used `mkstemps` with suffix length 5 (`".html"`) on Apple/Linux so the temp file retains the `.html` extension; emcc uses it as a hint for shell-file handling. Generic POSIX fallback to `mkstemp` retained via `#else` guard.
- `--shell-file` placed before `-o dist/web/index.html` in argv layout. emcc accepts shell-file anywhere in the linker argv; pre-output placement is a readability choice.
- `use_temp_shell` integer flag guards `unlink` — we never unlink `cfg->shell` (the user's file).
- Custom shell validation reads the full file into a heap buffer to run `strstr`; this is correct for shell files up to `LONG_MAX` bytes (in practice always <100 KB).

## Deviations from Plan

None — plan executed exactly as written. The `fcntl.h` include was added proactively since `mkstemps` may require it on some platforms (it does not on macOS/Linux but is harmless).

## Issues Encountered

None. Build compiled cleanly (only the pre-existing `ld: warning: ignoring duplicate libraries: '-lpthread'` linker warning, unrelated to this plan). All 69 ctest tests passed.

## Next Phase Readiness

- Phase 10 (Asset Preload + Top-Level Loader Guard) can proceed: `iron_build_web_link` argv slot budget is 40 HWM vs 48 capacity, leaving 8 slots free for `--preload-file` entries
- The `shell_path` and `use_temp_shell` variables are local to `iron_build_web_link`; Phase 10 changes to argv construction are independent

---
*Phase: 09-shell-template-audio-autoplay-unlock*
*Completed: 2026-04-12*
