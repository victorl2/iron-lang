---
phase: 89-migr-codemod
plan: "01"
subsystem: tooling
tags: [python, codemod, cli, migration, receiver-method, patch-block]

requires:
  - phase: 88-strict-gate
    provides: v3_strict_mode gate and E0260..E0264 error infrastructure

provides:
  - scripts/migrate_v2_to_v3.py: Python MVP codemod handling all four v2-to-v3 transform types
  - ironc migrate subcommand: thin execvp wrapper dispatching to the Python script

affects:
  - 89-02 (apply codemod to the in-tree source files)
  - 89-03 (gate flip depends on a working codemod)

tech-stack:
  added: [Python 3 codemod script, difflib unified diff]
  patterns:
    - Line-based segment classification with brace-depth tracking for object scope
    - Patch block grouping via pending-other buffer that defers close until next recv type is known
    - execvp shell-out from C subcommand for script-backed CLI commands

key-files:
  created:
    - scripts/migrate_v2_to_v3.py
  modified:
    - src/cli/main.c

key-decisions:
  - "Multi-line method bodies are consumed into the recv segment during classification so the grouping loop never sees partial methods"
  - "Pending 'other' lines (blanks, comments) between methods of the same type are buffered and flushed inside the patch block, not outside"
  - "Body line reindentation strips original indent relative to the func declaration indent and adds the inner patch block indent"
  - "execvp chosen over system() so ironc exit code is the Python script's exit code with no shell wrapper"

patterns-established:
  - "Segment-based classification: classify all lines into typed dicts before emitting output"
  - "Pending-other buffer pattern for lookahead grouping without full two-pass"

requirements-completed: [MIGR-02, MIGR-03, MIGR-04]

duration: 35min
completed: 2026-04-23
---

# Phase 89 Plan 01: Codemod Script and ironc migrate Subcommand Summary

**Python line-based codemod with four v2-to-v3 transforms and ironc migrate subcommand wired via execvp**

## Performance

- **Duration:** 35 min
- **Started:** 2026-04-23T22:21:19Z
- **Completed:** 2026-04-23T22:58:30Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- `scripts/migrate_v2_to_v3.py` handles all four transform types: receiver-method grouping into patch blocks, mut-receiver stripping, inline field defaults moved to init() bodies, and standalone mut removal
- Consecutive methods of the same receiver type are grouped into a single `patch object T { ... }` block with correct indentation and receiver-to-self rename in both signature and body lines
- `ironc migrate --from v2 --to v3 <path>` subcommand dispatches to the Python script via execvp; missing args print usage and exit 1
- Transform is idempotent: second run on already-migrated source produces zero diff
- Server build (ninja) clean with no new warnings; lexer and typecheck unit tests still pass

## Task Commits

1. **Task 1: Write scripts/migrate_v2_to_v3.py MVP codemod** - `d1e237ef` (feat)
2. **Task 2: Add migrate subcommand to src/cli/main.c** - `bb3c2f64` (feat)

## Files Created/Modified

- `scripts/migrate_v2_to_v3.py` - Python MVP codemod; segment-based classifier, patch-block grouper, init() synthesizer, unified diff to stderr
- `src/cli/main.c` - Added `migrate` entry to print_usage() and subcommand dispatch block using execvp; added `#include <unistd.h>`

## Decisions Made

- Multi-line method bodies are consumed into the recv segment during Phase A classification so the Phase C grouping loop never encounters partial-method lines.
- A pending-other buffer defers blank/comment lines between methods: if the next recv is the same type, they flush inside the block; otherwise the block closes first.
- Body lines are reindented by stripping the original func-declaration indent and prepending the inner patch-block indent, preserving relative indentation of nested constructs.
- Used `execvp` (not `system`) so the ironc process image is replaced by python3, making ironc's exit code exactly the Python script's exit code.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed multi-line body classification breaking patch-block grouping**

- **Found during:** Task 1 (initial transform test)
- **Issue:** Line-based regex captured only the func signature line; body lines became separate 'other' segments that called close_patch(), preventing consecutive methods from sharing a patch block and producing malformed output
- **Fix:** Changed Phase A to consume body lines (until matching brace closes) into the recv segment dict, so Phase C sees a single atomic recv unit per method
- **Files modified:** scripts/migrate_v2_to_v3.py
- **Verification:** Synthetic v2 test file with multi-line bodies transformed to correctly grouped and indented patch blocks; idempotent on second run
- **Committed in:** d1e237ef (Task 1 commit)

**2. [Rule 1 - Bug] Fixed receiver rename not applied to body lines**

- **Found during:** Task 1 (reviewing transform output)
- **Issue:** rename_receiver() was called only on the func signature line; body lines with `p.x` or `c.count` were emitted unrenamed
- **Fix:** Applied rename_receiver() to each reindented body line after reindent_body()
- **Files modified:** scripts/migrate_v2_to_v3.py
- **Verification:** `p.x + p.y` -> `self.x + self.y` in body; `c.count` -> `self.count` confirmed in diff output
- **Committed in:** d1e237ef (Task 1 commit)

**3. [Rule 1 - Bug] Fixed Python 3.9 type annotation incompatibility**

- **Found during:** Task 1 (first help check)
- **Issue:** First draft used `int | None` union syntax (Python 3.10+); server Python is 3.9, causing TypeError at import
- **Fix:** Rewrote to use plain dicts and removed all PEP 604 union annotations; no typing module required
- **Files modified:** scripts/migrate_v2_to_v3.py
- **Verification:** `python3 migrate_v2_to_v3.py --help` passes on both local Mac and silvaserver.local
- **Committed in:** d1e237ef (Task 1 commit)

---

**Total deviations:** 3 auto-fixed (all Rule 1 bugs found during Task 1 iteration)
**Impact on plan:** All fixes necessary for correctness. No scope creep.

## Issues Encountered

- `test_parser_errors` unit test is registered in CTest but binary does not exist on server (pre-existing issue, unrelated to this plan). All other unit tests pass.

## Next Phase Readiness

- `scripts/migrate_v2_to_v3.py` is ready for Plan 89-02 to apply to the full in-tree source
- `ironc migrate --from v2 --to v3 <path>` works end-to-end from the project root
- Both transforms verified with dry-run on `tests/integration/receiver_method_basic.iron` and `src/stdlib/raylib.iron`

---
*Phase: 89-migr-codemod*
*Completed: 2026-04-23*
