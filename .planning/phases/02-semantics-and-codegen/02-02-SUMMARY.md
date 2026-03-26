---
phase: 02-semantics-and-codegen
plan: 02
subsystem: analyzer
tags: [name-resolution, scope-tree, symbol-table, stb_ds, two-pass, tdd]

# Dependency graph
requires:
  - phase: 02-01
    provides: Iron_Type system, Iron_Scope/Iron_Symbol API (iron_scope_create, iron_scope_define, iron_scope_lookup)
  - phase: 01-frontend
    provides: AST node definitions (Iron_Program, Iron_Ident, Iron_FuncDecl, Iron_MethodDecl, etc.), parser pipeline

provides:
  - Two-pass name resolver (iron_resolve) that builds scope tree and links every Iron_Ident to its Iron_Symbol
  - Semantic annotation fields on all AST expression nodes (resolved_sym, resolved_type, auto_free, escapes, declared_type, resolved_return_type, owner_sym)
  - Error codes E0200 (undefined var), E0201 (duplicate decl), E0210 (self outside method), E0211 (super no parent)

affects:
  - 02-03 (type checker consumes resolved_sym, declared_type, resolved_return_type)
  - 02-04 (escape analysis reads/writes auto_free and escapes on Iron_HeapExpr)
  - 02-05 and beyond (all semantic passes depend on resolver having run first)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Two-pass collect-then-resolve pattern (Pass 1a: register top-level decls; Pass 1b: attach methods; Pass 2: recursive resolve)
    - ResolveCtx struct for thread-local resolver state (arena, diags, current_scope, current_method, current_type_name)
    - push_scope/pop_scope helpers that set current_scope; no stack data structure needed (parent pointer chain)
    - Direct switch dispatch on node kind (mirrors printer.c pattern) rather than iron_ast_walk visitor

key-files:
  created:
    - src/analyzer/resolve.h
    - src/analyzer/resolve.c
    - tests/test_resolver.c
  modified:
    - src/parser/ast.h (added semantic annotation fields to all expression structs)
    - src/parser/parser.c (initialize annotation fields to NULL/false; add IRON_TOK_SELF/SUPER to primary parser)

key-decisions:
  - "Two-pass resolver: Pass 1 collects all top-level decls before Pass 2 resolves any ident — handles forward references where method appears before its owning object"
  - "self and super represented as ordinary Iron_Ident nodes with name == \"self\" / \"super\"; resolver special-cases them rather than adding new AST node kinds"
  - "IRON_TOK_SELF and IRON_TOK_SUPER added to iron_parse_primary as Iron_Ident nodes — required to avoid infinite parse loop when self/super appear in expressions"
  - "Arena alloc does not zero memory; all parser-allocated AST nodes must explicitly initialize semantic annotation fields to NULL/false or arena garbage causes segfaults in resolver"

patterns-established:
  - "Parser rule: every newly allocated AST struct must initialize ALL fields, including semantic annotation fields, not just parse-time fields"
  - "Resolver rule: check owner_sym == NULL before dereferencing; attach_method returns early on lookup failure without setting owner_sym"

requirements-completed: [SEM-01, SEM-02, SEM-11, SEM-12]

# Metrics
duration: 68min
completed: 2026-03-25
---

# Phase 2 Plan 02: Two-Pass Name Resolution Summary

**Two-pass name resolver linking all Iron_Ident nodes to Iron_Symbol declarations, with forward-reference support, self/super handling, block scoping, and 15 Unity tests passing**

## Performance

- **Duration:** ~68 min (22:16 - 23:24 UTC-3)
- **Started:** 2026-03-25T22:16:08-03:00
- **Completed:** 2026-03-25T23:24:35-03:00
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments

- Extended all AST expression structs with semantic annotation fields (`resolved_sym`, `resolved_type`, `auto_free`, `escapes`, `declared_type`, `resolved_return_type`, `owner_sym`) in ast.h
- Implemented 657-line two-pass name resolver (`iron_resolve`) handling functions, methods, objects, interfaces, enums, imports, val/var decls, for loops, blocks, and self/super
- Added `IRON_TOK_SELF` and `IRON_TOK_SUPER` to `iron_parse_primary` (required for parser correctness — missing cases caused infinite loop)
- 15 Unity tests covering all resolution scenarios; all 9 test suites (91 total tests) pass

## Task Commits

Each task was committed atomically:

1. **Task 1: Extend ast.h with semantic annotation fields** - `40b6715` (feat)
2. **Task 2 RED: Add failing tests for two-pass name resolver** - `f6b5278` (test)
3. **Task 2 GREEN: Implement two-pass name resolver** - `2bf4926` (feat)

## Files Created/Modified

- `src/analyzer/resolve.h` - Public API: `Iron_Scope *iron_resolve(Iron_Program *, Iron_Arena *, Iron_DiagList *)`
- `src/analyzer/resolve.c` - Full two-pass resolver implementation with ResolveCtx, collect_decl, attach_method, resolve_node
- `tests/test_resolver.c` - 15 Unity tests covering forward refs, self/super, block scoping, error cases
- `src/parser/ast.h` - Semantic annotation fields added; struct tags added to Iron_ObjectDecl, Iron_InterfaceDecl, Iron_EnumDecl for forward-declaration compatibility
- `src/parser/parser.c` - SELF/SUPER primary expression handling; NULL initialization of all semantic annotation fields

