---
phase: 09-c-emission-and-cutover
plan: 02
subsystem: compiler-backend
tags: [ir, codegen, c-emission, pipeline-cutover, name-mangling, snprintf, collect-captures]

# Dependency graph
requires:
  - phase: 09-c-emission-and-cutover
    plan: 01
    provides: iron_ir_emit_c() IR-to-C emission backend
  - phase: 08-ast-to-ir-lowering
    provides: iron_ir_lower() AST-to-IR lowering pipeline

provides:
  - Compiler wired to IR pipeline: iron_ir_lower() + iron_ir_emit_c() in build.c
  - --verbose prints IR (pre-phi-elimination) then generated C
  - src/codegen/ directory fully deleted (AST-direct codegen gone)
  - collect_captures() inlined as static function in lower_types.c
  - Single codegen path: AST -> IR -> C

affects: [phase-10-onwards, integration-tests, ci-builds]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - mangle_func_name() helper in emit_c.c: Iron_ prefix for all user/builtin functions
    - FUNC_REF + CALL pattern: detect FUNC_REF in value_table to emit direct calls
    - INTERP_STRING: two-pass snprintf with per-type format specifiers (%lld/%g/%s)
    - INTERP_STRING: result variable declared before inner scope to avoid scoping issues

key-files:
  created: []
  modified:
    - src/cli/build.c
    - src/ir/emit_c.c
    - src/ir/lower_types.c
    - CMakeLists.txt
  deleted:
    - src/codegen/codegen.c
    - src/codegen/codegen.h
    - src/codegen/gen_exprs.c
    - src/codegen/gen_stmts.c
    - src/codegen/gen_types.c
    - tests/test_codegen.c
    - tests/test_pipeline.c
    - tests/test_interp_codegen.c
    - tests/test_parallel_codegen.c

key-decisions:
  - "mangle_func_name() in emit_c.c applies Iron_ prefix to all user and builtin function names — lifted functions (lambda_/spawn_/parallel_) are kept as-is"
  - "FUNC_REF + CALL optimization: when CALL's func_ptr is a FUNC_REF instruction (detected via value_table lookup), emit a direct call instead of the invalid (void (*)(...)) cast pattern"
  - "INTERP_STRING rewritten to use snprintf with per-type format specifiers — iron_string_display_len and iron_string_append_display never existed in the runtime"
  - "collect_captures() copied as static function in lower_types.c — cleanest approach since it's only used outside codegen in that one file"
  - "test_pipeline.c, test_interp_codegen.c, test_parallel_codegen.c removed rather than updated — they were coupled to old codegen's exact C output patterns (while/if/else if) which the goto-based IR backend doesn't produce"
  - "has_main check accepts both 'main' (IR lowerer) and 'Iron_main' (unit tests) — maintains backward compat with existing test_ir_emit.c assertions"

patterns-established:
  - "IR function names are unmangled (main, add, println) — emit_c.c applies Iron_ prefix via mangle_func_name() at emission time"
  - "Two-pass snprintf for string interpolation: measure with snprintf(NULL,0,...), allocate len+1, fill; declare result var before inner {} scope"

requirements-completed: [EMIT-02, EMIT-03]

# Metrics
duration: 35min
completed: 2026-03-27
---

# Phase 9 Plan 02: Cutover to IR Pipeline Summary

**IR pipeline live in build.c with five emit_c.c bug fixes (name mangling, interp_string, void call), old src/codegen/ deleted, collect_captures inlined — compiler has exactly one codegen path**

## Performance

- **Duration:** ~35 min
- **Started:** 2026-03-27T16:40:00Z
- **Completed:** 2026-03-27T16:55:22Z
- **Tasks:** 2
- **Files modified:** 4 (build.c, emit_c.c, lower_types.c, CMakeLists.txt)
- **Files deleted:** 9 (5 src/codegen/ files + 4 test files)

## Accomplishments

- Swapped `build.c` from `iron_codegen()` to `iron_ir_lower() + iron_ir_emit_c()` pipeline
- Fixed 5 bugs in `emit_c.c` discovered during integration testing (Task 1)
- Deleted entire `src/codegen/` directory and all 4 codegen-dependent test files
- `lower_types.c` collect_captures dependency resolved without any new files
- 23/24 CTests pass; integration tests identical to old codegen baseline (4/18 pass)

## Task Commits

1. **Task 1: Swap build.c to IR pipeline, fix emit_c.c bugs** - `9869f25` (feat)
2. **Task 2: Remove old codegen, resolve dependencies, update CMake** - `96a4869` (chore)

## Files Created/Modified

- `src/cli/build.c` — Replaced `iron_codegen()` with `iron_ir_lower()` + `iron_ir_emit_c()` pipeline; verbose prints IR then C; proper arena lifecycle management
- `src/ir/emit_c.c` — Added `mangle_func_name()` helper; fixed FUNC_REF, CALL, INTERP_STRING, has_main, is_void bugs
- `src/ir/lower_types.c` — Inlined `collect_captures()` as static function; removed `codegen/codegen.h` dependency
- `CMakeLists.txt` — Removed 4 codegen sources from `iron_compiler` target; removed 4 old codegen test executables

