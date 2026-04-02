---
phase: 37-compiler-dispatch-fixes-technical-debt
created: 2026-04-02
---

# Deferred Items

## Pre-existing Build Failures (Out of Scope for Plan 37-04)

**Discovered during:** Task 1 (WINDOWS-TODO comment additions)

**Issue:** Multiple unit test files call `iron_runtime_init()` with zero arguments but the function signature now requires `(int argc, char **argv)`. This causes compile errors in:
- `tests/unit/test_runtime_string.c` line 10
- `tests/unit/test_runtime_threads.c` line 16
- `tests/unit/test_runtime_collections.c` line 16
- `tests/unit/test_stdlib.c` line 17

**Why out of scope:** These failures existed before Plan 37-04 began (confirmed by git stash verification). Plan 37-04 only adds documentation comments to stdlib source files; it does not change any runtime headers or test infrastructure.

**Suggested fix:** Update each affected `setUp()` function to pass `(0, NULL)`:
```c
void setUp(void) { iron_runtime_init(0, NULL); }
```

**Note:** The stdlib targets (`iron_io.c`, `iron_log.c`, `iron_math.c`) all compile successfully — only the unit test executables are affected.
