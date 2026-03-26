---
phase: 02-semantics-and-codegen
verified: 2026-03-26T03:38:38Z
status: gaps_found
score: 3/5 must-haves verified
gaps:
  - truth: "A valid Iron program that uses variables, functions, objects, generics, interfaces, and null safety compiles to C that passes clang -std=c11 -Wall -Werror with zero warnings"
    status: failed
    reason: "Generated C contains three categories of bugs that cause clang -Wall -Werror to reject the output: (1) IRON_NODE_FIELD_ACCESS always emits dot notation (self.x) when the receiver is a pointer; inside methods where self is Iron_T*, this must be self->x. (2) IRON_NODE_CALL for object construction calls (e.g., Vec2(1.0, 2.0)) emits Iron_Vec2(1.0, 2.0) — a C function call — rather than the compound literal (Iron_Vec2){.x=1.0, .y=2.0} that IRON_NODE_CONSTRUCT produces. The codegen CALL handler has no SYM_TYPE disambiguation to redirect to compound-literal emission. (3) println codegen emits printf(\"...\", \"\\n\") as two separate arguments; with -Wformat-extra-args this is a hard error."
    artifacts:
      - path: "src/codegen/gen_exprs.c"
        issue: "IRON_NODE_FIELD_ACCESS always emits fa->object + '.' + fa->field (line 433-437). When fa->object is 'self' (a T* pointer) the output is self.x which is a compile error in C."
      - path: "src/codegen/gen_exprs.c"
        issue: "IRON_NODE_CALL handler (line 354-396) does not check whether the callee ident resolves to IRON_SYM_TYPE before emitting. When callee is a type name, codegen emits Iron_TypeName(args) — a C function call that does not exist — instead of the compound literal (Iron_TypeName){.field=arg}."
      - path: "src/codegen/gen_exprs.c"
        issue: "println stub emits printf(str, \"\\n\") treating newline as a second positional argument (line 366-370). clang -Wformat-extra-args -Werror rejects this. Must be printf(\"%s\\n\", str) or concatenate \\n into the format string."
    missing:
      - "IRON_NODE_FIELD_ACCESS: check whether the object expression's resolved_type is a pointer type (struct method self parameter) or struct value, and emit '->' vs '.' accordingly. For 'self' specifically, always use '->'."
      - "IRON_NODE_CALL: before the regular function-call path, check if callee ident's resolved_sym->sym_kind == IRON_SYM_TYPE. If so, redirect to compound-literal emission (same logic as IRON_NODE_CONSTRUCT: look up the object decl, emit (Iron_TypeName){.field=arg, ...})."
      - "println codegen: emit printf(\"%s\\n\", arg) when arg is a string argument (or concatenate \\n directly for string literal args). Never emit printf(str, \"\\n\") with the newline as a separate argument."

  - truth: "A program using defer with multiple early returns executes all deferred calls in reverse order on every exit path; ASan shows zero leaks"
    status: partial
    reason: "The defer mechanism in the compiler (emit_defers, defer_stacks) is structurally correct and passes unit tests. However the generated C cannot be compiled by clang due to the GEN-01 bugs above, so the defer behavior cannot be verified against a running binary. Additionally, the lambda body is incorrectly capturing the enclosing function's defer stack context — the sample output shows that Iron_main's defer(cleanup()) is being emitted inside the lifted Iron_main_lambda_0 function body rather than exclusively in Iron_main."
    artifacts:
      - path: "src/codegen/gen_exprs.c"
        issue: "emit_lambda calls emit_block on the lambda body using the same Iron_Codegen context (including defer_depth and function_scope_depth). If a defer is registered in the enclosing scope before a lambda is defined, the lambda body may incorrectly drain enclosing defers."
    missing:
      - "Verify generated defer behavior against a compiled and executed binary (blocked by GEN-01 bugs)."
      - "In emit_lambda, snapshot and reset defer_stacks state before entering the lambda body, and restore it after, so the lambda does not drain the enclosing function's defers."

  - truth: "Escape analysis marks non-escaping heap allocations and the generated C inserts the correct free; concurrency checks reject parallel for bodies that mutate outer non-mutex variables"
    status: partial
    reason: "Escape analysis (iron_escape_analyze) and concurrency checking (iron_concurrency_check) are implemented and all tests pass. The escape analysis correctly sets auto_free=true on non-escaping heap nodes and escapes=true with E0207 on leaking nodes. The concurrency checker correctly emits E0208. However the codegen does not emit free() calls for nodes marked auto_free=true — this was noted as a requirement in the 02-04 SUMMARY ('Codegen must emit free() calls at block exit for heap nodes with auto_free=true') but gen_stmts.c emit_block does not check for auto_free nodes to emit free calls."
    artifacts:
      - path: "src/codegen/gen_stmts.c"
        issue: "emit_block and emit_stmt have no code to emit free() for Iron_HeapExpr nodes with auto_free=true. The escape analysis sets the flag but codegen never reads it."
    missing:
      - "In emit_block exit path (or after each statement), scan the block's statements for heap expressions marked auto_free=true and emit the corresponding free(varname) call before the block exits."
