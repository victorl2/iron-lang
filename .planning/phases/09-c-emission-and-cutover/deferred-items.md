# Deferred Items — Phase 09

## Out-of-scope issues discovered during 09-04 execution

### 1. Snapshot test stale after 09-03 label rename

**Discovered during:** Task 2 verification (ctest run)
**Test:** `test_ir_lower` — `test_snapshot_if_else`
**Issue:** Commit `d5c91fc` (plan 09-03) renamed block labels from dots to underscores
(`if.then` → `if_then`, `if.else` → `if_else`, etc.) in lower_stmts.c but did not
update the snapshot file `tests/ir/snapshots/if_else.expected`.
**Fix needed:** Update `tests/ir/snapshots/if_else.expected` to use underscore labels.
**Files:** `tests/ir/snapshots/if_else.expected`
**Cause:** pre-existing from 09-03, not introduced by 09-04
