---
phase: 56-monomorphic-method-chain
plan: 02
subsystem: analyzer
tags: [typecheck, push-validation, narrowing-audit, phase-55-cleanup, diagnostics]

# Dependency graph
requires:
  - phase: 56-monomorphic-method-chain
    provides: "Plan 01 emit_structs.c pre-scan that emits Iron_List_Iron_<T> decls for every concrete object element type — without this, Plan 02's positive test would fail at codegen before the typecheck ever runs"
  - phase: 55-push-on-interface-arrays
    provides: "push_interface_*.iron test suite with the 2-element workaround pattern that Plan 02 audits"
  - phase: 55.1-empty-typed-array-literal
    provides: "Annotation-aware empty literal path (var shapes: [Shape] = []) used by Plan 02's push_interface_len_empty.iron rewrite"
provides:
  - "push_type_compatible helper in typecheck.c — validates that .push(arg) arg types are compatible with an array receiver's element type"
  - "type_display_name helper in typecheck.c — returns the decl name for object/interface types where iron_type_to_string returns literal '<object>' / '<interface>' placeholders"
  - "Arg-type validation wired into both IRON_NODE_METHOD_CALL array branches (ident-receiver and chained-receiver), emitting IRON_ERR_TYPE_MISMATCH on wrong-type pushes"
  - "tests/integration/mono_push_same_type.iron — positive test for the narrowing path (narrowed [Circle] + same-type push)"
  - "tests/compile_fail/ directory — new category for compile-fail tests that the integration runner does not scan, invoked directly by the verify step"
  - "tests/compile_fail/mono_push_wrong_type.iron — negative test asserting wrong-type push surfaces a clean frontend diagnostic, not a C codegen blowup"
  - "Phase 55 workaround audit: 1 rewrite (len_empty -> empty annotated literal), 7 annotations, 4 protected (untouched)"
affects:
  - "All future uses of .push() on narrowed mono collections: silent miscompilation is now prevented by the typecheck"
  - "Diagnostic quality for object/interface type mismatches: the type_display_name pattern can be adopted by other sites that currently surface '<object>' literals"

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "push_type_compatible: permissive on NULL/ERROR types, exact kind match for primitives, same-decl-pointer for Object==Object and Interface==Interface, implements_names scan for Object-into-Interface"
    - "type_display_name: decl-name fetch for IRON_TYPE_OBJECT and IRON_TYPE_INTERFACE, fallback to iron_type_to_string for other kinds; used only at error emission sites"
    - "Opt-out test category: tests/compile_fail/ sits outside run_tests.sh's scan root, invoked via direct compiler calls from verify commands; pattern for future negative tests"
    - "Audit pattern: for each candidate test, evaluate whether the workaround is load-bearing by inspecting what the test asserts; rewrite only when single-type would preserve assertions exactly; annotate with 5-6 line header comment when load-bearing"

key-files:
  created:
    - tests/integration/mono_push_same_type.iron
    - tests/integration/mono_push_same_type.expected
    - tests/compile_fail/mono_push_wrong_type.iron
    - tests/compile_fail/mono_push_wrong_type.expected
  modified:
    - src/analyzer/typecheck.c
    - tests/integration/push_interface_len_empty.iron
    - tests/integration/push_interface_get.iron
    - tests/integration/push_interface_set_same_type.iron
    - tests/integration/push_interface_len_pop.iron
    - tests/integration/push_interface_get_after_push.iron
    - tests/integration/push_interface_after_op.iron
    - tests/integration/push_interface_typed_var.iron
    - tests/integration/push_interface_prepopulated.iron

key-decisions:
  - "Emit push_type_compatible on BOTH IRON_NODE_METHOD_CALL array branches (ident-receiver and chained-receiver) — chained receivers like arr.filter(p).push(x) otherwise bypass the ident-branch validation"
  - "Add a local type_display_name helper instead of fixing iron_type_to_string — 41 call sites could benefit from the OBJECT/INTERFACE decl name fallback, but Plan 02 deliberately keeps scope narrow and leaves a global fix for a future dedicated pass"
  - "Put the negative test in a new tests/compile_fail/ directory instead of tests/integration/ — the integration runner scans integration/*.iron and treats build failures as test failures, which conflicts with the 'this should fail to build' semantics of a negative test"
  - "Keep the helper's compatibility rules conservative: permissive on NULL/ERROR types (other diagnostics fire first) but reject every unhandled combination, surfacing as a type-mismatch diagnostic the user can investigate"
  - "Phase 55 audit is OPPORTUNISTIC per 56-CONTEXT.md §1.3: 7 of the 8 candidates are load-bearing and only get a comment update; only push_interface_len_empty.iron rewrites cleanly (to the Phase 55.1 annotated empty literal path)"

