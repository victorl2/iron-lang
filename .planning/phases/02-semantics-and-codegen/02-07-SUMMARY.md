---
phase: 02-semantics-and-codegen
plan: 07
subsystem: testing
tags: [iron, compiler, pipeline, unity, integration-tests, analyzer, codegen]

# Dependency graph
requires:
  - phase: 02-06
    provides: monomorphization, generics, vtable emission — codegen is feature-complete
  - phase: 02-05
    provides: C code generator with structs, functions, enums, nullable, defer
  - phase: 02-04
    provides: escape analysis and concurrency checking
  - phase: 02-03
    provides: type checker
  - phase: 02-02
    provides: two-pass name resolver
  - phase: 02-01
    provides: Iron_Type system and scope tree
provides:
  - iron_analyze() unified entry point running all 4 semantic passes in order
  - test_pipeline.c with 12 end-to-end pipeline tests using iron_analyze
  - 6 integration test fixtures (hello, variables, functions, objects, nullable, control_flow)
  - run_integration.sh pattern-based verification script
  - print/println builtin registration in global scope
affects:
  - Phase 3 runtime and stdlib (builtins API pattern established here)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Unified analyzer entry point: iron_analyze() runs resolve->typecheck->escape->concurrency"
    - "Builtin registration: print/println registered as IRON_SYM_FUNCTION in global scope before Pass 1a"
    - "Pipeline test helper: compile_iron() in test_pipeline.c owns its arena lifecycle"
    - "Integration fixtures: .expected files contain C patterns (not Iron source) for grep-based verification"

key-files:
  created:
    - src/analyzer/analyzer.h
    - src/analyzer/analyzer.c
    - tests/test_pipeline.c
    - tests/integration/hello.iron
    - tests/integration/hello.expected
    - tests/integration/variables.iron
    - tests/integration/variables.expected
    - tests/integration/functions.iron
    - tests/integration/functions.expected
    - tests/integration/objects.iron
    - tests/integration/objects.expected
    - tests/integration/nullable.iron
    - tests/integration/nullable.expected
    - tests/integration/control_flow.iron
    - tests/integration/control_flow.expected
    - tests/integration/run_integration.sh
  modified:
    - src/analyzer/resolve.c
    - CMakeLists.txt

key-decisions:
  - "iron_analyze() early-exits after resolve errors (before typecheck) and after typecheck errors (before escape/concurrency) to avoid cascading failures on incomplete AST"
  - "print/println registered as func(String)->Void builtins in global scope so resolver and typechecker accept them; codegen special-cases them to printf()"
  - "Pipeline test compile_iron() helper keeps its own arena alive (intentionally not freed) so the returned C string pointer remains valid for test assertions"
  - "Integration .expected files contain C output patterns (Iron_ prefix names, C types) rather than Iron source — grep-based verification"

patterns-established:
  - "Unified analyzer pattern: single iron_analyze() call instead of chaining resolve/typecheck/escape/concurrency manually"
  - "Builtin function pattern: register builtins as real symbols in global scope with correct types, let codegen handle emission specially"

requirements-completed: [TEST-01, TEST-02, GEN-01]

# Metrics
duration: 6min
completed: 2026-03-25
---

# Phase 02 Plan 07: Pipeline Integration and Tests Summary

**Unified iron_analyze() entry point plus 12 end-to-end pipeline tests and 6 .iron integration fixtures proving Iron compiles to correct C**

## Performance

- **Duration:** 6 min
- **Started:** 2026-03-25T03:45:28Z
- **Completed:** 2026-03-25T03:51:28Z
- **Tasks:** 2
- **Files modified:** 18

## Accomplishments