human_verification:
  - test: "Defer reverse-order execution across multiple early returns"
    expected: "All deferred calls execute in LIFO order on every code path (early return, fall-through, exception-like paths)"
    why_human: "Blocked by GEN-01 compile failures — cannot run the compiled binary. Once GEN-01 is fixed, test with: defer A; defer B; if condition { return }; should execute B then A on the early return path and B then A on fall-through."
  - test: "Auto-free heap memory with ASan"
    expected: "ASan reports zero leaks for programs that use heap() without explicit free, when escape analysis marks the allocation as non-escaping"
    why_human: "Blocked by missing free() emission in codegen (identified above) and GEN-01 compile failures."
---

# Phase 2: Semantics and Codegen Verification Report

**Phase Goal:** The compiler's analysis passes fully annotate the AST and the code generator emits C11 that compiles and executes correctly
**Verified:** 2026-03-26T03:38:38Z
**Status:** gaps_found
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #   | Truth | Status | Evidence |
|-----|-------|--------|----------|
| 1   | A valid Iron program compiles to C that passes `clang -std=c11 -Wall -Werror` | FAILED | Generated C has 3 bug categories: pointer field access (`self.x` not `self->x`), type-constructor call emission (`Iron_Vec2(...)` not compound literal), and println format string bug (`printf(str, "\n")` not `printf("%s\n", str)`). All verified by running the actual pipeline and attempting clang compilation. |
| 2   | `defer` executes in reverse order on every exit path; ASan clean | PARTIAL | Defer drain logic is structurally present and passes unit tests, but cannot be run due to GEN-01 compile failures. Lambda body incorrectly inherits enclosing defer context. |
| 3   | `val` reassignment, nullable without null check, missing interface method each produce compile error pointing to offending line | VERIFIED | E0203 (val reassignment), E0204 (nullable access), E0205 (missing interface method) all implemented in typecheck.c and verified by test_pipeline.c and test_typecheck.c (207 total Unity tests passing). |
| 4   | Escape analysis marks non-escaping heap allocs; concurrency checks reject parallel-for outer mutations | PARTIAL | Analysis passes (iron_escape_analyze, iron_concurrency_check) set auto_free/escapes flags and emit E0207/E0208 correctly. But codegen does NOT emit free() for auto_free nodes — the semantic annotation exists but codegen ignores it. |
| 5   | C unit tests cover lexer, parser, semantic, and codegen internals; end-to-end .iron integration tests verify compilation and execution | VERIFIED | 14 test executables, 207 Unity tests total, all passing (ctest 100%). 6 .iron integration fixtures exist. Integration tests verify C pattern output but do NOT invoke clang on the generated C (no iron_integrate binary exists). |

