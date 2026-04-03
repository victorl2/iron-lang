---
gsd_state_version: 1.0
milestone: v0.0
milestone_name: milestone
status: completed
stopped_at: Completed 39-05-PLAN.md
last_updated: "2026-04-03T02:25:24.308Z"
last_activity: "2026-04-02 — Phase 38-03 complete: 24 integration test files created, two compiler bugs fixed (iron_runtime.h declarations, typecheck non-ident receiver handling)"
progress:
  total_phases: 11
  completed_phases: 4
  total_plans: 18
  completed_plans: 17
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-02)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** v0.1.0-alpha Lambda Capture — Phase 38 string built-in methods in progress

## Current Position

Phase: 38 of 38 (String Built-In Methods — ALL PLANS COMPLETE)
Plan: 03 complete — Phase 38 finished
Status: Phase 38 complete — all 19 string method bodies implemented and all 12 integration test pairs passing
Last activity: 2026-04-02 — Phase 38-03 complete: 24 integration test files created, two compiler bugs fixed (iron_runtime.h declarations, typecheck non-ident receiver handling)

## Accumulated Context

### Decisions

- [Phase 32]: Uniform Iron_Closure for ALL closures (capturing + non-capturing), env=NULL when no captures
- [Phase 32]: Iron_Closure typedef in runtime/iron_runtime.h
- [Phase 32]: Env struct fields use original Iron variable names
- [Phase 32]: Mutable var captures use typed pointer fields (int64_t *count)
- [Phase 32]: Capture analysis in src/analyzer/capture.c, runs between typecheck and escape analysis
- [Phase 32]: Full val/var distinction in capture analysis
- [Phase 32]: Self capture deferred to Phase 34
- [Phase 32]: Non-capturing closures call lifted function directly by name
- [Phase 32-03]: Copy-prop must exclude allocas captured by MAKE_CLOSURE (outer function's alloca forwarding bug)
- [Phase 32-03]: capture_04 (array-of-closures) causes compiler infinite loop — deferred to Phase 33
- [Phase 38-03]: Iron_runtime.h needs explicit forward decls for Iron_string_* methods — generated C calls them without implicit declarations (ISO C99)
- [Phase 38-03]: typecheck.c method call handler must use check_expr return value for non-ident receivers; string literal receivers on String type now resolve via decl scan
- [Phase 38-03]: Iron integration tests: booleans print as true/false in interpolation; float 0.0 prints as 0; use val-binding for bool results to avoid nested-quote hang
- [Phase 39]: File-scope __thread RNG (s_math_rng/s_math_rng_init) replaces function-local copies so Math.seed() affects random() and random_int()
- [Phase 39]: Iron_math_sign returns int64_t (-1/0/1) matching Iron Int return type; Iron_math_log wraps log() without collision via Iron_ prefix
- [Phase 33]: capture_12 uses rewritten imperative form instead of if-as-expression to avoid unimplemented codegen path
- [Phase 33]: Iron_TypeAnnotation.is_func: func-type annotations parsed with is_func=true, func_params[], func_return for downstream typecheck/codegen
- [Phase 33]: Parser error recovery: skip-to-] loop in array branch prevents infinite hang on unknown tokens
- [Phase 33]: DCE fix uses inline capture_count check in run_dce only (not touching iron_lir_instr_is_pure) — avoids modifying 8+ call sites
- [Phase 33]: Iron_List_Iron_Closure follows same IRON_LIST_DECL/IMPL macro pair as all other collection types
- [Phase 39]: emit_c.c GET_FIELD for type-name objects must emit Iron_TypeName_FieldName (underscore) not Iron_TypeName.FieldName (dot) — dot notation is invalid C when the object is a type name
- [Phase 39]: Iron_Log_DEBUG/INFO/WARN/ERROR #defines in iron_log.h map enum values to int64_t (Iron Int type) for Log constant field access
- [Phase 39-module-completions-math-io-time-log]: IO.extension returns extension without leading dot — spec note overrides RESEARCH example
- [Phase 39-module-completions-math-io-time-log]: IO.read_lines strips trailing empty element for files ending with newline (POSIX convention)
- [Phase 39]: Timer mutation pitfall: Iron passes struct self by value in hir_to_lir.c — Iron_timer_update/reset take Iron_Timer* but receive a copy; mutation deferred to plan 39-05 integration test
- [Phase 39]: Timer.update/Timer.reset omitted from integration tests: Iron codegen passes Timer struct by value but C functions take Iron_Timer* — causes compile error; TIME-04/05 need compiler fix to emit pointer
- [Phase 39]: Iron string literals do not support backslash escape sequences (\n) — write_file content cannot use \n, must avoid embedded newlines in test strings

### Pending Todos

- Fix compiler infinite loop when compiling array-of-Iron_Closure (capture_04_loop_snapshot)

### Blockers/Concerns

- Array-of-Iron_Closure type resolution causes compiler hang — must be fixed in Phase 33 before loop snapshot examples can work

### Decisions (Phase 38)

- [Phase 38-01]: iron_string_len returns byte count (not codepoint count) — O(1), consistent with STR-ADV-01 deferral
- [Phase 38-01]: iron_string_char_at returns empty string for out-of-range index (no crash)
- [Phase 38-01]: iron_string_count returns 0 for empty sub (avoids strstr infinite-loop pitfall)
- [Phase 38-01]: All 10 method bodies appended to iron_string.c Phase 38 section; no existing code modified
- [Phase 38-02]: Iron_string_replace walks source string (not output buffer) to avoid infinite loop when new_s contains old_s
- [Phase 38-02]: to_int/to_float return 0/0.0 only when end==s (nothing consumed); partial prefix "42abc" returns 42
- [Phase 38-02]: pad_left/pad_right use only first byte of ch parameter as the pad character
- [Phase 38-02]: substring treats start/end_idx as byte offsets, consistent with char_at byte indexing

## Session Continuity

Last session: 2026-04-03T02:25:24.304Z
Stopped at: Completed 39-05-PLAN.md
Resume file: None
