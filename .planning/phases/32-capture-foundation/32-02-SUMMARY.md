---
phase: 32-capture-foundation
plan: 02
subsystem: compiler
tags: [closures, lambda, capture, lir, hir, emit-c, optimizer, iron-closure]

# Dependency graph
requires:
  - phase: 32-capture-foundation/32-01
    provides: Iron_CaptureEntry populated on Iron_LambdaExpr nodes by capture analysis

provides:
  - Iron_Closure fat pointer typedef and IRON_CALL_CLOSURE macro in runtime header
  - HIR closure struct extended with captures/capture_count from AST capture analysis
  - LIR MAKE_CLOSURE captures[] populated with real ValueIds from hir_to_lir
  - IronLIR_Func.capture_metadata wired for lifted lambda functions
  - emit_c.c generates __lambda_N_env_t typedef with named, typed fields
  - emit_c.c emits Iron_Closure fat pointer literals for all closures (capturing and non)
  - Closure call sites dispatch through fn+env pair via explicit C cast expressions
  - Function inliner skips all __lambda_N functions
  - Optimizer guards for capture-aliased allocas (copy-prop and store-load-elim)
  - Dead-alloca-elim extended to treat MAKE_CLOSURE captures as address escapes

affects:
  - 32-capture-foundation/32-03
  - any future phase involving closures, higher-order functions, iterators

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Iron_Closure fat pointer: { void *env; void (*fn)(void*); } for all closure values"
    - "__lambda_N_env_t typedef in C struct_bodies section with named fields per capture"
    - "Capture alias alloca: lifted function allocas matching capture names treated as opaque by optimizer"
    - "Escape set extended for MAKE_CLOSURE operands to prevent dead-alloca-elim on var captures"

key-files:
  created: []
  modified:
    - src/runtime/iron_runtime.h
    - src/hir/hir.h
    - src/hir/hir.c
    - src/hir/hir_lower.c
    - src/hir/hir_to_lir.c
    - src/lir/lir.h
    - src/lir/emit_c.c
    - src/lir/lir_optimize.c
    - src/analyzer/capture.c

key-decisions:
  - "Use Iron_Closure fat pointer for all closures (capturing and non-capturing) for uniform call dispatch"
  - "Read id->resolved_type instead of id->resolved_sym->type for capture type: resolver symbols have type=NULL, types are annotated on ident nodes by typechecker"
  - "Capture-alias allocas in lifted functions are opaque to optimizer: skip copy-prop and store-load forwarding for them"
  - "MAKE_CLOSURE captures treated as address escapes in compute_escape_set to protect var-capture outer allocas from dead-alloca-elim"
  - "Closure call sites emit explicit C cast expressions rather than IRON_CALL_CLOSURE macro to handle return types and multiple arguments"

patterns-established:
  - "alloca_is_capture_alias(): check by name_hint match against fn->capture_metadata before any store-forwarding optimization"
  - "compute_escape_set() must be updated whenever new LIR instructions can take alloca addresses"

requirements-completed: [FOUND-02, FOUND-03, FOUND-04]

# Metrics
duration: 85min
completed: 2026-04-02
---

# Phase 32 Plan 02: Capture Foundation - Iron_Closure Wiring Summary

**Iron_Closure fat pointer wired end-to-end: typed env structs with named fields emitted from capture analysis metadata, closure calls dispatched through fn+env pair, optimizer hardened against capture-alias aliasing hazards**

## Performance

- **Duration:** ~85 min
- **Started:** 2026-04-02T19:43:51Z
- **Completed:** 2026-04-02T21:09:20Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments

- Replaced bare `void*` closure representation with `Iron_Closure { void *env; void (*fn)(void*); }` across all callsites in generated C
- Wired capture metadata from AST through HIR and LIR to emit_c.c, producing `__lambda_N_env_t` typedef structs with named and typed fields (e.g., `Iron_String name` instead of `void* _cap0`)
- Fixed three root-cause optimizer bugs that corrupted lifted lambda functions: wrong type source in capture analysis, copy-prop forwarding through env-aliased allocas, and dead-alloca-elim removing var-capture outer-function allocas

## Task Commits

Each task was committed atomically:

1. **Task 1: Iron_Closure typedef, HIR capture wiring, and function inliner guard** - `bcf8b89` (feat)
2. **Task 2: LIR capture wiring, emit_c.c env struct generation, and optimizer hardening** - `d325716` (feat)

**Plan metadata:** (this commit) (docs)

## Files Created/Modified

- `src/runtime/iron_runtime.h` - Added `Iron_Closure` typedef and `IRON_CALL_CLOSURE` macro
- `src/hir/hir.h` - Extended IRON_HIR_EXPR_CLOSURE with `captures` and `capture_count` fields; updated constructor declaration
- `src/hir/hir.c` - Updated `iron_hir_expr_closure` to accept and store `captures`/`capture_count`
- `src/hir/hir_lower.c` - Pass 2 wires AST capture entries to HIR closure; Pass 3 prepends `_env` param and emits synthetic LET stmts for captured variable access
- `src/hir/hir_to_lir.c` - Populates LIR MAKE_CLOSURE `captures[]` with real ValueIds; populates `IronLIR_Func.capture_metadata` for lifted lambda funcs
- `src/lir/lir.h` - Added `capture_metadata`/`capture_count` to `IronLIR_Func`; added `IronLIR_CaptureEntry` struct
- `src/lir/emit_c.c` - Full closure emission: env struct typedef, env heap allocation, `Iron_Closure` literal, closure call dispatch with explicit C cast, capture-alias map for lifted functions
- `src/lir/lir_optimize.c` - Inliner guard for `__lambda_*`; `alloca_is_capture_alias()` helper; guards in copy-prop and store-load-elim; MAKE_CLOSURE escape tracking in `compute_escape_set()`
- `src/analyzer/capture.c` - Fixed capture type source: use `id->resolved_type` (typechecker-annotated) instead of `id->resolved_sym->type` (always NULL for variables)