**Score:** 3/5 truths verified (truths 3 and 5 fully verified; truths 2 and 4 partially implemented; truth 1 definitively failed)

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/analyzer/types.h` | Iron_TypeKind enum (26 kinds), Iron_Type discriminated union | VERIFIED | 26 type kinds present, compound types heap-allocated, primitive singletons interned |
| `src/analyzer/types.c` | Primitive interning, iron_type_equals, iron_type_to_string | VERIFIED | 328 lines, substantive implementation |
| `src/analyzer/scope.h` | Iron_Scope tree, Iron_Symbol, CRUD API | VERIFIED | Complete scope tree with parent-chain, stb_ds hash maps |
| `src/analyzer/scope.c` | Scope tree implementation | VERIFIED | 69 lines, uses sh_new_strdup |
| `src/analyzer/resolve.h` | iron_resolve() two-pass resolver | VERIFIED | Clean API signature |
| `src/analyzer/resolve.c` | Two-pass name resolver | VERIFIED | 679 lines, handles forward refs, self/super, builtin registration |
| `src/analyzer/typecheck.h` | iron_typecheck() entry point | VERIFIED | Clean API |
| `src/analyzer/typecheck.c` | Full type checking with flow-sensitive narrowing | VERIFIED | 1229 lines, substantive |
| `src/analyzer/escape.h` | iron_escape_analyze() | VERIFIED | API present |
| `src/analyzer/escape.c` | Intra-procedural escape analysis | VERIFIED | 389 lines, sets auto_free/escapes |
| `src/analyzer/concurrency.h` | iron_concurrency_check() | VERIFIED | API present |
| `src/analyzer/concurrency.c` | Parallel-for outer mutation checker | VERIFIED | 252 lines |
| `src/analyzer/analyzer.h` | Iron_AnalyzeResult, iron_analyze() | VERIFIED | Unified pipeline API |
| `src/analyzer/analyzer.c` | Unified pipeline | VERIFIED | 35 lines, chains resolve->typecheck->escape->concurrency |
| `src/codegen/codegen.h` | iron_codegen() + Iron_Codegen context | VERIFIED | Full context struct with 7 section buffers, mono_registry, defer stacks, etc. |
| `src/codegen/codegen.c` | Orchestrator: emission order, topo sort | VERIFIED | 458 lines, section-ordered emission |
| `src/codegen/gen_types.c` | iron_type_to_c(), Optional structs, vtable/mono | VERIFIED | 329 lines |
| `src/codegen/gen_stmts.c` | emit_stmt, emit_block, defer drain | STUB (partial) | 498 lines, defer drain works but missing free() emission for auto_free heap nodes |
| `src/codegen/gen_exprs.c` | emit_expr, emit_lambda | STUB (bugs) | 557 lines; field access and CALL disambiguation are buggy — generate invalid C |
| `tests/test_types.c` | Unit tests for type system | VERIFIED | 23 tests, all pass |
| `tests/test_resolver.c` | Unit tests for name resolver | VERIFIED | 15 tests, all pass |
| `tests/test_typecheck.c` | Unit tests for type checker | VERIFIED | 22 tests, all pass |
| `tests/test_escape.c` | Unit tests for escape analysis | VERIFIED | 11 tests, all pass |
| `tests/test_concurrency.c` | Unit tests for concurrency checker | VERIFIED | 5 tests, all pass |
| `tests/test_codegen.c` | Unit tests for codegen | PARTIAL | 24 tests, all pass; but tests verify C string patterns not actual clang compilation. Bugs that produce invalid C slip through because tests only check substring presence. |
| `tests/test_pipeline.c` | End-to-end pipeline tests | PARTIAL | 12 tests, all pass; same caveat — no actual clang invocation |
| `tests/integration/*.iron` | 6+ Iron integration fixtures | VERIFIED | 7 fixtures (hello, variables, functions, objects, nullable, control_flow, game) |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/codegen/codegen.c` | `src/util/strbuf.h` | `iron_strbuf_appendf` for all emission | WIRED | Every section buffer uses iron_strbuf_appendf |
| `src/codegen/gen_types.c` | `src/analyzer/types.h` | IRON_TYPE_INT -> "int64_t" mapping | WIRED | Full switch over all Iron_TypeKind values |
| `src/codegen/gen_stmts.c` | `src/codegen/codegen.h` | `defer_stacks` for defer drain | WIRED | emit_defers iterates defer_stacks in reverse |
| `src/codegen/gen_exprs.c` | `src/analyzer/scope.h` | `iron_scope_lookup` for CONSTRUCT symbol lookup | WIRED | Used in IRON_NODE_CONSTRUCT to get field names |
| `src/codegen/gen_exprs.c` | resolver's sym_kind | `IRON_SYM_TYPE` check in CALL handler | NOT_WIRED | CALL handler does NOT check sym_kind before emitting — this is the root cause of the construction call bug |
| `src/codegen/gen_stmts.c` | `src/analyzer/escape.h` | `auto_free` flag on heap nodes triggers `free()` emission | NOT_WIRED | emit_block never reads auto_free; escape analysis annotations are unused by codegen |
| `src/analyzer/analyzer.c` | `src/analyzer/resolve.h` | `iron_resolve` as first pass | WIRED | Confirmed by direct call in analyzer.c |
| `tests/test_pipeline.c` | `src/codegen/codegen.h` | `iron_codegen` to generate C | WIRED | run_pipeline helper calls iron_codegen |
| `tests/integration/run_integration.sh` | clang binary | actual C compilation verification | NOT_WIRED | Script has no iron_integrate or iron binary available; falls back to counting fixtures as pass without checking C output. No clang invocation occurs. |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| SEM-01 | 02-01, 02-02 | Name resolution builds scoped symbol table (global → module → function → block) | SATISFIED | Iron_Scope tree with GLOBAL/MODULE/FUNCTION/BLOCK kinds; iron_scope_define/lookup; 15 resolver tests pass |
| SEM-02 | 02-01, 02-02 | All identifiers resolve to declarations; undefined variables produce errors | SATISFIED | E0200 emitted for undefined vars; test_pipeline_undefined_error passes |
| SEM-03 | 02-03 | Type inference for val/var without explicit types | SATISFIED | Int literals → IRON_TYPE_INT, Float → IRON_TYPE_FLOAT, Bool → IRON_TYPE_BOOL; test_typecheck.c passes |
| SEM-04 | 02-03 | Type checker validates assignments, function calls, and return types | SATISFIED | E0202 (type mismatch), E0217 (bad arg count), E0218 (not callable) all implemented |
| SEM-05 | 02-03 | val immutability enforced | SATISFIED | E0203 emitted; test_pipeline_val_error and test_typecheck tests pass |
| SEM-06 | 02-03 | Nullable types require null check before use; compiler narrows type after check | SATISFIED | E0204 emitted for unchecked nullable access; flow-sensitive narrowing via stb_ds map |
| SEM-07 | 02-03 | Interface implementation completeness validated | SATISFIED | E0205 emitted for missing interface methods |
| SEM-08 | 02-03 | Generic type parameters validated and instantiated | SATISFIED | Generic params in Iron_TypeKind; type checker validates at call/construction sites |
| SEM-09 | 02-04 | Escape analysis tracks heap allocs and marks non-escaping for auto-free | SATISFIED (analysis only) | iron_escape_analyze sets auto_free=true correctly; 11 tests pass. But codegen does NOT act on auto_free (see GEN gap). |
| SEM-10 | 02-04 | Concurrency checks enforce parallel-for body cannot mutate outer non-mutex variables | SATISFIED | E0208 emitted; 5 concurrency tests pass |
| SEM-11 | 02-02 | Import resolution locates .iron files by path and builds module graph | SATISFIED (basic) | Import nodes resolved in resolver; test_resolve_import passes |
| SEM-12 | 02-02 | self and super resolve correctly inside methods | SATISFIED | self/super special-cased in resolver; test_resolve_self_inside_method passes |
| GEN-01 | 02-05, 02-07 | C code emitted compiles with gcc/clang -std=c11 -Wall -Werror | FAILED | Actual clang invocation fails on all programs containing methods (self.x bug), object construction (Iron_Vec2(...) bug), or println. Three separate code paths in gen_exprs.c produce invalid C. |
| GEN-02 | 02-05 | Defer statements execute in reverse order at every scope exit | PARTIAL | emit_defers drain logic is structurally correct; unit tests verify ordering within generated C strings. Cannot run binary due to GEN-01 failures. |
| GEN-03 | 02-05 | Object inheritance uses struct embedding; child pointer castable to parent | SATISFIED (structural) | _base embedding emitted; topo sort ensures parent before child; test_inheritance_uses_base_embedding passes |
| GEN-04 | 02-06 | Interface dispatch uses vtable structs with function pointers | SATISFIED | Iron_Drawable_vtable struct + Iron_Drawable_ref fat pointer + static instances emitted; test_interface_generates_vtable_struct passes |
| GEN-05 | 02-06 | Generics monomorphized to concrete C types | SATISFIED (stub) | ensure_monomorphized_type emits stub structs with items/count/capacity; dedup registry works; test_monomorphization_generates_concrete_struct and test_monomorphization_dedup pass |
| GEN-06 | 02-05 | Forward declarations and topological sort prevent C compilation order issues | SATISFIED | typedef struct Iron_X Iron_X forward decls emitted; DFS topo sort for struct bodies; test_emission_order verifies ordering |
| GEN-07 | 02-05 | Generated C uses consistent namespace prefix to prevent symbol collisions | SATISFIED | iron_mangle_name/iron_mangle_method enforce Iron_ prefix; test_iron_prefix_applied verifies |
| GEN-08 | 02-05 | Nullable types generate Optional structs with value + has_value flag | SATISFIED | Iron_Optional_T struct emitted via ensure_optional_type; has_value field present; test_nullable_generates_optional_struct passes |
| GEN-09 | 02-06 | Lambda expressions generate C function pointers with closure data | SATISFIED | Pure lambdas lifted as Iron_func_lambda_N; closures use env struct; tests pass |
| GEN-10 | 02-06 | Spawn/await/channel/mutex generate correct thread pool and synchronization code | SATISFIED (codegen stubs) | Spawn generates Iron_spawn_N + Iron_pool_submit; await generates Iron_handle_wait; test_spawn_generates_lifted_function_and_pool_submit passes |
| GEN-11 | 02-06 | Parallel-for generates range splitting, chunk submission, and barrier | SATISFIED (codegen stubs) | Iron_parallel_chunk_N + Iron_pool_submit + Iron_pool_barrier emitted; test_parallel_for_generates_chunk_and_barrier passes |
| TEST-01 | 02-07 | C unit tests cover all compiler internals | SATISFIED | 207 Unity tests across 14 test executables covering lexer, parser, printer, types, resolver, typecheck, escape, concurrency, codegen, pipeline; all pass |
| TEST-02 | 02-07 | .iron integration tests verify end-to-end compilation and execution | PARTIAL | 7 .iron fixtures exist; test_pipeline.c verifies C string patterns; run_integration.sh exists but cannot invoke clang (no iron_integrate binary). Actual C compilation and execution not verified. |

All 25 requirements claimed by phase plans are accounted for. No orphaned requirements.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `src/codegen/gen_exprs.c` | 433-437 | `IRON_NODE_FIELD_ACCESS` emits `.` unconditionally — produces `self.x` instead of `self->x` for pointer receivers | BLOCKER | Causes `clang -Wall -Werror` to fail on any Iron program with methods |
| `src/codegen/gen_exprs.c` | 354-395 | `IRON_NODE_CALL` has no SYM_TYPE check — object construction emits `Iron_Vec2(...)` function call syntax | BLOCKER | Causes `clang -Wall -Werror` to fail on any program with object construction |
| `src/codegen/gen_exprs.c` | 366-370 | `println` stub emits `printf(str, "\n")` — extra argument causes `-Wformat-extra-args` error | BLOCKER | Causes `clang -Wall -Werror` to fail on any Iron program with `println` |
| `src/codegen/gen_stmts.c` | emit_block | No `auto_free` check — escape analysis annotations never converted to `free()` calls | BLOCKER | SEM-09 semantic annotation exists but GEN requirement (free emission) not satisfied |
| `src/codegen/gen_exprs.c` | ~200 | Lambda body emits with same defer context as enclosing scope — deferred calls may appear inside lifted lambda functions | WARNING | Defer correctness in programs combining defer + lambdas is incorrect |
| `src/codegen/gen_exprs.c` | 287-290 | `IRON_NODE_INTERP_STRING` emits `""` stub — interpolated strings produce empty C output | WARNING | Any Iron program using `"{x}"` syntax generates incorrect C (empty string) |

### Human Verification Required

#### 1. Defer execution correctness (after GEN-01 fix)

**Test:** Write an Iron program with `defer` and multiple early returns; compile and run the generated C
**Expected:** Deferred calls execute in LIFO order on every exit path (early return and fall-through)
**Why human:** Cannot run binary until GEN-01 compile bugs are fixed; the structural test only checks text ordering in the generated C string

#### 2. ASan leak detection for auto-free heap nodes (after free() emission fix)

**Test:** Write an Iron program using `heap Obj(...)` that does not escape scope; compile with `-fsanitize=address`; run the binary
**Expected:** ASan reports zero leaks
**Why human:** Requires fixing the missing free() emission in codegen first, then running under ASan

---

## Gaps Summary

Three categories of gaps prevent the phase goal from being achieved:

**Category 1 — Generated C is invalid (GEN-01 blocker):** The phase's primary deliverable — "emits C11 that compiles and executes correctly" — fails at the most basic level. `clang -std=c11 -Wall -Werror` rejects the output of every non-trivial Iron program. Three specific code paths in `src/codegen/gen_exprs.c` produce invalid C:

1. `IRON_NODE_FIELD_ACCESS` always emits dot notation. Methods receive `self` as a `T*` pointer, so `self.x` is invalid; must be `self->x`.
2. `IRON_NODE_CALL` does not redirect to compound-literal emission when the callee resolves to a type. `Vec2(1.0, 2.0)` emits `Iron_Vec2(1.0, 2.0)` — a nonexistent C function — rather than `(Iron_Vec2){.x=1.0, .y=2.0}`.
3. `println` emits `printf(str, "\n")` with newline as a separate extra argument, which `-Wformat-extra-args -Werror` rejects.

**Category 2 — Escape analysis annotation not consumed by codegen:** `iron_escape_analyze` correctly marks `Iron_HeapExpr` nodes with `auto_free=true` but `emit_block` in `gen_stmts.c` never reads this field to emit `free()` calls. SEM-09 is satisfied at the analysis level but the codegen half of GEN-01 (freeing non-escaping allocations) is missing.

**Category 3 — Integration test cannot verify actual C compilation:** The `run_integration.sh` script falls back to counting fixtures as passed when no `iron_integrate` or `iron` binary is available. No test in the suite invokes clang on the generated C output. The GEN-01 bugs went undetected because all tests verify generated C as a *string pattern* rather than as compilable C code.

**Root cause:** The unit tests in `test_codegen.c` check only that certain substrings appear in the generated C. They do not compile the generated C with clang. If they did, the field-access bug, construction bug, and printf format bug would have failed during TDD.

---

_Verified: 2026-03-26T03:38:38Z_
_Verifier: Claude (gsd-verifier)_
