---
phase: 05-codegen-fixes-stdlib-wiring
plan: 03
subsystem: stdlib
tags: [iron, codegen, stdlib, math, io, time, log, auto-static, method-dispatch]

# Dependency graph
requires:
  - phase: 04-comptime-game-dev-and-cross-platform
    provides: raylib import detection pattern in build.c, extern func codegen, iron_snake_to_camel
  - phase: 03-runtime-stdlib-and-cli
    provides: iron_math.h, iron_io.h, iron_time.h, iron_log.h C implementations linked into every binary
provides:
  - Four .iron wrapper files (math, io, time, log) importable from Iron source
  - Auto-static method dispatch in codegen: Math.sin(x) -> Iron_math_sin(x)
  - Type-level field access: Math.PI -> IRON_PI
  - build.c import detection for import math/io/time/log with .iron source prepend
  - Stdlib header includes in generated C
  - Fixed METHOD_CALL return type resolution in typecheck.c
affects: [any Iron code using Math/IO/Time/Log stdlib modules]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Auto-static dispatch: type-receiver method call (IRON_SYM_TYPE) emits Iron_<lower(type)>_<method>(args...) without self pointer"
    - "Auto-static field: type-receiver field access emits IRON_<UPPER(field)> C macro"
    - "Empty-body method guard: methods with zero statements skipped in prototype/impl emission"
    - "Import detection: strstr-based source detection in build.c with .iron prepend, same as raylib pattern"

key-files:
  created:
    - src/stdlib/math.iron
    - src/stdlib/io.iron
    - src/stdlib/time.iron
    - src/stdlib/log.iron
  modified:
    - src/cli/build.c
    - src/codegen/codegen.c
    - src/codegen/gen_exprs.c
    - src/analyzer/typecheck.c

key-decisions:
  - "05-03: Stdlib .iron wrappers use top-level func Math.method() syntax (not method decls inside object body) because parser only accepts val/var fields inside object blocks"
  - "05-03: Auto-static dispatch triggered by IRON_SYM_TYPE on receiver symbol, not resolved_type kind — allows type-level method calls without instance allocation"
  - "05-03: Empty-body method guard uses stmt_count==0 check on Iron_Block* body to skip stdlib wrapper methods in prototype/impl emission loops"
  - "05-03: METHOD_CALL return type fixed in typecheck.c by adding program pointer to TypeCtx and scanning program->decls for matching type+method name"
  - "05-03: Math constant fields (PI, TAU, E) declared without initializers in .iron wrapper — IRON_PI etc. macros emitted directly by auto-static field access path"

patterns-established:
  - "Iron_%s_%s naming convention: Iron_ + lowercase(TypeName) + _ + method_name matches stdlib C function names"
  - "IRON_%s naming convention: IRON_ + UPPER(field_name) matches stdlib C macro names"
  - "TypeCtx.program field enables method return type lookup in typecheck.c check_expr"

requirements-completed: [STD-01, STD-02, STD-03, STD-04]

# Metrics
duration: 7min
completed: 2026-03-26
---

# Phase 5 Plan 03: Stdlib Wiring Summary

**Four stdlib modules (math, io, time, log) importable via Iron source with auto-static dispatch: Math.sin(1.0) -> Iron_math_sin(1.0) and Math.PI -> IRON_PI**

## Performance

- **Duration:** 7 min
- **Started:** 2026-03-26T19:41:45Z
- **Completed:** 2026-03-26T19:49:35Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- Created four .iron wrapper files declaring object types with empty-body methods at top-level using `func Math.sin()` syntax
- Extended build.c with strstr-based import detection for math/io/time/log, prepending .iron source before lex phase
- Implemented auto-static method dispatch in gen_exprs.c: IRON_SYM_TYPE receiver triggers Iron_math_sin-style direct C call
- Added auto-static field access for constants (Math.PI -> IRON_PI) in FIELD_ACCESS case
- Added stdlib header includes (iron_math.h, iron_io.h, iron_time.h, iron_log.h) unconditionally in codegen.c
- Fixed METHOD_CALL return type resolution in typecheck.c (was always void; now scans program method decls)
- All four modules verified: `Math.sin`, `Time.now_ms`, `Log.info`, `IO.file_exists` all compile and run

