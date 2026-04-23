---
phase: 90-ident-byte-identity
plan: "01"
subsystem: testing
tags: [bash, ironc, byte-identity, golden-files, stdlib]

requires:
  - phase: 89-migr
    provides: v3-migrated stdlib iron files (int.iron, float.iron, io.iron) in src/stdlib/

provides:
  - scripts/verify-v3-migration.sh release-blocker script
  - tests/migrate_identity/sources/ with 3 stdlib fixture files
  - tests/migrate_identity/expected/.gitkeep tracking the golden directory

affects:
  - 90-02 (generates goldens and runs full verification)
  - 91-docs (release gate documentation)

tech-stack:
  added: []
  patterns:
    - "golden-file testing: fixture sources in sources/, expected C in expected/"
    - "bash verification script with clang-absent graceful skip path"
    - "subcommand probe pattern for forward-compatible ironc emit-c detection"

key-files:
  created:
    - scripts/verify-v3-migration.sh
    - tests/migrate_identity/sources/int.iron
    - tests/migrate_identity/sources/float.iron
    - tests/migrate_identity/sources/io.iron
    - tests/migrate_identity/expected/.gitkeep
    - tests/migrate_identity/README.md
  modified: []

key-decisions:
  - "Check golden existence before invoking ironc so WARN path works without a working emit-c subcommand"
  - "Probe emit-c subcommand by inspecting output for unknown command string, not exit code alone"
  - "Script exits 0 with WARN when no goldens present (not a failure -- goldens come in Plan 90-02)"

patterns-established:
  - "verify-v3-migration.sh --generate populates expected/ from current ironc output"
  - "WARN (no golden) is non-fatal; FAIL (diff mismatch) is the release gate"

requirements-completed: [IDENT-01, IDENT-02]

duration: 3min
completed: 2026-04-23
---

# Phase 90 Plan 01: IDENT Infrastructure Summary

**Bash verify-v3-migration.sh release-blocker script and 3 stdlib iron fixtures under tests/migrate_identity/, with graceful WARN path when goldens are absent**

## Performance

- **Duration:** 3 min
- **Started:** 2026-04-23T20:24:49Z
- **Completed:** 2026-04-23T20:27:58Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- scripts/verify-v3-migration.sh created and executable: PASS/FAIL/WARN loop, --generate flag, clang-absent NOTE, subcommand probe
- tests/migrate_identity/sources/ populated with int.iron, float.iron, io.iron copied verbatim from src/stdlib/
- Script exits 0 with WARN per fixture when no goldens exist (no false failures before Plan 90-02)
- Script exits 0 with NOTE when clang is absent (silvaserver.local compatible)

## Task Commits

1. **Task 1: Write scripts/verify-v3-migration.sh** - `b9c58b9c` (feat)
2. **Task 2: Create tests/migrate_identity/sources/** - `51a1c03c` (feat)
3. **Deviation fix: golden-check ordering** - `01b854c3` (fix)

## Files Created/Modified
- `scripts/verify-v3-migration.sh` - Release-blocker verification script with PASS/FAIL/WARN loop and --generate mode
- `tests/migrate_identity/sources/int.iron` - stdlib int module fixture
- `tests/migrate_identity/sources/float.iron` - stdlib float module fixture
- `tests/migrate_identity/sources/io.iron` - stdlib io module fixture
- `tests/migrate_identity/expected/.gitkeep` - Tracks expected/ dir before goldens land
- `tests/migrate_identity/README.md` - How to run and regenerate goldens

## Decisions Made
- Check golden existence before invoking ironc: stdlib fixtures currently exit non-zero from ironc (E0500 NULL body for C-backed functions), so checking golden first lets the no-golden WARN path work cleanly without ironc needing to succeed
- Subcommand probe uses output inspection (`grep -qv "unknown command"`) since `emit-c --help` exits 0 even for unknown commands in current ironc

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Golden-check moved before ironc emit step**
- **Found during:** Task 1 (verify script) / post-Task 2 integration test
- **Issue:** Plan spec ordered emit-C then check for golden. But stdlib fixtures fail ironc build (E0500 NULL body for C-backed functions like int_to_string), so script recorded FAIL for every fixture even with no goldens. This violated success criteria "exits 0 with WARN per fixture when no goldens present."
- **Fix:** Moved `if [ ! -f "$EXPECTED_DIR/$name.c" ]` check before ironc invocation. When no golden exists, emit is skipped and WARN printed. Only when a golden is present does the script invoke ironc and diff.
- **Files modified:** scripts/verify-v3-migration.sh
- **Verification:** `bash scripts/verify-v3-migration.sh` exits 0 with 3 WARN lines when no goldens present
- **Committed in:** 01b854c3 (fix commit)

**2. [Rule 1 - Bug] Subcommand probe uses output inspection not exit code**
- **Found during:** Task 1 (verify script)
- **Issue:** `ironc emit-c --help` exits 0 even when `emit-c` is an unknown command (ironc always exits 0 for its help/usage path). Probe logic `if $IRONC emit-c --help >/dev/null 2>&1` selected `SUBCOMMAND="emit-c"` even though the command is unsupported.
- **Fix:** Capture probe output and grep for "unknown command" string to distinguish real support from unsupported command that exits 0.
- **Files modified:** scripts/verify-v3-migration.sh
- **Committed in:** 01b854c3 (fix commit, same as above)

---

**Total deviations:** 2 auto-fixed (both Rule 1 bugs in same fix commit)
**Impact on plan:** Both fixes necessary for correct script behavior. No scope creep.

## Issues Encountered
- ironc has no `emit-c` or `build --emit-c` subcommand yet; script is written for the forward-compatible interface the plan describes, with the golden-check-first ordering making it robust until that subcommand ships

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Plan 90-02 can now generate goldens via `scripts/verify-v3-migration.sh --generate` (requires ironc to succeed on these files, which may need E0500 resolution or a workaround)
- Script infrastructure is complete and tested for the WARN and clang-absent paths

---
*Phase: 90-ident-byte-identity*
*Completed: 2026-04-23*
