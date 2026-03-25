---
phase: 01-frontend
plan: 03
subsystem: compiler-frontend
tags: [c, ast, parser, pratt, recursive-descent, arena, unity]

requires:
  - phase: 01-02
    provides: Iron_Token/Iron_TokenKind from iron_lex_all, lexer API
  - phase: 01-01
    provides: Iron_Arena, Iron_Span, Iron_DiagList infrastructure

provides:
  - Iron_NodeKind enum with 55 node kinds covering all Iron syntax forms
  - All AST node structs (Iron_FuncDecl, Iron_IfStmt, Iron_BinaryExpr, etc.)
  - Iron_Visitor pattern with iron_ast_walk for generic tree traversal
  - Iron_Parser struct and iron_parse() API producing complete Iron_Program AST
  - Pratt expression parser with correct operator precedence (PREC_NONE through PREC_CALL)
  - Error recovery via ErrorNode + sync to statement/toplevel boundaries
  - 35 Unity tests validating all declaration, statement, expression, and span forms

affects:
  - 01-04 (semantic analysis consumes the AST)
  - 02-codegen (codegen walks AST via Iron_Visitor)

tech-stack:
  added: []
  patterns:
    - Struct-per-node AST with Iron_Node base (span + kind as first two fields)
    - Pratt/top-down operator precedence for expressions
    - Arena allocation of all AST nodes via ARENA_ALLOC macro
    - ErrorNode + synchronization for resilient error recovery
    - Iron_Visitor with visit_node/post_visit callbacks for generic AST passes

key-files:
  created:
    - src/parser/ast.h
    - src/parser/ast.c
    - src/parser/parser.h
    - src/parser/parser.c
    - tests/test_parser.c
  modified:
    - CMakeLists.txt

key-decisions:
  - "heap/rc/comptime/await use PREC_UNARY (not PREC_CALL) as inner expression minimum so call expressions like Enemy(args) are captured inside the wrapper node"
  - "ConstructExpr and CallExpr unified at parse time (both emit IRON_NODE_CALL); semantic analysis disambiguates based on whether callee is a type name"
  - "Interface method signatures stored as Iron_FuncDecl with body=NULL to reuse the same node type"
  - "iron_advance() skips newlines automatically after each token advance; callers never need iron_skip_newlines after iron_advance"

patterns-established:
  - "Pratt loop condition: prec <= min_prec breaks; ensures left-associativity and correct precedence hierarchy"
  - "Every node constructor sets span and kind as first two operations"
  - "iron_make_error() returns a non-NULL ErrorNode — parse functions never return NULL"
  - "stb_ds arrput() used for all dynamic node arrays; freed when arena is freed"

requirements-completed: [PARSE-01, PARSE-04]

duration: 8min
completed: 2026-03-25
---

# Phase 1 Plan 03: Parser and AST Summary

**Recursive descent parser with Pratt expression parsing producing span-annotated ASTs for all Iron syntax forms, validated by 35 Unity tests**

## Performance

- **Duration:** ~8 min
- **Started:** 2026-03-25T21:39:53Z
- **Completed:** 2026-03-25T21:48:00Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments

- Complete AST type system: 55 node kinds, all Iron structs, Iron_Visitor/iron_ast_walk
- Complete recursive descent parser covering all declarations (object, func, method, interface, enum, import), all statements (val, var, assign, if/elif/else, while, for parallel, match, defer, free, leak, spawn, return), all expression forms
- Pratt parser for expressions with correct precedence: PREC_FACTOR > PREC_TERM > PREC_COMPARISON > PREC_EQUALITY > PREC_AND > PREC_OR > PREC_IS
- 35 Unity tests all passing, including operator precedence trees, span invariant (all nodes have line > 0), generic functions/objects

## Task Commits

1. **Task 1: AST node types and recursive descent parser** - `9a2e5ff` (feat)
2. **Task 2: Comprehensive parser tests + bug fix** - `7c7a331` (feat + fix)

## Files Created/Modified

- `src/parser/ast.h` — Iron_NodeKind enum, all 55 AST node structs, Iron_Visitor, iron_ast_walk declaration
- `src/parser/ast.c` — iron_ast_walk dispatch over all node kinds, iron_node_kind_str
- `src/parser/parser.h` — Iron_Parser struct, iron_parser_create/iron_parse API
- `src/parser/parser.c` — Complete recursive descent parser (~650 lines), Pratt expression parser, error recovery
- `tests/test_parser.c` — 35 Unity tests covering all Iron syntax forms
- `CMakeLists.txt` — Added ast.c, parser.c to iron_compiler; added test_parser executable and ctest entry

## Decisions Made

- `heap`/`rc`/`comptime`/`await` parse their inner expression at `PREC_UNARY` (not `PREC_CALL`) so that postfix operators like function calls (`Enemy(args)`) are captured inside the wrapper node rather than applied to the wrapper itself
- `ConstructExpr` and `CallExpr` unified at parse time — both emit `IRON_NODE_CALL`; semantic analysis resolves whether a callee is a type name
- Interface method signatures stored as `Iron_FuncDecl` with `body = NULL` to reuse the existing struct rather than adding a separate signature node type
- `iron_advance()` auto-skips newlines after advancing so callers in the parse loop never need explicit `iron_skip_newlines` after `iron_advance`

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] heap/rc/comptime/await inner expression precedence**
- **Found during:** Task 2 (test_heap_expr failure)
- **Issue:** `heap Enemy(1, 2)` parsed as `CallExpr(HeapExpr(Ident(Enemy)), [1, 2])` instead of `HeapExpr(CallExpr(Ident(Enemy), [1, 2]))`. The inner expression was called with `PREC_CALL` as minimum, causing the Pratt loop to refuse to consume the LPAREN (since `PREC_CALL <= PREC_CALL`).
- **Fix:** Changed inner expression minimum precedence from `PREC_CALL` to `PREC_UNARY` for all four wrapper keywords (heap, rc, comptime, await)
- **Files modified:** `src/parser/parser.c`
- **Verification:** test_heap_expr passes, all 35 tests pass, all 4 test suites pass
- **Committed in:** `7c7a331` (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 — bug in Pratt precedence for wrapper expressions)
**Impact on plan:** Essential fix for correctness; no scope creep.

## Issues Encountered

None beyond the precedence bug documented above.

## Next Phase Readiness

- AST complete and tested — semantic analysis (Plan 04) can start immediately
- Iron_Visitor established — semantic passes use the visitor pattern for tree traversal
- All 35 parser tests passing alongside 25 lexer tests, arena tests, and diagnostic tests (4 test suites, 4/4 passing)

---
*Phase: 01-frontend*
*Completed: 2026-03-25*