## Decisions Made

- Two-pass approach required for Iron's forward-reference pattern (method `func Player.update()` may appear before `object Player { }` in source)
- `self` and `super` parsed as `Iron_Ident` nodes (name "self"/"super") rather than dedicated AST node kinds; resolver special-cases by name — simpler with no AST churn
- Arena allocator does not zero memory; all parser allocations must explicitly zero semantic fields — this became a critical correctness invariant documented as a project pattern

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Parser infinite loop on `self`/`super` in expressions**
- **Found during:** Task 2 GREEN (test_resolve_self_inside_method hang at 84+ seconds)
- **Issue:** `IRON_TOK_SELF` and `IRON_TOK_SUPER` had no cases in `iron_parse_primary`. The parser returned an error node without advancing the token, causing the block statement loop to spin forever on the same token.
- **Fix:** Added `case IRON_TOK_SELF:` and `case IRON_TOK_SUPER:` to `iron_parse_primary` in `parser.c`; each creates an `Iron_Ident` node with `iron_advance(p)`.
- **Files modified:** `src/parser/parser.c`
- **Verification:** Test suite completes in <1s; test 8 (`test_resolve_self_inside_method`) passes.
- **Committed in:** `2bf4926` (Task 2 feat commit)

**2. [Rule 1 - Bug] Segfault from uninitialized `owner_sym` in Iron_MethodDecl**
- **Found during:** Task 2 GREEN (`test_resolve_method_unresolved_type` crash)
- **Issue:** `Iron_MethodDecl.owner_sym` was not initialized in `iron_parse_func_or_method`. Arena allocation returns unzeroed memory (`0xbebebebe...` pattern under ASan). `attach_method` checked `if (md->owner_sym)` and then accessed `md->owner_sym->type`, causing SIGSEGV at resolve.c:305.
- **Fix:** Added `m->owner_sym = NULL;` to `Iron_MethodDecl` initialization in `parser.c`.
- **Files modified:** `src/parser/parser.c`
- **Verification:** ASan/UBSan clean; all 15 resolver tests pass.
- **Committed in:** `2bf4926`

**3. [Rule 1 - Bug] Uninitialized semantic fields on FuncDecl, ValDecl, VarDecl, HeapExpr, interface signatures**
- **Found during:** Task 2 GREEN (revealed by ASan uninitialized-read warnings)
- **Issue:** Several parser allocation sites for `Iron_FuncDecl`, `Iron_ValDecl`, `Iron_VarDecl`, `Iron_HeapExpr`, and interface `FuncDecl` signatures omitted NULL-initialization of the newly added semantic fields (`resolved_return_type`, `declared_type`, `resolved_type`, `auto_free`, `escapes`).
- **Fix:** Added explicit NULL/false initialization at each allocation site.
- **Files modified:** `src/parser/parser.c`
- **Verification:** Build clean with -Wextra -Werror; all tests pass.
- **Committed in:** `2bf4926`

**4. [Rule 1 - Bug] Test 13 used newline-separated enum variants instead of comma-separated**
- **Found during:** Task 2 GREEN (test 13 reported 2 parse errors, expected 0)
- **Issue:** Test source used `Red\n  Green\n  Blue` but Iron enum syntax requires `Red,\n  Green,\n  Blue`. Parser stops after first variant when no comma follows.
- **Fix:** Changed test 13 enum source to use comma-separated variants.
- **Files modified:** `tests/test_resolver.c`
- **Verification:** Test 13 (`test_resolve_enum_variants`) passes with 0 errors.
- **Committed in:** `2bf4926`

**5. [Rule 1 - Bug] Test 14 used string-literal import path instead of dot-separated identifier path**
- **Found during:** Task 2 GREEN (pre-empted during test source review)
- **Issue:** Test used `import "std/math"` (string literal syntax) but Iron import parser expects identifier path `import std.math`.
- **Fix:** Changed test 14 import source to `"import std.math\n"`.
- **Files modified:** `tests/test_resolver.c`
- **Verification:** Test 14 (`test_resolve_import`) passes with 0 errors.
- **Committed in:** `2bf4926`

---

**Total deviations:** 5 auto-fixed (5 Rule 1 bugs)
**Impact on plan:** All fixes were necessary for correctness. The parser SELF/SUPER fix and field initialization fixes are correctness invariants that would have caused issues in every subsequent semantic pass. No scope creep.

## Issues Encountered

- Unity writes to stdout via `putchar`; test diagnostics to stderr. When a test hung under a timeout wrapper, stdout buffered output was lost, making it appear tests produced no output. Resolved by identifying the hang first, then running interactively.
- Debugging required creating a temporary `test_debug.c` (no Unity framework) to isolate parse/resolve behavior without Unity's setjmp/longjmp signal handling. Removed after debugging.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Name resolution complete; all identifiers resolved to Iron_Symbol or error emitted
- All semantic annotation fields initialized and ready for type checker to populate `resolved_type`, `declared_type`, `resolved_return_type`
- `iron_resolve()` returns global scope root; type checker can walk scope tree directly
- Blocker: resolve.c does not yet validate type annotations (e.g., `val x: i32 = ...` — the `i32` token is not resolved to a type). Type checker (02-03) must handle this.

## Self-Check: PASSED

All created files verified present. All task commits verified in git history.

---
*Phase: 02-semantics-and-codegen*
*Completed: 2026-03-25*
