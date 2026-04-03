---
phase: 39-module-completions-math-io-time-log
plan: "04"
subsystem: compiler
tags: [iron-stdlib, log, emit_c, codegen, constants]

# Dependency graph
requires:
  - phase: 39-module-completions-math-io-time-log
    provides: iron_log.c Iron_log_set_level already implemented

provides:
  - log.iron with DEBUG/INFO/WARN/ERROR val fields and set_level func declaration
  - iron_log.h Iron_Log_DEBUG/INFO/WARN/ERROR #define macros
  - iron_math.h Iron_Math_PI/TAU/E #define macros
  - emit_c.c val_is_type_ref() and correct type-name field access emission

affects:
  - phase-39-plan-05 (log integration test uses Log.set_level and Log.WARN)
  - any Iron program using Math.PI / Math.TAU / Math.E

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Type-name field access pattern: Iron_TypeName_FieldName #defines in module headers resolve object constant field accesses (Math.PI -> Iron_Math_PI)"

key-files:
  created: []
  modified:
    - src/stdlib/log.iron
    - src/stdlib/iron_log.h
    - src/stdlib/iron_math.h
    - src/lir/emit_c.c

key-decisions:
  - "emit_c.c GET_FIELD must detect FUNC_REF objects (type names) and emit Iron_TypeName_FieldName instead of Iron_TypeName.FieldName — the latter is invalid C"
  - "Iron_Log_DEBUG/INFO/WARN/ERROR are #defined as (int64_t)IRON_LOG_xxx in iron_log.h — these map enum values to int64_t matching Iron Int type"
  - "Iron_Math_PI/TAU/E are #defined as IRON_PI/IRON_TAU/IRON_E in iron_math.h — same pattern, no new values needed"
  - "val_is_type_ref() helper added to emit_c.c: a GET_FIELD whose object value is IRON_LIR_FUNC_REF is a type-namespace access, not a runtime struct access"

patterns-established:
  - "Object constant field pattern: declare val fields in .iron object body + add #define Iron_TypeName_FieldName macros in .h header — compiler emits the underscore form"

requirements-completed: [LOG-01, LOG-02]

# Metrics
duration: 35min
completed: 2026-04-02
---

# Phase 39 Plan 04: Log Level Constants and set_level Summary

**Log.set_level(Log.WARN) callable from Iron with correct constant values via compiler bug fix — emit_c.c now emits Iron_Log_WARN instead of invalid Iron_Log.WARN C**

## Performance

- **Duration:** ~35 min
- **Started:** 2026-04-02T22:45:00Z
- **Completed:** 2026-04-02T23:20:00Z
- **Tasks:** 1
- **Files modified:** 4

## Accomplishments
- Updated log.iron with 4 val constant fields (DEBUG/INFO/WARN/ERROR) and set_level function declaration
- Added Iron_Log_DEBUG/INFO/WARN/ERROR #define macros to iron_log.h mapping to IRON_LOG_xxx enum values as int64_t
- Fixed compiler bug in emit_c.c: type-name object field accesses (Math.PI, Log.DEBUG) now emit Iron_TypeName_FieldName (valid C) instead of Iron_TypeName.FieldName (invalid C)
- Also fixed Math.PI/TAU/E as a consequence of the same compiler fix
- Verified: Log.set_level(Log.WARN) suppresses DEBUG and INFO, shows WARN output

## Task Commits

Each task was committed atomically:

1. **Task 1: Investigate constant field emission and update log.iron** - `d79a6f0` (feat)

**Plan metadata:** (pending after SUMMARY commit)

## Files Created/Modified
- `src/stdlib/log.iron` - Added val DEBUG/INFO/WARN/ERROR fields and func Log.set_level
- `src/stdlib/iron_log.h` - Added Iron_Log_DEBUG/INFO/WARN/ERROR #define macros
- `src/stdlib/iron_math.h` - Added Iron_Math_PI/TAU/E #define macros (same pattern fix)
- `src/lir/emit_c.c` - Added val_is_type_ref() helper; fixed GET_FIELD emission for type-name field accesses in both emit_instr and emit_expr_to_buf paths

## Decisions Made

1. **Compiler fix required (Rule 1 auto-fix):** Investigation revealed the compiler emits `Iron_Log.DEBUG` for `Log.DEBUG` which is invalid C (can't do struct member access on a type name). This is a pre-existing compiler bug that blocked the feature. Fixed in emit_c.c by adding `val_is_type_ref()` to detect FUNC_REF objects and emit `Iron_TypeName_FieldName` instead.

2. **#define macro format:** `Iron_Log_DEBUG = ((int64_t)IRON_LOG_DEBUG)` — cast to int64_t to match Iron's Int type, using the existing Iron_LogLevel enum values as the source of truth.

3. **Math constants also fixed:** Same compiler fix resolves Math.PI/TAU/E. Added corresponding `#define Iron_Math_PI IRON_PI` etc. to iron_math.h. No existing tests used Math.PI (confirming the bug was pre-existing and untested).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed compiler GET_FIELD emission for type-name objects**
- **Found during:** Task 1 (investigation step)
- **Issue:** The compiler emitted `Iron_Log.DEBUG` (type-name.field syntax) which is invalid C. The probe `Math.sin(Math.PI)` confirmed with `error: unexpected type name 'Iron_Math': expected expression`.
- **Fix:** Added `val_is_type_ref()` helper in emit_c.c; both GET_FIELD cases (emit_instr and emit_expr_to_buf) now detect FUNC_REF objects and emit `Iron_TypeName_FieldName` underscore form instead.
- **Files modified:** src/lir/emit_c.c, src/stdlib/iron_math.h (also added Math constant macros as part of same fix)
- **Verification:** Probe `Math.sin(Math.PI)` compiles and runs correctly; Log.set_level(Log.WARN) compiles and filters correctly
- **Committed in:** d79a6f0 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 - compiler bug blocking feature)
**Impact on plan:** The auto-fix was necessary to make the feature work at all. It also fixes Math.PI/TAU/E as a side effect. No scope creep — the fix is minimal (one helper + two case additions in emit_c.c).

## Issues Encountered

The research for this plan (RESEARCH.md Pitfall 3) predicted the emission might be `Iron_Log_DEBUG` and suggested `#define Iron_Log_DEBUG 0`. The actual emission was `Iron_Log.DEBUG` (dot notation on a type name) which is invalid C. The investigation phase caught this before attempting a broken implementation. The compiler fix is the correct solution.

## Next Phase Readiness
- Plan 05 (log integration test) can use `Log.set_level(Log.WARN)` and `Log.DEBUG/INFO/WARN/ERROR`
- Math.PI/TAU/E also now work, unblocking any Iron programs that use these constants
- No blockers

---
*Phase: 39-module-completions-math-io-time-log*
*Completed: 2026-04-02*
