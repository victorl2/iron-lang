---
phase: 02-semantics-and-codegen
plan: 05
subsystem: codegen
tags: [codegen, c-emission, type-mapping, defer, inheritance, optional]
dependency_graph:
  requires: [02-04]
  provides: [iron_codegen, emit_stmt, emit_expr, emit_defers, iron_type_to_c]
  affects: [codegen pipeline, C output]
tech_stack:
  added: [src/codegen/]
  patterns:
    - Section buffer approach (includes/forward_decls/struct_bodies/prototypes/implementations/main_wrapper)
    - DFS topological sort for struct emission ordering
    - Defer stack with depth tracking for scope-exit and early-return drain
    - iron_mangle_name/iron_mangle_method for Iron_ prefix enforcement
key_files:
  created:
    - src/codegen/codegen.h
    - src/codegen/codegen.c
    - src/codegen/gen_types.c
    - src/codegen/gen_stmts.c
    - src/codegen/gen_exprs.c
    - tests/test_codegen.c
  modified:
    - CMakeLists.txt
decisions:
  - Iron_Codegen has a program field (added during implementation) — needed for has_subtype detection in struct body emission
  - Integer literals cast to (int64_t) explicitly to avoid implicit widening surprises
  - Unused variable warnings in generated C are expected/acceptable (Iron doesn't track usage)
  - Nullable var init from null fails typecheck (E0202) — test uses object field with nullable type instead
metrics:
  duration: 10 min
  completed: 2026-03-26
  tasks: 2
  files: 7
---

# Phase 2 Plan 5: Core C Code Generator Summary

**One-liner:** Full C11 code generator with section-ordered emission, type mapping, defer drain, inheritance embedding, and 16 Unity tests.

## What Was Built

The `src/codegen/` directory with the complete Phase 2 core code generator:

- **`codegen.h`** — `Iron_Codegen` context struct (7 section buffers, defer stacks, optional registry) and all internal function declarations
- **`codegen.c`** — `iron_codegen()` orchestrator implementing the 7-section emission order: includes, forward decls (typedef struct Iron_Foo Iron_Foo), struct bodies (topo-sorted), enum defs, prototypes, implementations, main() wrapper
- **`gen_types.c`** — `iron_type_to_c()` mapping all Iron types to C; `ensure_optional_type()` for Iron_Optional_T struct generation
- **`gen_stmts.c`** — `emit_stmt()` for all statement kinds; `emit_block()` with defer push/pop; `emit_defers()` for reverse-order defer drain; function/method prototype and implementation emission
- **`gen_exprs.c`** — `emit_expr()` for all expression kinds including binary, unary, call, method_call, construct, field_access, heap, is, array_lit, comptime stubs

## Key Design Points

**Emission order (codegen.c):**
1. `#include <stdint.h>` etc + `typedef const char* Iron_String;`
2. `typedef struct Iron_Foo Iron_Foo;` forward declarations
3. Struct bodies in topo-sorted order (DFS with gray/black coloring for cycle detection)
4. `typedef enum { ... } Iron_EnumName;` enum definitions
5. Function prototypes (`void Iron_foo();`)
6. Function implementations
7. `int main()` wrapper calling `Iron_main()`

**Inheritance:** Objects with `extends_name` embed parent as first field `_base`. Root objects that have subtypes get `int32_t iron_type_tag` as first field.

**Defer:** Each block pushes a level onto `defer_stacks`. `IRON_NODE_DEFER` pushes the expr onto the current level. Return statements call `emit_defers(sb, ctx, ctx->function_scope_depth)` to drain all levels back to function entry. Block exit drains the block's own level.

**Name mangling:** All user symbols use `Iron_` prefix. Methods use `Iron_TypeName_method`. The `self` parameter is always `TypeName* self`.

## Tests

16 Unity tests in `tests/test_codegen.c` covering:
- val/var declaration → const/mutable C variables
- Function with params and return type
- main() wrapper calls Iron_main()
- Object → typedef struct with field types
- Emission order (forward decl before struct, prototype before impl)
- Iron_ prefix on all user symbols
- Defer executes before return and at scope exit
- Inheritance → _base embedding, topo sort
- Nullable fields → Iron_Optional_T struct
- Enum → typedef enum
- Standard includes
- while loop, binary arithmetic, if statement

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing field] Iron_Codegen missing program pointer**
- **Found during:** Task 1 execution and Task 2 testing
- **Issue:** `has_subtype()` needed access to the program to scan all declarations for inheritance checks, but `Iron_Codegen` only had `global_scope`. The code had a placeholder `ctx->global_scope ? NULL : NULL` that always passed NULL.
- **Fix:** Added `Iron_Program *program` field to `Iron_Codegen` and set it in `iron_codegen()`.
- **Files modified:** `src/codegen/codegen.h`, `src/codegen/codegen.c`
- **Commit:** ad6bcf7, 63c4a0a

**2. [Rule 1 - Bug] Test source used semicolons between object fields**
- **Found during:** Task 2 TDD RED phase
- **Issue:** Test cases used `object Vec2 { var x: Float; var y: Float }` but the Iron parser uses newlines (not semicolons) as field separators in object bodies. The parser emitted E0101 and codegen returned NULL.
- **Fix:** Updated all test source strings to use newline-separated fields.
- **Files modified:** `tests/test_codegen.c`
- **Commit:** 63c4a0a

**3. [Rule 1 - Bug] Defer test searched wrong position in output**
- **Found during:** Task 2 TDD RED phase
- **Issue:** Tests for defer used `strstr(c, "Iron_foo(")` which finds the PROTOTYPE section first, not the implementation body. Searching for `"Iron_bar()"` from there found the bar prototype, not the deferred call within foo's body.
- **Fix:** Updated tests to search from `"Iron_foo() {"` (implementation signature) instead of `"Iron_foo("` (which matches both prototype and implementation).
- **Files modified:** `tests/test_codegen.c`
- **Commit:** 63c4a0a

**4. [Rule 1 - Bug] Emission order test used ambiguous strstr pattern**
- **Found during:** Task 2 TDD RED phase
- **Issue:** `str_before(c, "Iron_bar()", "Iron_bar(")` — both needles start with the same bytes so `strstr` finds the same location for both.
- **Fix:** Changed to check `str_before(c, "Iron_bar();", "Iron_bar() {")` which distinguishes prototype (semicolon) from implementation (open brace).
- **Files modified:** `tests/test_codegen.c`
- **Commit:** 63c4a0a

## Self-Check: PASSED

All created files verified on disk:
- FOUND: src/codegen/codegen.h
- FOUND: src/codegen/codegen.c
- FOUND: src/codegen/gen_types.c
- FOUND: src/codegen/gen_stmts.c
- FOUND: src/codegen/gen_exprs.c
- FOUND: tests/test_codegen.c

All commits verified in git log:
- FOUND: ad6bcf7 (Task 1 commit)
- FOUND: 63c4a0a (Task 2 commit)
