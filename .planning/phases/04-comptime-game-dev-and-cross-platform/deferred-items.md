# Deferred Items — Phase 04

## comptime_basic integration test failure

**Discovered during:** 04-03 Task 3
**File:** tests/integration/comptime_basic.iron

The `comptime_basic` integration test was created during plan 04-02 but not committed.
It fails because global `val GREETING = comptime "hello comptime"` doesn't have its
comptime-evaluated value properly referenced in generated C — the identifier `GREETING`
appears unresolved as a bare name instead of the computed string value.

This is pre-existing and out of scope for 04-03. The fix requires extending comptime
evaluation to handle global-level val declarations, not just local ones.

**Status:** Needs investigation in a future plan or hotfix session.
