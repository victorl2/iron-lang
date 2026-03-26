---
phase: 04-comptime-game-dev-and-cross-platform
plan: 01
subsystem: compiler
tags: [extern, ffi, raylib, draw, codegen, parser, lexer, resolver]

# Dependency graph
requires:
  - phase: 03-runtime-stdlib-and-cli
    provides: iron_runtime.h infrastructure, Iron_String type, iron_string_cstr()
provides:
  - extern func declaration parsing (snake_case -> CamelCase C name derivation)
  - draw {} block parsing and codegen (BeginDrawing/EndDrawing pair)
  - extern call emission in codegen with raw C names and string auto-conversion
  - IRON_TOK_EXTERN and IRON_TOK_DRAW in lexer
  - IRON_NODE_DRAW AST node and Iron_DrawBlock struct
  - is_extern/extern_c_name fields on Iron_FuncDecl and Iron_Symbol
affects: [04-02-comptime, 04-03-cross-platform]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - iron_check_name() helper allows 'draw' keyword as a valid function/method name
    - is_extern flag on FuncDecl and Symbol routes codegen around Iron_ mangling
    - snake_to_camel conversion derives C FFI names from Iron identifiers

key-files:
  created: []
  modified:
    - src/lexer/lexer.h
    - src/lexer/lexer.c
    - src/parser/ast.h
    - src/parser/ast.c
    - src/parser/parser.c
    - src/parser/printer.c
    - src/analyzer/scope.h
    - src/analyzer/resolve.c
    - src/codegen/gen_exprs.c
    - src/codegen/gen_stmts.c
    - src/codegen/codegen.c
    - tests/test_codegen.c

key-decisions:
  - "iron_check_name() accepts IRON_TOK_DRAW as valid func/method name to avoid keyword conflict with 'func draw()' method declarations"
  - "Extern func prototype and impl emission skipped in codegen.c — extern funcs declared in external C headers, not Iron_-prefixed"
  - "String literal args to extern funcs emit as raw C strings (not iron_string_from_literal) since C APIs like raylib expect const char*"
  - "iron_snake_to_camel helper derives CamelCase C name from Iron snake_case (init_window -> InitWindow)"

requirements-completed: [GAME-01, GAME-02]

# Metrics
duration: 18min
completed: 2026-03-25
---

# Phase 4 Plan 01: Extern Func and Draw Block Support Summary

**Full compiler pipeline support for `extern func` declarations and `draw {}` blocks: lexer tokens, AST nodes, resolver propagation, and codegen emitting raw CamelCase C calls and BeginDrawing/EndDrawing pairs**

## Performance

- **Duration:** ~18 min
- **Started:** 2026-03-25
- **Completed:** 2026-03-25
- **Tasks:** 2
- **Files modified:** 12

## Accomplishments
- Lexer recognizes `extern` and `draw` as keywords (alphabetically inserted in kw_table)
- Parser parses `extern func init_window(...)` with snake_to_camel C name derivation; no body
- Parser parses `draw { ... }` blocks as Iron_DrawBlock statements
- Resolver: collects extern funcs with is_extern=true and extern_c_name on symbol; skips body resolution
- Codegen: extern calls use raw C name (InitWindow), no Iron_ prefix, string literals as raw C strings
- Codegen: draw {} lowers to BeginDrawing(); { body } EndDrawing();
- Codegen: skips prototype and impl emission for extern funcs
- 2 new codegen tests added (test_extern_func_call, test_draw_block); all 36 codegen tests pass

## Task Commits

1. **Task 1: Add extern func and draw block to lexer, parser, AST, and resolver** - `72361ae` (feat)
2. **Task 2: Add extern call emission and draw block codegen, plus tests** - `345e1d1` (feat)

