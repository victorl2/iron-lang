---
phase: 03-runtime-stdlib-and-cli
plan: "04"
subsystem: codegen
tags: [codegen, runtime, parallel-for, builtins, threading]

requires:
  - phase: 03-01
    provides: iron_runtime.h with Iron_String, iron_runtime_init/shutdown, Iron_println
  - phase: 03-02
    provides: Iron_Pool, Iron_pool_thread_count, Iron_pool_submit, Iron_pool_barrier

provides:
  - Generated C output includes runtime/iron_runtime.h giving all runtime types/functions
  - parallel-for uses Iron_pool_thread_count(Iron_global_pool) for dynamic chunk sizing
  - main() wrapper calls iron_runtime_init() and iron_runtime_shutdown()
  - Builtin functions len, min, max, clamp, abs, assert registered in resolver scope

affects: [03-05, 03-06, 03-07, 03-08]

tech-stack:
  added: []
  patterns:
    - Generated C includes runtime header first so all runtime types are available
    - Builtins registered in resolver before Pass 1a so call sites resolve without errors
    - Dynamic parallel-for chunks: Iron_pool_thread_count queries actual pool size at runtime

key-files:
  created: []
  modified:
    - src/codegen/codegen.c
    - src/codegen/gen_stmts.c
    - src/analyzer/resolve.c
    - tests/test_codegen.c

key-decisions:
  - "Runtime header emitted first in includes section so transitively included stdlib headers do not create conflicts with the explicit stdint.h etc. that follow"
  - "Builtins (len/min/max/clamp/abs/assert) registered in resolver with simplified signatures (Int/String params) — type-checker only verifies IRON_SYM_FUNCTION exists, not full signature matching"
  - "Dynamic parallel-for: _nthreads variable introduced before _chunk_size so chunk count adapts to pool configuration at runtime rather than compile-time constant"

patterns-established:
  - "All future codegen builtins must be registered in both resolve.c (for analysis) and gen_exprs.c (for emission) — registration in only one place causes undefined-identifier errors at analysis time"

requirements-completed: [RT-08]

duration: 3min
completed: 2026-03-25
---

# Phase 3 Plan 4: Codegen Runtime Integration Summary

**Generated C now includes runtime/iron_runtime.h, parallel-for uses dynamic thread count via Iron_pool_thread_count, and builtins (len/min/max/clamp/abs/assert) are registered in the resolver so all runtime features compile cleanly**

## Performance

- **Duration:** 3 min
- **Started:** 2026-03-25T18:07:19Z
- **Completed:** 2026-03-25T18:10:39Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments

- Added `#include "runtime/iron_runtime.h"` as the first include in all generated C output, giving Iron programs access to Iron_String, Iron_println, iron_runtime_init/shutdown, Iron_Pool and all collection types without manual header injection
- Fixed parallel-for chunk sizing from hardcoded `(_total + 3) / 4` to dynamic `Iron_pool_thread_count(Iron_global_pool)` so work is distributed across the actual number of pool threads at runtime
- Registered 6 missing builtin functions (len, min, max, clamp, abs, assert) in the resolver so programs calling these functions pass analysis without undefined-identifier errors
- Added 5 new codegen integration tests verifying runtime header inclusion, Iron_println usage, init/shutdown in main(), dynamic parallel-for chunks, and Iron_len builtin emission

## Task Commits

Each task was committed atomically:

1. **Task 1: Fix codegen includes and parallel-for chunk calculation** - `096e18f` (feat)
2. **Task 2: Update codegen tests to verify runtime integration** - `b1ec1cb` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified

- `src/codegen/codegen.c` - Replaced standard-includes-only preamble with runtime header first, removed stale TODO comment
- `src/codegen/gen_stmts.c` - Replaced hardcoded 4-chunk parallel-for split with dynamic Iron_pool_thread_count query
- `src/analyzer/resolve.c` - Registered len, min, max, clamp, abs, assert as IRON_SYM_FUNCTION builtins before Pass 1a
- `tests/test_codegen.c` - Added 5 runtime integration tests: includes_runtime, print_uses_iron_print, main_has_init_shutdown, parallel_for_dynamic_chunks, builtin_len

## Decisions Made

- Runtime header emitted first in the includes section so that if the runtime header itself pulls in stdint.h etc. via its own includes, the subsequent explicit `#include <stdint.h>` lines are harmless no-ops (include guards)
- Builtin registration uses simplified signatures (all params as Int or String) because the current type-checker only checks that a matching IRON_SYM_FUNCTION symbol exists in scope — full overload resolution is a Phase 4 concern
- Dynamic parallel-for introduces a named `_nthreads` variable (not an inline expression) to keep the chunk-size formula readable and allow future instrumentation

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Registered builtin functions len/min/max/clamp/abs/assert in resolver**
- **Found during:** Task 2 (test_codegen_builtin_len test)
- **Issue:** The `len` builtin (and all other builtins except print/println) was handled by codegen emission in gen_exprs.c but never registered as a symbol in the resolver. Any Iron program calling `len(s)` emitted an undefined-identifier error, causing iron_codegen() to return NULL.
- **Fix:** Added resolver registration for all 6 codegen-handled builtins with appropriate type signatures in src/analyzer/resolve.c alongside the existing print/println registration block
- **Files modified:** src/analyzer/resolve.c
- **Verification:** test_codegen_builtin_len passes; all 17 tests pass with zero regressions
- **Committed in:** b1ec1cb (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 missing critical)
**Impact on plan:** Auto-fix essential for correctness — without it, any Iron program using len/min/max/clamp/abs/assert would fail to compile. No scope creep.

## Issues Encountered

None — the builtin registration gap was caught during Task 2 test execution and fixed inline.

## Next Phase Readiness

- Generated C output is now self-contained: includes the runtime header plus standard headers
- parallel-for correctly utilizes all pool threads
- All codegen builtins are both emittable and resolvable
- Ready for Plan 05 (stdlib or collections integration)

---
*Phase: 03-runtime-stdlib-and-cli*
*Completed: 2026-03-25*
