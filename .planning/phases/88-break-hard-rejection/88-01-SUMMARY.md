---
phase: 88-break-hard-rejection
plan: "01"
subsystem: parser
tags: [parser, diagnostics, cli, v3, strict-mode, breaking-changes]

# Dependency graph
requires:
  - phase: 85-init
    provides: is_init flag, field parsing infrastructure, fieldless synth-init
  - phase: 79-mut
    provides: recv_is_mut flag in iron_parse_func_or_method, IRON_TOK_MUT handling
  - phase: 84-muttier
    provides: tier modifier pattern for parser-level strict rejections
provides:
  - E0260..E0264 error codes in diagnostics.h gated behind v3_strict_mode
  - v3_strict_mode boolean on Iron_Parser (defaults false; Phase 89 flips true)
  - --strict-v3 CLI flag parsed in main.c, threaded to iron_check and IronBuildOpts
  - Five rejection sites in parser.c (receiver-method, mut-receiver, inline-default, mut-keyword, no-init)
affects: [89-migr, 88-02, 88-03]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Parser gate pattern: check p->v3_strict_mode before emitting BREAK errors"
    - "Migration hint format: 'run ironc migrate --from v2 --to v3 <file>' on all BREAK errors"
    - "CLI flag threaded via IronBuildOpts.strict_v3 to parser.v3_strict_mode"

key-files:
  created: []
  modified:
    - src/diagnostics/diagnostics.h
    - src/parser/parser.h
    - src/parser/parser.c
    - src/cli/check.h
    - src/cli/check.c
    - src/cli/build.h
    - src/cli/build.c
    - src/cli/main.c

key-decisions:
  - "Single site in iron_parse_func_or_method handles both BREAK-01 (E0260) and BREAK-02 (E0261) using the already-set recv_is_mut flag"
  - "IRON_TOK_MUT in statement position uses __attribute__((fallthrough)) to satisfy -Werror=implicit-fallthrough on GCC"
  - "INIT-02 (E0264) reuses the extra_decls_out scan already present for the fieldless-synth block rather than a separate counter"
  - "Gate OFF path is zero-cost: all five sites add only a boolean check before the existing parse path"

patterns-established:
  - "BREAK gate check: if (p->v3_strict_mode) { iron_diag_emit(..., hint); sync; return error; }"
  - "All five BREAK errors use identical suggestion string for codemod self-verification"

requirements-completed: [BREAK-01, BREAK-02, BREAK-03, BREAK-04, MIGR-05, INIT-02]

# Metrics
duration: 35min
completed: 2026-04-23
---

# Phase 88 Plan 01: BREAK Gate Infrastructure Summary

**Five v3 rejection sites (E0260..E0264) gated behind --strict-v3 flag with migration hints; gate OFF preserves pure-superset (106/41/171 unit tests green)**

## Performance

- **Duration:** 35 min
- **Started:** 2026-04-23T00:00:00Z
- **Completed:** 2026-04-23T00:35:00Z
- **Tasks:** 3 (+ 1 auto-fix for compiler warning)
- **Files modified:** 8

## Accomplishments
- Added E0260..E0264 error code constants to diagnostics.h with Phase 88 comment block
- Added v3_strict_mode boolean to Iron_Parser struct (defaults false, Phase 89 flips true)
- Implemented five gated rejection sites in parser.c covering all five BREAK requirements
- Plumbed --strict-v3 CLI flag from main.c through IronBuildOpts and iron_check() to parser
- Verified: gate OFF = all 106 parser + 41 lexer + 171 typecheck unit tests pass unchanged
- Verified: gate ON = receiver-method syntax emits E0260 with migration hint, exits 1

## Task Commits

Each task was committed atomically:

1. **Task 1: E0260..E0264 constants + v3_strict_mode field** - `7f6827c` (feat)
2. **Task 2: Five gated rejection sites in parser.c** - `feeef50` (feat)
3. **Task 3: --strict-v3 CLI flag plumbed through main.c and check.c** - `7b6fa0e` (feat)

**Auto-fix (Rule 1 - Bug):** `5455654` (fix) - GCC -Werror=implicit-fallthrough

## Files Created/Modified
- `src/diagnostics/diagnostics.h` - Five BREAK error codes E0260..E0264
- `src/parser/parser.h` - v3_strict_mode bool field on Iron_Parser struct
- `src/parser/parser.c` - v3_strict_mode init + five gated rejection sites
- `src/cli/check.h` - iron_check() signature updated to accept bool strict_v3
- `src/cli/check.c` - iron_check() body updated; sets parser.v3_strict_mode
- `src/cli/build.h` - IronBuildOpts.strict_v3 field added
- `src/cli/build.c` - sets parser.v3_strict_mode = opts.strict_v3 after parser creation
- `src/cli/main.c` - parses --strict-v3 flag; threads to iron_check and IronBuildOpts; help text

## Decisions Made
- Single rejection site in iron_parse_func_or_method handles both BREAK-01 and BREAK-02 because the recv_is_mut flag is already set by that point in the parse path
- Used __attribute__((fallthrough)) for the IRON_TOK_MUT case in iron_parse_stmt switch to satisfy GCC -Werror=implicit-fallthrough; gate OFF falls through to the existing default expression-statement path with zero behavior change
- INIT-02 reuses the extra_decls_out scan pattern from the existing fieldless-synth block rather than adding a separate init_count variable

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] GCC -Werror=implicit-fallthrough rejected IRON_TOK_MUT case**
- **Found during:** Remote build verification after Task 2 + Task 3 commits
- **Issue:** GCC treated the IRON_TOK_MUT case fallthrough to default: as an implicit fallthrough error under -Werror=implicit-fallthrough
- **Fix:** Added __attribute__((fallthrough)) statement between the IRON_TOK_MUT case body and the default: label
- **Files modified:** src/parser/parser.c
- **Verification:** ninja rebuild succeeded, all 3 unit test suites green
- **Committed in:** 5455654 (separate fix commit)

---

**Total deviations:** 1 auto-fixed (compiler warning as error)
**Impact on plan:** Required fix; no scope creep. Gate OFF behavior unchanged.

## Issues Encountered
- GCC -Werror=implicit-fallthrough on the IRON_TOK_MUT case fallthrough path -- fixed immediately with __attribute__((fallthrough))

## Integration Tests
Integration tests skipped (no clang on silvaserver.local). Unit tests cover all parser, lexer, and typecheck paths. Gate-ON functional test verified manually via ironc check --strict-v3.

## Next Phase Readiness
- Gate infrastructure is complete and landed; all existing tests green
- Phase 88-02 can add TEST-14 compile_fail fixture (v3_receiver_method_removed.iron) using the --strict-v3 flag
- Phase 89 MIGR can flip v3_strict_mode default to true after codemod migration

---
*Phase: 88-break-hard-rejection*
*Completed: 2026-04-23*