## Task Commits

1. **Task 1: Create stdlib .iron wrapper files and extend build.c import detection** - `dcd0b38` (feat)
2. **Task 2: Implement auto-static method dispatch and verify Math.sin works** - `1ddaf9c` (feat)

## Files Created/Modified
- `src/stdlib/math.iron` - Math object with PI/TAU/E fields and empty-body trig methods
- `src/stdlib/io.iron` - IO object with empty-body file operation methods
- `src/stdlib/time.iron` - Time object with empty-body timing methods
- `src/stdlib/log.iron` - Log object with empty-body logging methods
- `src/cli/build.c` - Added import detection and .iron prepend for math/io/time/log
- `src/codegen/codegen.c` - Added stdlib header includes + empty-body method guard
- `src/codegen/gen_exprs.c` - Auto-static dispatch in METHOD_CALL and FIELD_ACCESS cases
- `src/analyzer/typecheck.c` - Fixed METHOD_CALL return type using program method decl scan

## Decisions Made
- Stdlib .iron wrappers use top-level `func Math.method()` syntax (not inside object body) because the parser only supports `val`/`var` field declarations inside object blocks
- Auto-static dispatch is triggered by `IRON_SYM_TYPE` on the receiver symbol, allowing `Math.sin()` to call `Iron_math_sin()` without needing an instance
- Empty-body methods (stmt_count == 0) are guarded in both prototype and implementation emit loops to avoid broken C stubs
- `TypeCtx` gained a `program` pointer so `check_expr` can scan method decls for return type resolution — fixes previously-void return type for method calls

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Stdlib .iron wrapper method syntax: object body doesn't support method declarations**
- **Found during:** Task 1 (testing compiled output)
- **Issue:** Parser emits `expected field declaration (val or var)` for `func sin(...)` inside object body because object parser only accepts `val`/`var` declarations
- **Fix:** Changed all four .iron wrappers to use top-level `func Math.method()` declaration syntax instead of inline method declarations
- **Files modified:** src/stdlib/math.iron, io.iron, time.iron, log.iron
- **Verification:** `import math` compiles without parse errors
- **Committed in:** 1ddaf9c (Task 2 commit)

**2. [Rule 1 - Bug] METHOD_CALL type checker always returns void causing `const void x = Iron_math_sin(1.0)` in generated C**
- **Found during:** Task 2 (testing `val x = Math.sin(1.0)`)
- **Issue:** `check_expr` for `IRON_NODE_METHOD_CALL` always returned `IRON_TYPE_VOID`; when the result is assigned to a `val`, C emits `const void x = ...` which is a clang error
- **Fix:** Added program pointer to TypeCtx; METHOD_CALL case now scans program->decls for matching Iron_MethodDecl and uses its resolved_return_type
- **Files modified:** src/analyzer/typecheck.c
- **Verification:** `val x = Math.sin(1.0)` compiles and runs; all four stdlib modules tested
- **Committed in:** 1ddaf9c (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (both Rule 1 bugs)
**Impact on plan:** Both fixes were required for the plan's core feature to work. No scope creep.

## Issues Encountered
None beyond the two auto-fixed bugs above.

## Next Phase Readiness
- All four stdlib modules (math, io, time, log) are importable and functional
- Auto-static dispatch pattern established and verified — any future type-level module follows the same `Iron_<lower>_<method>` convention
- METHOD_CALL return type resolution now correct for both auto-static and instance methods
- Integration tests pass (0 regressions)

---
*Phase: 05-codegen-fixes-stdlib-wiring*
*Completed: 2026-03-26*