patterns-established:
  - "Negative tests live in tests/compile_fail/ with a .iron file + a .expected file containing the expected error substring, and are invoked directly by the plan's verify command rather than via run_tests.sh"
  - "Argument-type validation for builtin array methods goes in the IRON_NODE_METHOD_CALL dispatch, not in resolve_array_builtin_method — the latter only gets the method name and array type, not the arg nodes, and emit_error needs the method call's span"
  - "Audit candidate tests by inspecting the test's assertions, not its scaffolding; a 2-element multi-type literal is load-bearing if the test's expected output depends on multi-type dispatch being exercised"

requirements-completed: [MONO-FIX-01, MONO-FIX-02]

# Metrics
duration: 33 min
completed: 2026-04-09
---

# Phase 56 Plan 02: Narrowing Audit + Phase 55 Cleanup Summary

**Type-check arg validation for .push() on array receivers closes the silent-miscompilation gap Plan 01 accidentally opened, the Phase 55 workaround audit rewrites 1 test and annotates 7 as load-bearing, and a new tests/compile_fail/ directory hosts negative tests invoked directly by verify commands outside the integration runner's scan.**

## Root Cause

Before Phase 56, `resolve_array_builtin_method` (src/analyzer/typecheck.c:894) returned IRON_TYPE_VOID for `.push` without ever inspecting the arg type against `arr_type->array.elem`. Wrong-type pushes like `var shapes = [Circle(1)]; shapes.push(Square(2))` used to be caught indirectly: the codegen path emitted `Iron_List_Iron_Circle_push(...)` for the first push, but `Iron_List_Iron_Circle` was never declared (Plan 01 fixed that), so clang rejected the file with `use of undeclared identifier 'Iron_List_Iron_Circle_push'`. That codegen error acted as an accidental safety net.

Plan 01 removed the safety net by pre-scanning ARRAY_LIT elem_types and emitting `IRON_LIST_DECL/IMPL` for every concrete object element type, which is exactly what the primary Phase 56 regression (`.map` on narrowed mono collections) required. The side effect: wrong-type pushes would now silently type-check, silently codegen (with a Square bit pattern written into a Circle-sized slot, wrong tag dispatch, wrong sizeof), and miscompile. Plan 02 closes the gap by hardening the typecheck directly.

## The Fix

Added `push_type_compatible(elem_type, arg_type)` in `src/analyzer/typecheck.c`, returning true when the arg can legally push onto an array of `elem_type`:
- Permissive on NULL / IRON_TYPE_ERROR (lets other diagnostics fire first).
- Primitive kinds must match exactly (Int == Int, not Int == Int32).
- Object == Object requires identical decl pointers.
- Object arg into Interface elem is allowed iff the object's decl lists the interface in `implements_names`.
- Interface == Interface requires identical decl pointers.
- All other combinations reject.

Wired the check into both IRON_NODE_METHOD_CALL array branches: the ident-receiver branch at line 1427 and the chained-receiver (non-ident) branch at line 1498. Each hook runs after `resolve_array_builtin_method` returns and before `mc->resolved_type` is set, so the diagnostic carries the method-call span. The arg type is fetched via a second `check_expr(ctx, mc->args[0])` call, which is idempotent because all args are already checked at the top of the IRON_NODE_METHOD_CALL case.

Added `type_display_name(t, arena)` next to the helper because `iron_type_to_string` returns literal `<object>` / `<interface>` strings for object and interface types (it does not dereference the decl to get the name). Without this helper the error message would read "cannot push value of type '<object>' onto array of element type '<object>'" — useless for diagnosis. The helper is local to typecheck.c and only used at error emission sites; Plan 02 deliberately does not touch the 41 other `iron_type_to_string` call sites.

## Phase 55 Audit

Per 56-CONTEXT.md §1.3, the audit is OPPORTUNISTIC: rewrite only where the 2-element `[Circle, Square]` workaround is NOT load-bearing. Protected files (4) are explicitly listed in the plan's `read_first` block. Audit candidates (8) each got inspected for whether the multi-type literal drives the test's assertions.

### Audit Tally

