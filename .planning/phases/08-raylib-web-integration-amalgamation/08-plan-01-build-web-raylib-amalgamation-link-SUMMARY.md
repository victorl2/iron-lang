---
phase: 08-raylib-web-integration-amalgamation
plan: 01
subsystem: build
tags: [emcc, raylib, amalgamation, web, wasm, emscripten]

# Dependency graph
requires:
  - phase: 07-build-web-c-emcc-orchestration
    provides: iron_build_web_link function with canonical flag set including -sUSE_GLFW=3

provides:
  - iron_build_web_link emcc argv extended with raylib.c + -DPLATFORM_WEB + -DGRAPHICS_API_OPENGL_ES2 + -Isrc/vendor/raylib when opts.use_raylib is true

affects:
  - 08-plan-02 (hello_raylib fixture + CI smoke step that exercises this code path)
  - Phase 11 (web config overrides — cfg param already plumbed)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Raylib web: single-source-file amalgamation approach (raylib.c #includes all sibling .c files, rcore.c routes to rcore_web.c under -DPLATFORM_WEB)"
    - "String-literal argv entries for static flags — no malloc/free, simpler cleanup paths"
    - "opts.use_raylib gate: same flag set by build.c for native path; web path mirrors the gating idiom"

key-files:
  created: []
  modified:
    - src/cli/build_web.c

key-decisions:
  - "Single-token -Isrc/vendor/raylib form chosen over two-token -I + src/vendor/raylib — keeps cleanup paths trivial (no allocation)"
  - "String literals only for the 4 new argv entries — no strdup/malloc avoids adding free() calls on 4 exit paths"
  - "No -I src/vendor/raylib/external/glfw/include added — emcc -sUSE_GLFW=3 (already in IRON_WEB_CANONICAL_FLAGS) provides GLFW/glfw3.h via sysroot; adding the vendor path would be premature"
  - "Forbidden-flag audit verified by inspection before edit: none of PLATFORM_WEB, GRAPHICS_API_OPENGL_ES2, -Isrc/vendor/raylib, src/vendor/raylib/raylib.c contain any IRON_WEB_FORBIDDEN_FLAGS substring"

patterns-established:
  - "Raylib amalgamation discipline: only raylib.c referenced as source; individual .c files (rcore.c, rshapes.c, etc.) are never referenced directly in build_web.c"

requirements-completed:
  - WEB-BUILD-05

# Metrics
duration: 6min
completed: 2026-04-12
---

# Phase 8 Plan 01: Build Web Raylib Amalgamation Link Summary

**Gated raylib amalgamation entry in iron_build_web_link: appends src/vendor/raylib/raylib.c + -DPLATFORM_WEB + -DGRAPHICS_API_OPENGL_ES2 + -Isrc/vendor/raylib to the emcc argv when opts.use_raylib is true**

## Performance

- **Duration:** ~6 min
- **Started:** 2026-04-12T02:04:15Z
- **Completed:** 2026-04-12T02:10:15Z
- **Tasks:** 1 of 1
- **Files modified:** 1

## Accomplishments

- Added a gated raylib block inside `iron_build_web_link` (src/cli/build_web.c) that appends exactly 4 string-literal argv entries when `opts.use_raylib` is true
- Phase 7 raylib-free baseline unchanged: `opts.use_raylib == false` emits the identical argv as before
- All 69 ctests pass (0 failures), ironc builds clean, forbidden-flag audit unaffected
- Amalgamation discipline holds: no individual raylib .c files (rcore.c, rshapes.c, etc.) referenced in argv

## Argv Entries Added (when opts.use_raylib is true)

1. `"src/vendor/raylib/raylib.c"` — amalgamation driver; pulls in rcore.c (which routes to platforms/rcore_web.c under PLATFORM_WEB), rshapes.c, rtextures.c, rtext.c, rmodels.c, raudio.c, utils.c via #include
2. `"-DPLATFORM_WEB"` — routes rcore.c through platforms/rcore_web.c (verified at src/vendor/raylib/rcore.c lines 545-546)
3. `"-DGRAPHICS_API_OPENGL_ES2"` — selects the GLES2 rlgl backend that emcc/WebGL2 expects
4. `"-Isrc/vendor/raylib"` — header search path for raylib's internal #include "raylib.h" / "rcamera.h" / etc.

## Grep Evidence

```
grep -c 'opts.use_raylib'              src/cli/build_web.c  → 2  (gated block + existing comment)
grep -c 'src/vendor/raylib/raylib.c'  src/cli/build_web.c  → 3  (argv entry + comment + pre-existing comment at line 489)
grep -c -- '-DPLATFORM_WEB'           src/cli/build_web.c  → 2  (argv entry + comment)
grep -c -- '-DGRAPHICS_API_OPENGL_ES2' src/cli/build_web.c → 2  (argv entry + comment)
grep -c -- '-Isrc/vendor/raylib'      src/cli/build_web.c  → 2  (argv entry + comment)
```

Amalgamation discipline check:
```
! grep -E 'argv\[n\+\+\] = .*"(rcore|rglfw|rshapes|rtext|rtextures|rmodels|raudio|utils)\.c"' src/cli/build_web.c → no matches (PASS)
```

Pin discipline check:
```
! grep -F '4.0.23' src/cli/build_web.c → no matches (PASS)
```

## Task Commits

1. **Task 1: Add gated raylib amalgamation source + flags inside iron_build_web_link** - `18e9e50` (feat)

**Plan metadata:** (see final commit below)

## Files Created/Modified

- `src/cli/build_web.c` — Added 31 lines (25-line comment block + 6-line gated if block) between the IRON_WEB_SRC_COUNT source file loop and the `argv[n] = NULL` terminator

## Decisions Made

- **Single-token `-Isrc/vendor/raylib`** chosen over two-token form (`"-I"`, `"src/vendor/raylib"`) — simpler, no additional argv slot semantics to track
- **String literals only** for all 4 new entries — no `strdup`/`malloc` means no new `free()` calls across the 4 existing cleanup paths (audit-failure, spawnp-failure, waitpid-failure, success)
- **No `-I src/vendor/raylib/external/glfw/include`** — `-sUSE_GLFW=3` in IRON_WEB_CANONICAL_FLAGS provides GLFW/glfw3.h from the emcc sysroot; if a future link error proves this wrong, that is the escalation trigger per CONTEXT.md
- **max_argv budget**: new high-water mark is ~38 entries (was ~34), well within the 48-slot ceiling

## Invariants Confirmed

- `_WIN32 #error` guard at the top of build_web.c is unchanged; no Windows code path was introduced
- No vendored file under `src/vendor/raylib/` was modified
- `IRON_WEB_CANONICAL_FLAGS` array unchanged (12 entries, -sUSE_GLFW=3 already present from Phase 7)
- `IRON_WEB_FORBIDDEN_FLAGS` array and `is_forbidden_flag()` function unchanged
- `iron_build_web_link` signature unchanged; `build_web.h` unchanged; `build.c` unchanged

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Plan 01 complete. Plan 02 can now proceed: add `tests/integration/web/hello_raylib.iron` fixture and the CI smoke step that exercises `opts.use_raylib = true` end-to-end against a real emcc.
- The Phase 7 hello.iron raylib-free baseline remains the regression anchor.
- WEB-BUILD-05 requirement is satisfied by this plan; Plan 02 will mark it complete in REQUIREMENTS.md.

---
*Phase: 08-raylib-web-integration-amalgamation*
*Completed: 2026-04-12*