## Decisions Made

- **Iron_Closure for all closures:** Non-capturing lambdas also use `Iron_Closure` with `.env = NULL`. This simplifies call dispatch — all closure values have identical representation.
- **Typed capture fields in env struct:** Env struct fields use the actual captured variable types (`Iron_String name`, `int64_t count`) rather than positional `_cap0`, `_cap1` names. Requires correct type propagation from capture analysis.
- **resolved_type vs resolved_sym->type:** The resolver creates symbols without types; the typechecker annotates types onto `Iron_Ident.resolved_type`. Capture analysis must read from the ident node, not the symbol.
- **Capture alias opacity in optimizer:** Allocas in lifted functions that correspond to capture names must be treated as opaque — they represent env struct field aliases, not simple local variables. Single-store copy forwarding would create circular references.
- **Explicit C casts for closure calls:** `IRON_CALL_CLOSURE` macro handles only the zero-arg void case; emit_c.c writes explicit cast expressions for all other cases to handle return types and argument lists without `##__VA_ARGS__`.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed capture type resolution reading NULL-typed resolver symbols**
- **Found during:** Task 2 (emit_c.c env struct emission testing)
- **Issue:** `capture_metadata[ci].type` was always NULL because `capture.c` read `id->resolved_sym->type`. Resolver symbols never have their `type` field set for variable declarations — types are annotated by the typechecker onto `Iron_Ident.resolved_type` on the AST node itself. This caused env structs to emit `void* name` instead of `Iron_String name`.
- **Fix:** Changed `capture.c` line ~150 to `cap.type = id->resolved_type ? id->resolved_type : id->resolved_sym->type`
- **Files modified:** `src/analyzer/capture.c`
- **Verification:** Env struct fields now emit correctly-typed declarations
- **Committed in:** d325716 (Task 2 commit)

**2. [Rule 1 - Bug] Fixed copy propagation forwarding through capture-alias allocas in lifted functions**
- **Found during:** Task 2 (optimizer testing with --dump-ir-passes)
- **Issue:** `run_copy_propagation` counted `store %3, %6` (emitted by the env-unpacking LET stmt in the lifted function body) as ONE store to the capture-alias alloca `%3`. Since there was only one store, copy-prop replaced ALL subsequent loads of `%3` with `%6` — creating a self-referential instruction `%6 = add %6, %5` when the load result was used in a computation.
- **Fix:** Added `alloca_is_capture_alias()` helper and guarded the store-tracking logic in both `run_copy_propagation` and `run_store_load_elim` to skip capture-alias allocas.
- **Files modified:** `src/lir/lir_optimize.c`
- **Verification:** Generated C for mutable captures no longer has self-referential expressions; `--dump-ir-passes` confirms no forwarding through capture-alias allocas
- **Committed in:** d325716 (Task 2 commit)

**3. [Rule 1 - Bug] Fixed dead-alloca-elim removing var-capture outer-function allocas**
- **Found during:** Task 2 (testing var-capture lambda compilation)
- **Issue:** `run_dead_alloca_elimination` used `compute_escape_set()` to determine if an alloca's address is taken. `compute_escape_set()` only checked CALL arguments, STORE values, and RETURN values — it did not check MAKE_CLOSURE captures. The outer-function alloca for a `var` capture appeared to have no address takers and was eliminated, but `_env->field = &_vN` still referenced it, causing a C compiler "undeclared identifier" error.
- **Fix:** Added `IRON_LIR_MAKE_CLOSURE` case to `compute_escape_set()`, marking all closure capture operands as escaped so their allocas are preserved.
- **Files modified:** `src/lir/lir_optimize.c`
- **Verification:** Outer-function var-capture allocas are retained; C compilation succeeds; mutable capture test produces correct output `1\n2\n3`
- **Committed in:** d325716 (Task 2 commit)

---

**Total deviations:** 3 auto-fixed (3 bugs in optimizer/analysis interaction with new closure machinery)
**Impact on plan:** All three fixes were necessary for correctness. The bugs were introduced by the new closure infrastructure interacting with existing optimizer passes that had no knowledge of capture semantics. No scope creep.

## Issues Encountered

- Tracing the `resolved_sym->type = NULL` bug required understanding the compiler's two-phase symbol resolution: the resolver creates symbols (type=NULL for variables), then the typechecker creates its own scope chain and annotates types on ident nodes (`resolved_type`) without back-propagating to resolver symbols.
- Copy propagation corruption was detected by adding `--dump-ir-passes` output showing `%4 = load %3` transforming to `%4 = %6` after copy-prop, which then caused `add %4, %5` to become `add %6, %5` — a self-reference.
- Dead-alloca-elim bug required recognizing that `compute_escape_set()` is a shared utility called by multiple optimizer passes, and extending it is the correct fix rather than patching each individual pass.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Closure representation is fully wired: val captures (String, Int) and var captures (mutable) both work correctly
- All 43/43 tests pass (benchmark_smoke excluded — pre-existing unrelated timeout)
- 169 integration tests pass
- Ready for Plan 32-03 (call-site cleanup, stdlib integration, or additional closure features)
- Known non-critical: K&R empty-prototype warning in non-capturing lambda calls (`((ret_type (*)())closure.fn)(args...)`) — deferred to future cleanup

## Self-Check: PASSED

All files present. All commits verified (bcf8b89, d325716).

---
*Phase: 32-capture-foundation*
*Completed: 2026-04-02*