## Files Created/Modified
- `src/lexer/lexer.h` - Added IRON_TOK_EXTERN, IRON_TOK_DRAW
- `src/lexer/lexer.c` - Added "draw"/"extern" to kw_table and kind names table
- `src/parser/ast.h` - Added IRON_NODE_DRAW, Iron_DrawBlock, is_extern/extern_c_name on Iron_FuncDecl
- `src/parser/ast.c` - Added IRON_NODE_DRAW to name table and walk dispatch
- `src/parser/parser.c` - Added iron_check_name(), iron_snake_to_camel(), iron_parse_extern_func(); draw {} stmt parsing; extern in iron_parse_decl
- `src/parser/printer.c` - Added IRON_NODE_DRAW print case
- `src/analyzer/scope.h` - Added is_extern, extern_c_name to Iron_Symbol
- `src/analyzer/resolve.c` - Propagate is_extern/extern_c_name in collect_decl; skip body for extern; IRON_NODE_DRAW case
- `src/codegen/gen_exprs.c` - Extern call detection and emission before SYM_TYPE check
- `src/codegen/gen_stmts.c` - IRON_NODE_DRAW case emits BeginDrawing/EndDrawing
- `src/codegen/codegen.c` - Skip extern funcs in prototype/impl emission loops
- `tests/test_codegen.c` - Added test_extern_func_call and test_draw_block

## Decisions Made
- `iron_check_name()` helper: when parsing func/method names, accept both IRON_TOK_IDENTIFIER and IRON_TOK_DRAW to allow `func draw()` method declarations in interfaces without breaking keyword parsing
- Extern func prototype/impl emission skipped: extern funcs are C declarations in external headers; emitting `void Iron_init_window()` would be wrong
- String literal args to extern funcs: emitted as raw C strings `"text"` not `iron_string_from_literal(...)` since raylib and other C APIs expect `const char*`

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed IRON_NODE_DRAW missing in printer.c switch**
- **Found during:** Task 1 (build verification)
- **Issue:** printer.c has exhaustive switch on node kind; adding IRON_NODE_DRAW without a case caused -Werror,-Wswitch build failure
- **Fix:** Added IRON_NODE_DRAW case to print_node() in printer.c
- **Files modified:** src/parser/printer.c
- **Verification:** Build succeeded with zero errors
- **Committed in:** 72361ae (Task 1 commit)

**2. [Rule 1 - Bug] Fixed 'draw' keyword conflict with existing interface method names**
- **Found during:** Task 1 (test_parser/test_typecheck failures)
- **Issue:** Adding "draw" as a keyword broke 3 existing tests that used `func draw()` as a method name in interfaces; IRON_TOK_DRAW was not accepted as a valid identifier in method name position
- **Fix:** Added iron_check_name() helper accepting both IRON_TOK_IDENTIFIER and IRON_TOK_DRAW; updated interface parser, func/method parser, and extern_func parser to use it
- **Files modified:** src/parser/parser.c
- **Verification:** test_parser (35 tests), test_typecheck (22 tests), test_codegen (36 tests) all pass
- **Committed in:** 72361ae (Task 1 commit)

**3. [Rule 1 - Bug] Fixed extern func emitting Iron_-prefixed prototype and impl**
- **Found during:** Task 2 (test_extern_func_call failure)
- **Issue:** codegen.c unconditionally called emit_func_prototype and emit_func_impl for all IRON_NODE_FUNC_DECL nodes; extern funcs got `void Iron_init_window(...);\n` in the prototype section
- **Fix:** Added `if (fd->is_extern) continue;` guard in both codegen loops
- **Files modified:** src/codegen/codegen.c
- **Verification:** test_extern_func_call passes; InitWindow() emitted without Iron_ prefix
- **Committed in:** 345e1d1 (Task 2 commit)

---

**Total deviations:** 3 auto-fixed (3 Rule 1 bugs)
**Impact on plan:** All fixes necessary for correct behavior. No scope creep.

## Issues Encountered
- pre-existing test_comptime failure (stack overflow in stb_ds) unrelated to our changes; confirmed by git stash test

## Next Phase Readiness
- extern func and draw {} foundation is in place for raylib integration
- The compiler now correctly parses, resolves, and emits extern C FFI calls
- Phase 4 Plan 2 (comptime evaluator) can proceed independently

---
*Phase: 04-comptime-game-dev-and-cross-platform*
*Completed: 2026-03-25*