| Test                                       | Decision  | Reason                                                             |
| ------------------------------------------ | --------- | ------------------------------------------------------------------ |
| push_interface_collection.iron             | protected | plan read_first — DO NOT MODIFY                                     |
| push_interface_multi_type.iron             | protected | plan read_first — DO NOT MODIFY                                     |
| push_interface_pop_order.iron              | protected | plan read_first — DO NOT MODIFY                                     |
| push_interface_loop_100.iron               | protected | plan read_first — DO NOT MODIFY                                     |
| push_interface_len_empty.iron              | rewritten | workaround was pop-to-empty of arbitrary pair; rewrote to Phase 55.1 empty annotated literal (var shapes: [Shape] = []) + push + pop |
| push_interface_get.iron                    | annotated | asserts .get(0..2) on [Circle, Square, Circle] — single-type bypasses _order tag dispatch |
| push_interface_set_same_type.iron          | annotated | overwrites slot 0 (Circle) AND slot 1 (Square); single-type covers only one sub-array |
| push_interface_len_pop.iron                | annotated | asserts last-pop Circle(3).area() via _order tag-switch            |
| push_interface_get_after_push.iron         | annotated | asserts .get(0..3) maps to push-order interleave across types      |
| push_interface_after_op.iron               | annotated | filter keeps one Circle + one Square; final sum 152 depends on multi-type |
| push_interface_typed_var.iron              | annotated | Mode (b) interface-typed push only makes sense with multiple sub-arrays; output 3/48/9 depends on split grouping |
| push_interface_prepopulated.iron           | annotated | "preserved across BOTH sub-arrays" semantics degenerates with single type |

**Rewritten:** 1
**Annotated (load-bearing, comment-only edit):** 7
**Protected (untouched):** 4
**Total push_interface_* tests still passing:** 12 / 12

### Annotation Pattern

Each annotated file gets a 5-6 line comment block in the header starting with `-- Phase 56 Plan 02 audit: ...` that explains which assertion depends on the multi-type literal. No `.iron` semantic changes, no `.expected` file modifications.

### Rewrite Pattern (only applied to len_empty)

`push_interface_len_empty.iron` originally started with `var shapes = [Circle(5), Square(7)]` + pop both + assert len 0. The workaround existed for two historical reasons:
1. Phase 55.1 did not support annotation-aware empty literals yet.
2. A single-element `[Circle(X)]` would narrow to mono and hit the Phase 56 codegen gap.

Both are now obsolete. The rewrite starts with `var shapes: [Shape] = []` (Phase 55.1 empty annotated literal), pushes Circle(5) + Square(7) to reach len 2, then pops both to return to len 0. Semantics preserved: the `.expected` file is unchanged (`2\n0`) and the test still exercises .len() on an empty split collection plus a push/pop round trip.

## Positive + Negative Tests

**Positive: `tests/integration/mono_push_same_type.iron`.** Narrows `var shapes = [Circle(1)]` to `[Circle]` (concrete, mono), pushes `Circle(2)` and `Circle(3)`, sums the areas via a for-in loop. Expected: `42` (3 + 12 + 27). Confirms:
- Plan 01's emit_structs.c pre-scan declares `Iron_List_Iron_Circle` and its push/pop/len/get/set before `main` references them.
- Plan 02's `push_type_compatible` returns true for `Circle == Circle` via object-decl pointer equality (does not over-reject same-type pushes).
- The narrowing path works end-to-end from literal → narrowing → mono collapse → decl emission → IRON_LIST_IMPL push → for-in iteration.

**Negative: `tests/compile_fail/mono_push_wrong_type.iron`.** Same setup but `shapes.push(Square(2))`. Direct compiler invocation asserts:
1. Exit code is non-zero (compiler rejected the program).
2. Stderr contains both `Circle` and `Square` (diagnostic names both types).
3. Stderr does NOT contain `Iron_List_Iron_` (proves the error comes from the frontend, not codegen).
4. No `unreachable` in the program output (the binary was never built and never ran).

Actual diagnostic:
```
error[E0202]: cannot push value of type 'Square' onto array of element type 'Circle': the collection narrows to a single concrete type
  --> tests/compile_fail/mono_push_wrong_type.iron:84:3
   83 |   var shapes = [Circle(1)]
   84 |   shapes.push(Square(2))
      |   ^
   85 |   println("unreachable")
      = help: to push mixed types, annotate the variable with an interface array type, e.g. `var xs: [Shape] = ...`
```