- Created `iron_analyze()` unified entry point that runs resolve, typecheck, escape analysis, and concurrency checks in sequence with early-exit on errors
- Added `test_pipeline.c` with 12 end-to-end tests covering: hello, variables, arithmetic, if/elif/else, while, functions, objects, methods, and error cases (E0200, E0203)
- Created 6 integration test fixtures (hello, variables, functions, objects, nullable, control_flow) with C pattern .expected counterparts
- Fixed missing print/println builtin registration so the full pipeline accepts println calls without resolver errors

## Task Commits

1. **Task 1: Create unified analyzer** - `b21e21a` (feat)
2. **Task 2: Pipeline tests and integration fixtures** - `3306ad0` (feat)

## Files Created/Modified

- `src/analyzer/analyzer.h` - Iron_AnalyzeResult struct and iron_analyze() declaration
- `src/analyzer/analyzer.c` - Unified pipeline: resolve->typecheck->escape->concurrency
- `src/analyzer/resolve.c` - Added builtin registration (print, println) in global scope
- `tests/test_pipeline.c` - 12 end-to-end pipeline tests using iron_analyze()
- `tests/integration/hello.iron` - Hello world fixture
- `tests/integration/hello.expected` - Expected C patterns
- `tests/integration/variables.iron` - val/var inference and immutability test
- `tests/integration/variables.expected` - Expected const int64_t / int64_t patterns
- `tests/integration/functions.iron` - Function with params and return
- `tests/integration/functions.expected` - Expected int64_t Iron_add prototype
- `tests/integration/objects.iron` - Object with fields and method
- `tests/integration/objects.expected` - Expected typedef struct, double fields, self->
- `tests/integration/nullable.iron` - Nullable Int? with null check
- `tests/integration/nullable.expected` - Expected Iron_Optional, has_value patterns
- `tests/integration/control_flow.iron` - if/elif/else and while
- `tests/integration/control_flow.expected` - Expected if (, else if (, while ( patterns
- `tests/integration/run_integration.sh` - Shell script for pattern-based C verification
- `CMakeLists.txt` - Added analyzer.c to library, added test_pipeline target

## Decisions Made

- `iron_analyze()` early-exits after resolver errors and after typecheck errors separately — prevents cascading failures on an AST that may have unresolved names or wrong types
- `print`/`println` registered as `func(String)->Void` builtins in the global scope during `iron_resolve()` (before Pass 1a). This matches how codegen already handles them specially. A future stdlib plan will replace these with proper runtime functions.
- The pipeline test `compile_iron()` helper intentionally does not free its arena — the returned C string is arena-allocated and must remain valid for test assertions

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Registered print/println as builtins in global scope**
- **Found during:** Task 2 (test_pipeline_hello failing in RED step)
- **Issue:** `println("hello")` caused resolver to emit E0200 (undefined identifier) then typecheck returned ERROR type; pipeline returned NULL for any Iron program using print/println
- **Fix:** Added builtin symbol registration in `iron_resolve()` after global scope creation: registers `print` and `println` as `IRON_SYM_FUNCTION` with type `func(String)->Void`; codegen already handles them as printf() stubs
- **Files modified:** `src/analyzer/resolve.c`
- **Verification:** `test_pipeline_hello` passes; all 14 tests pass including all prior resolver/typecheck tests
- **Committed in:** `3306ad0` (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 2 - missing critical functionality)
**Impact on plan:** Required fix — print/println are fundamental to every Iron program. No scope creep; the codegen special-case was already present, the resolver/typecheck just lacked the corresponding builtin registration.

## Issues Encountered

None beyond the auto-fixed builtin registration.

## Next Phase Readiness

- Phase 2 complete: Iron source -> lex -> parse -> analyze -> codegen -> C string pipeline fully working
- 14 unit tests pass covering all Phase 2 subsystems
- 6+ .iron integration fixtures serve as living specification
- Phase 3 (Runtime + Stdlib + CLI) can build on top of this pipeline to add: runtime library, actual println implementation, CLI driver that writes C to disk and invokes clang

---
*Phase: 02-semantics-and-codegen*
*Completed: 2026-03-25*
