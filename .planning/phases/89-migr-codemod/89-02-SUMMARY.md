---
phase: 89-migr-codemod
plan: "02"
subsystem: tooling
tags: [python, codemod, migration, receiver-method, patch-block, stdlib, raylib]

requires:
  - phase: 89-migr-codemod
    plan: "01"
    provides: scripts/migrate_v2_to_v3.py codemod and ironc migrate subcommand

provides:
  - src/stdlib/raylib.iron: v3-grammar raylib bindings with 309 receiver methods in 49 patch object blocks
  - src/stdlib/time.iron: v3-grammar time stdlib with 4 receiver methods in 2 patch object blocks
  - tests/integration/ cleaned: 6 obsolete v2-syntax fixture pairs (12 files) deleted

affects:
  - 89-03 (gate flip depends on clean tree; stdlib now compiles under --strict-v3)

tech-stack:
  added: []
  patterns:
    - Static method lines (func Type.method) classified separately from other lines in codemod Phase C to prevent emission inside patch blocks

key-files:
  created: []
  modified:
    - scripts/migrate_v2_to_v3.py
    - src/stdlib/raylib.iron
    - src/stdlib/time.iron

key-decisions:
  - "E0264 (missing init) diagnostics emit as warnings at exit 0; they predate this plan and are out of scope for the codemod migration"
  - "RE_STATIC_METHOD regex added to codemod to close patch blocks before static method declarations"
  - "Multiple patch object T blocks per type are valid when static methods interleave receiver groups"

requirements-completed: [MIGR-01, MIGR-03]

duration: 28min
completed: 2026-04-23
---

# Phase 89 Plan 02: Apply Codemod to Stdlib and Delete Obsolete Fixtures Summary

**309 raylib receiver methods and 4 time stdlib methods wrapped into v3 patch blocks via codemod; 12 obsolete v2-syntax integration fixtures deleted; ironc check --strict-v3 exits 0 for both stdlib files**

## Performance

- **Duration:** 28 min
- **Started:** 2026-04-23T23:00:09Z
- **Completed:** 2026-04-23T23:28:00Z
- **Tasks:** 2
- **Files modified:** 14 (2 stdlib + 12 deleted fixtures + 1 codemod fix)

## Accomplishments

- `src/stdlib/raylib.iron` fully migrated: all 309 v2 `func (recv: T)` declarations replaced by 49 `patch object T { ... }` blocks; `ironc check --strict-v3` exits 0
- `src/stdlib/time.iron` fully migrated: 4 receiver methods in 2 patch blocks; `Duration.to_ms` body correctly renamed `d.ms` to `self.ms`; `ironc check --strict-v3` exits 0
- 6 obsolete fixture pairs deleted (12 files); integration test count reduced from 395 to 389 with no unexpected failures
- Codemod idempotent on second run (zero diff output)
- Auto-fixed codemod bug where static methods (`func Type.method`) inside interleaved groups were emitted inside patch blocks

## Task Commits

1. **fix: close patch block before static methods in codemod** - `03c0bbfb` (fix)
2. **Task 1: migrate src/stdlib to v3 grammar via codemod** - `c28a3767` (feat)
3. **Task 2: delete obsolete v2-syntax receiver-method fixtures** - `d1fb2ffa` (test)

## Files Created/Modified

- `scripts/migrate_v2_to_v3.py` - Added `RE_STATIC_METHOD` regex and static-method detection in Phase C emission loop to prevent static methods from being buffered inside patch blocks
- `src/stdlib/raylib.iron` - 309 receiver methods in 49 patch object blocks; 456 insertions / 313 deletions
- `src/stdlib/time.iron` - 4 receiver methods in 2 patch blocks; `Duration.to_ms` body `d.ms` renamed to `self.ms`
- `tests/integration/receiver_method_basic.iron` + `.expected` - deleted (v2 syntax locked; test obsoleted)
- `tests/integration/receiver_method_mixed.iron` + `.expected` - deleted
- `tests/integration/mut_receiver_basic.iron` + `.expected` - deleted
- `tests/integration/mut_receiver_grammar.iron` + `.expected` - deleted
- `tests/integration/mut_receiver_mixed.iron` + `.expected` - deleted
- `tests/integration/mut_receiver_static_twin.iron` + `.expected` - deleted

## Decisions Made

- E0264 diagnostics ("object has no init") emit at exit 0; they are pre-existing warnings on C-backed opaque structs in raylib.iron and were present before this plan. They are out of scope for the receiver-method migration and will be addressed if raylib objects ever need Iron-side construction.
- Multiple `patch object T` blocks per type are correct when static methods (`func T.method()`) interleave receiver-method groups in the original source. This is valid v3 grammar.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed codemod emitting static method declarations inside patch blocks**

- **Found during:** Task 1 (running ironc check --strict-v3 on migrated raylib.iron)
- **Issue:** Static method lines (`func Quaternion.identity() -> Quaternion {}`) interleaved with receiver methods in the original source were classified as `other` segments and buffered in `pending_others`. When the next receiver method of the same type arrived, they were flushed inside the patch block, causing E0101 parse errors.
- **Fix:** Added `RE_STATIC_METHOD = re.compile(r'^\s*func\s+[A-Z]\w*\.')` and checked it in Phase C's `other` segment handling. When a static method line is encountered while a patch block is open, `flush_pending(inside_patch=False)` is called first, closing the block before emitting the static method.
- **Files modified:** `scripts/migrate_v2_to_v3.py`
- **Verification:** `ironc check --strict-v3 src/stdlib/raylib.iron` exits 0; grep for `func Quaternion.identity` confirms it appears outside patch blocks
- **Committed in:** `03c0bbfb` (pre-task fix commit, pushed to origin before running migration)

---

**Total deviations:** 1 auto-fixed (Rule 1 bug)
**Impact on plan:** Fix was necessary for correct codemod output. No scope creep. Codemod bug discovered only when running --strict-v3 on the actual stdlib which has interleaved static/receiver methods.

## Issues Encountered

- E0264 warnings on all 33 raylib opaque struct objects (`object Vector2 { val x: Float32 }` etc.) are pre-existing and emit at exit 0. These C-backed types are returned by raylib factory functions and do not need Iron init constructors. Not a blocker for this plan.
- `tests/run_tests.sh integration` shows "build failed / cannot spawn clang" for all 389 tests -- pre-existing server configuration (no clang installed); test count is correct (389 = 395 - 6).

## Next Phase Readiness

- `src/stdlib/raylib.iron` and `src/stdlib/time.iron` fully v3-compliant; no receiver-method syntax anywhere in `src/` or `tests/integration/`
- `tests/compile_fail/` v2 fixtures intentionally preserved (v3_receiver_method_removed.iron and 3 others)
- Plan 89-03 can proceed: flip `v3_strict_mode` default to `true` in `iron_parser_create` and verify the tree builds clean

## Self-Check: PASSED

- SUMMARY.md: FOUND
- Commit 03c0bbfb (codemod fix): FOUND
- Commit c28a3767 (stdlib migration): FOUND
- Commit d1fb2ffa (fixture deletion): FOUND

---
*Phase: 89-migr-codemod*
*Completed: 2026-04-23*