The negative test file lives in `tests/compile_fail/`, a new directory outside `tests/run_tests.sh`'s scan root. The integration runner only scans `tests/integration/*.iron`, so `compile_fail/` files cannot accidentally fail the suite.

## Performance

- **Duration:** 33 min
- **Started:** 2026-04-09T22:18:15Z
- **Completed:** 2026-04-09T22:51:56Z
- **Tasks:** 2
- **Files modified:** 13 (1 source + 2 positive test files + 2 negative test files + 1 rewritten test + 7 annotated tests)
- **Commits:** 2 (feat + test)

## Accomplishments

- Narrowing audit: `push_type_compatible` + `type_display_name` helpers added to typecheck.c (~80 lines incl. comments)
- Validation wired into both IRON_NODE_METHOD_CALL array branches (ident + chained)
- Positive test `mono_push_same_type.iron` passes, returns 42 as expected
- Negative test `mono_push_wrong_type.iron` rejected with clean E0202 mentioning both `Circle` and `Square`
- `tests/compile_fail/` directory created as a new opt-out category for negative tests
- Phase 55 audit: 1 clean rewrite + 7 load-bearing annotations + 4 protected files
- Integration suite: 314 passed / 0 failed (Plan 01 baseline 313 + mono_push_same_type)
- Zero regressions in any Phase 55 push_interface_* test
- Build: `cmake --build build --target ironc` clean, no warnings

## Task Commits

1. **Task 1: push arg-type validation + positive/negative tests** — `e7aa7d9` (feat)
2. **Task 2: Phase 55 workaround audit + rewrite/annotate** — `40c10e7` (test)

## Files Created / Modified

**Created:**
- `src/analyzer/typecheck.c` — added `type_display_name` helper (~15 lines) and `push_type_compatible` helper (~55 lines) above `resolve_array_builtin_method`, plus 2 validation hooks inside IRON_NODE_METHOD_CALL (~30 lines each)
- `tests/integration/mono_push_same_type.iron` + `.expected` — positive test (same-type push on narrowed mono collection)
- `tests/compile_fail/mono_push_wrong_type.iron` + `.expected` — negative test (wrong-type push)
- `tests/compile_fail/` — new directory

**Modified:**
- `tests/integration/push_interface_len_empty.iron` — rewrote to Phase 55.1 empty annotated literal path
- `tests/integration/push_interface_get.iron` — annotated load-bearing
- `tests/integration/push_interface_set_same_type.iron` — annotated load-bearing
- `tests/integration/push_interface_len_pop.iron` — annotated load-bearing
- `tests/integration/push_interface_get_after_push.iron` — annotated load-bearing
- `tests/integration/push_interface_after_op.iron` — annotated load-bearing
- `tests/integration/push_interface_typed_var.iron` — annotated load-bearing
- `tests/integration/push_interface_prepopulated.iron` — annotated load-bearing

## Decisions Made

- **Two branches, not one.** Chained receivers like `arr.filter(p).push(x)` otherwise slip past the ident-branch validation. Both IRON_NODE_METHOD_CALL array branches get the same check.
- **Local `type_display_name` helper instead of fixing `iron_type_to_string` globally.** 41 call sites could benefit from the OBJECT/INTERFACE decl-name fallback, but the risk surface is wider than Plan 02's scope. The local helper is used only at the two new error sites; a future pass can promote it if the pattern proves useful elsewhere.
- **Permissive on NULL / ERROR types.** Other diagnostics fire first for malformed trees; Plan 02 should not double-diagnose.
- **Reject unhandled combinations.** Conservative fallback; if a new type kind appears in array.elem, the helper rejects and the user sees a diagnostic they can investigate, rather than silently accepting and miscompiling.
- **Separate `tests/compile_fail/` directory instead of in-band run_tests.sh handling.** The integration runner treats build failures as test failures; a negative test in `tests/integration/` would fail the whole suite. `compile_fail/` is a clean opt-out that can grow into a real negative-test category later.
- **Audit is opportunistic, not target-driven.** The plan's "0-8 rewrites is a valid outcome" is deliberate. 7 of 8 candidates are load-bearing on close inspection; only len_empty admits a clean rewrite. Rewriting any of the others would either silently bypass the exact dispatch path the test is verifying or require rewriting the `.expected` — both violate the audit rules.
- **Use `check_expr` idempotently for arg type fetch.** All args are already checked at line 1407 at the top of IRON_NODE_METHOD_CALL; calling `check_expr(ctx, mc->args[0])` again walks the arg subtree and returns the already-set `resolved_type` field. Simpler than case-dispatching on every node kind like the plan's Step 3 sketch.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing critical functionality] `iron_type_to_string` returns `<object>` for object types**