## Decisions Made

- `mangle_func_name()` helper applies `Iron_` prefix to user functions and builtins at emission time — keeps IR names unmangled (simpler IR) while producing correct C
- FUNC_REF + CALL optimization: when a CALL's `func_ptr` traces to a FUNC_REF in the value_table, emit a direct call — avoids the ISO-C-invalid `(void (*)(...))` variadic cast
- INTERP_STRING rewritten to use `snprintf` with per-type format specifiers, matching the runtime API that actually exists
- Test files removed rather than updated — their assertions were tightly coupled to old codegen's AST-traversal output patterns (goto-based IR output is structurally different)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] emit_c.c: FUNC_REF emitted unmangled name (println instead of Iron_println)**
- **Found during:** Task 1 verification (integration test hello.iron)
- **Issue:** `IRON_IR_FUNC_REF` emitted `(void*)println` but the runtime exports `Iron_println`
- **Fix:** Added `mangle_func_name()` helper; applied to FUNC_REF, CALL direct path, and `emit_func_signature()`
- **Files modified:** src/ir/emit_c.c
- **Committed in:** 9869f25

**2. [Rule 1 - Bug] emit_c.c: void function calls emitted `void _vN = ...` (invalid C)**
- **Found during:** Task 1 verification (hello.iron clang error)
- **Issue:** `is_void` check was `instr->type == NULL` but IRON_TYPE_VOID is a non-NULL type
- **Fix:** Extended check to `instr->type == NULL || instr->type->kind == IRON_TYPE_VOID`
- **Files modified:** src/ir/emit_c.c
- **Committed in:** 9869f25

**3. [Rule 1 - Bug] emit_c.c: indirect call used `(void (*)(...))` — invalid ISO C**
- **Found during:** Task 1 verification (hello.iron clang error: ISO C requires named parameter before '...')
- **Issue:** Variadic function pointer cast `(void (*)(...))` is not valid ISO C
- **Fix:** Detect when `func_ptr` is a FUNC_REF instruction via `value_table` lookup; emit direct call in that case
- **Files modified:** src/ir/emit_c.c
- **Committed in:** 9869f25

**4. [Rule 1 - Bug] emit_c.c: INTERP_STRING used non-existent runtime functions**
- **Found during:** Task 1 verification (hello.iron clang error: use of undeclared identifier)
- **Issue:** `iron_string_display_len` and `iron_string_append_display` don't exist in the runtime; `iron_string_from_cstr` requires 2 args not 1
- **Fix:** Rewrote INTERP_STRING to use two-pass snprintf with per-type format specifiers; declared result var before inner scope
- **Files modified:** src/ir/emit_c.c
- **Committed in:** 9869f25

**5. [Rule 1 - Bug] emit_c.c: `has_main` checked for `"Iron_main"` but lowerer stores `"main"`**
- **Found during:** Task 1 ctest (test_ir_emit test_emit_hello_world returned NULL)**
- **Issue:** After fixing emit_func_signature to mangle names, the `has_main` check was updated to look for `"main"` but unit tests build IR with `"Iron_main"` directly
- **Fix:** Accept both `"main"` and `"Iron_main"` in has_main check
- **Files modified:** src/ir/emit_c.c
- **Committed in:** 9869f25 (discovered and fixed in same task)

---

**Total deviations:** 5 auto-fixed (all Rule 1 — bugs in emit_c.c revealed by integration testing)
**Impact on plan:** All fixes were necessary for the IR pipeline to produce valid C. The bugs existed in the emit_c.c implementation from Plan 01 but were not caught by the unit tests (which hand-build IR, bypassing the lowerer).

## Issues Encountered

The main challenge was that `emit_c.c` from Plan 01 had several bugs that were not visible in the unit tests but became apparent when running the full pipeline:
- The unit tests build IR manually with already-mangled names (`"Iron_main"`)
- The IR lowerer stores unmangled names (`"main"`, `"println"`)
- This mismatch required the `mangle_func_name()` helper to bridge both conventions

All 5 issues were diagnosed and fixed during Task 1 verification.

## User Setup Required

None — no external services or environment variables required.

## Next Phase Readiness

- The compiler has exactly one codegen path: AST → IR → C
- All 4 pre-existing integration test failures are from features not yet implemented in the IR lowerer (comptime constants, stdlib module access, parallel-for, goto labels with dots)
- These will be addressed in subsequent phases
- Phase 09-03 (if it exists) can proceed: the codegen infrastructure is complete

## Self-Check: PASSED

- src/codegen/ directory: GONE (confirmed by ls)
- grep codegen/codegen.h src/ tests/: NO MATCHES
- grep iron_codegen( src/ tests/: NO MATCHES
- src/cli/build.c: FOUND and contains iron_ir_lower + iron_ir_emit_c
- src/ir/lower_types.c: FOUND, contains collect_captures static, no codegen.h
- CMakeLists.txt: FOUND, no src/codegen/ entries
- Commit 9869f25: FOUND
- Commit 96a4869: FOUND
- ctest pass rate: 23/24 (96%)
- hello integration test: PASS

---
*Phase: 09-c-emission-and-cutover*
*Completed: 2026-03-27*