- **Found during:** Task 1 (first run of the negative test after wiring up validation)
- **Issue:** `iron_type_to_string` at `src/analyzer/types.c:290-295` returns literal strings `<object>` and `<interface>` for `IRON_TYPE_OBJECT` and `IRON_TYPE_INTERFACE` — it never reads the decl name. The first version of Plan 02's diagnostic used `iron_type_to_string` per the plan sketch and produced "cannot push value of type '<object>' onto array of element type '<object>'" — which fails the acceptance criteria requiring both `Circle` and `Square` to appear in the error text, and is useless for diagnosis.
- **Fix:** Added `type_display_name(t, arena)` helper above `push_type_compatible` in typecheck.c. Returns `t->object.decl->name` for OBJECT, `t->interface.decl->name` for INTERFACE, and falls back to `iron_type_to_string` for other kinds. Replaced both error-site call sites to use the new helper.
- **Files modified:** `src/analyzer/typecheck.c`
- **Verification:** Negative test now emits "cannot push value of type 'Square' onto array of element type 'Circle'" — acceptance criteria satisfied (both type names present). Also checked that the positive test still passes (same-type compat still returns true).
- **Committed in:** Same commit as Task 1 (`e7aa7d9`), the helper and its use were added before the first clean test run.
- **Scope note:** Did NOT fix `iron_type_to_string` itself even though 41 other call sites would benefit. Plan 02's scope is narrowing audit + Phase 55 cleanup; a global diagnostic fix belongs in a future dedicated pass.

**Total deviations:** 1 auto-fixed (missing critical functionality for diagnostic quality).

**Impact on plan:** The plan's sketch in Step 3 explicitly listed `iron_type_to_string` as the name-fetch function. The deviation is a local fix that keeps the plan's intent (diagnostic mentions both types) while working around an unrelated limitation in `iron_type_to_string`. No scope creep.

## Issues Encountered

- **Stale binaries in working tree.** The Iron compiler produces binaries at the project root (e.g., `./mono_push_same_type`) when tests run. They show up as `??` in git status. None are part of this plan — they are accumulated from Plan 01 and earlier. Not fixed in Plan 02; they should be added to `.gitignore` in a cleanup pass.

## Next Plan Readiness

- Phase 56 Plan 02 complete. Phase 56 is complete (Plan 01 + Plan 02 satisfy MONO-FIX-01 and MONO-FIX-02 jointly).
- ROADMAP Phase 56 success criteria:
  - **SC1-SC5 (Plan 01):** already satisfied via mono_method_chain.iron, mono_map_only.iron, mono_chain_filter_reduce.iron, mono_fusion_chain.iron, mono_len_pop_get_set.iron, mono_different_concrete_types.iron (10 tests total)
  - **Narrowing audit (Plan 02 §1.2):** satisfied via push_type_compatible + mono_push_wrong_type.iron negative test
  - **Phase 55 cleanup (Plan 02 §1.3):** satisfied via 1 rewrite + 7 annotations + 4 protected
  - **Negative test (Plan 02 §1.4):** satisfied via tests/compile_fail/mono_push_wrong_type.iron with direct-compiler-invocation verify
- Phase 57 (SoA + fusion composition) is the next phase candidate per 56-CONTEXT.md out-of-scope list.

## User Setup Required

None — no external service configuration required.

## Self-Check: PASSED

All 13 key files exist on disk:
- tests/integration/mono_push_same_type.iron + .expected
- tests/compile_fail/mono_push_wrong_type.iron + .expected
- tests/integration/push_interface_len_empty.iron (rewritten)
- tests/integration/push_interface_{get,set_same_type,len_pop,get_after_push,after_op,typed_var,prepopulated}.iron (annotated)
- src/analyzer/typecheck.c (modified)

Both task commits reachable via git log:
- `e7aa7d9` (Task 1: push arg-type validation + positive/negative tests)
- `40c10e7` (Task 2: Phase 55 workaround audit)

Symbol check in src/analyzer/typecheck.c:
- `push_type_compatible` helper exists
- `type_display_name` helper exists
- `IRON_ERR_TYPE_MISMATCH` wiring in place

---
*Phase: 56-monomorphic-method-chain*
*Completed: 2026-04-09*
