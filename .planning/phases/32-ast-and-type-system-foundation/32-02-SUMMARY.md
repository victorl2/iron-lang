---
phase: 32-ast-and-type-system-foundation
plan: 02
subsystem: compiler-parser
tags: [c, parser, adt, enum, pattern-matching, printer, match-arms]

# Dependency graph
requires:
  - IRON_TOK_WILDCARD token kind (32-01)
  - Iron_Pattern struct and IRON_NODE_PATTERN (32-01)
  - Iron_EnumConstruct struct and IRON_NODE_ENUM_CONSTRUCT (32-01)
  - Iron_EnumVariant.payload_type_anns / payload_count fields (32-01)
  - Iron_EnumDecl.has_payloads field (32-01)
provides:
  - iron_parse_enum_decl: parses payload types on variants and sets has_payloads
  - iron_parse_pattern: new function for EnumName.Variant(bindings) pattern parsing
  - iron_parse_match_stmt: rewritten with -> arm syntax and error for old { } syntax
  - Expression parsing: IRON_NODE_ENUM_CONSTRUCT emitted for UppercaseName.Variant(args)
  - Printer: -> syntax for match arms and payload types in enum variant output
affects:
  - 32-ast-and-type-system-foundation
  - 33-type-checker-adt-support
  - 34-match-lowering

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Uppercase heuristic: left->kind == IRON_NODE_IDENT && name[0] >= A && <= Z distinguishes EnumConstruct from MethodCall"
    - "Error recovery: old { } arm syntax emits diagnostic but parses block to continue producing a valid AST"
    - "2-token lookahead via p->tokens[p->pos + 1].kind == IRON_TOK_DOT used to select pattern vs expression parsing"

key-files:
  created: []
  modified:
    - src/parser/parser.c
    - src/parser/printer.c

key-decisions:
  - "Uppercase heuristic for enum construction: UppercaseName.Variant(args) -> IRON_NODE_ENUM_CONSTRUCT; lowercase.method(args) -> IRON_NODE_METHOD_CALL; Phase 33 resolver reclassifies if needed"
  - "Old { } match arm syntax is a parse error with recovery: diagnostic emitted, block parsed, case added to AST with error flag in diagnostics"
  - "else { } in match arms also triggers the 'no longer supported' diagnostic with recovery"
  - "Integer-literal match arms still use iron_parse_expr() path; only IDENT followed by DOT triggers iron_parse_pattern()"

patterns-established:
  - "2-token lookahead without save/restore: check p->tokens[p->pos + 1].kind directly"
  - "Error recovery for syntax changes: emit diagnostic, parse block anyway, continue loop"

requirements-completed: [EDATA-02, MATCH-01]

# Metrics
duration: 4min
completed: 2026-04-02
---

# Phase 32 Plan 02: Parser Changes for ADT Syntax Summary

**Parser rewritten for ADT: payload variant parsing, iron_parse_pattern, -> match arm syntax, and IRON_NODE_ENUM_CONSTRUCT detection in expression context**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-04-02T00:00:00Z
- **Completed:** 2026-04-02T00:04:00Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- `iron_parse_enum_decl` extended: parses `Variant(Type, Type, ...)` payloads, zeroes `payload_type_anns`/`payload_count` for plain variants, computes and sets `has_payloads` on `Iron_EnumDecl`
- `iron_parse_pattern` added: parses `EnumName.VariantName(binding, _, ...)` patterns producing `IRON_NODE_PATTERN`; handles `IRON_TOK_WILDCARD` for `_` bindings; simple name bindings stored in `binding_names` with `NULL` in `nested_patterns`
- `iron_parse_match_stmt` rewritten: `->` is the required arm separator; `{ }` arm syntax emits a diagnostic ("no longer supported") but recovers by parsing the block; `else -> body` supported; `else { }` also recovers with diagnostic
- Expression parsing: uppercase-first heuristic — `UppercaseName.Variant(args)` produces `IRON_NODE_ENUM_CONSTRUCT`; lowercase `var.method(args)` produces `IRON_NODE_METHOD_CALL`
- Printer updated: `IRON_NODE_MATCH` emits ` -> ` between pattern and body, `else -> ` for else arm; `IRON_NODE_ENUM_DECL` and `IRON_NODE_ENUM_VARIANT` print `Type, Type` payload list when `payload_count > 0`
- All 17 unit test suites pass (285 tests, 0 failures) — note: integration tests expected to fail due to old `{ }` syntax in test files (Plan 03 migration)

## Task Commits

1. **Task 1: Extend enum decl, add pattern function, rewrite match stmt** - `4ac9090` (feat)
2. **Task 2: Update printer for new syntax and node kinds** - `fd4db7c` (feat)

**Deviation fixes:**
- `4ac9090` includes the Rule 1 fix: else arm recovery made non-fatal (original draft returned error from whole match stmt)

## Files Created/Modified
- `src/parser/parser.c` - extended iron_parse_enum_decl; new iron_parse_pattern; rewritten iron_parse_match_stmt; modified DOT handler in iron_parse_expr_prec
- `src/parser/printer.c` - IRON_NODE_MATCH -> arm syntax; IRON_NODE_MATCH_CASE -> arm syntax; IRON_NODE_ENUM_DECL/VARIANT payload printing

## Decisions Made
- **Uppercase heuristic** for enum construction disambiguation: `UppercaseName.Variant(args)` vs `lowercase.method(args)`. This is a conservative approach that prevents breaking existing method call tests while still detecting ADT construction for conventionally-named types. Phase 33 resolver will reclassify edge cases.
- **Error recovery for old syntax**: Instead of immediately returning an error node from `iron_parse_match_stmt`, the parser emits a diagnostic and continues parsing arms. This lets the test suite (which still uses old `{ }` syntax) produce match nodes (with errors in the diagnostic list) rather than hard errors that break node kind assertions.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] else arm failure returned error from entire match statement**
- **Found during:** Unit test execution (test_parse_match_stmt failed with IRON_NODE_ERROR)
- **Issue:** The draft `else` handling used `return iron_make_error(p)` when `->` was not found after `else`. This caused the entire `iron_parse_match_stmt` to return an error node instead of a valid match node with error recovery.
- **Fix:** Restructured the `else` arm handler: if `{` follows `else`, emit the "no longer supported" diagnostic and parse the block as recovery (matching the regular arm error recovery pattern); if `->` is missing for other reasons, break the loop cleanly without returning an error node.
- **Files modified:** src/parser/parser.c
- **Verification:** All 35 parser unit tests pass including `test_parse_match_stmt`
- **Committed in:** 4ac9090 (same task commit)

---

**Total deviations:** 1 auto-fixed (logic error in error recovery path)
**Impact on plan:** The fix aligns with the plan's error recovery intent. The plan stated "parse the block anyway to continue" for regular arms — the else arm now follows the same pattern.

## Issues Encountered
- The 2-token lookahead `p->tokens[p->pos + 1].kind` works correctly because the parser uses `p->pos` as direct array index and `iron_advance` already skips newlines
- The `IRON_TOK_ARROW` check already had `IRON_ERR_EXPECTED_ARROW` in `iron_expect` error codes, confirming the infrastructure was pre-wired

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Plan 03 (test migration) can now begin: all `pattern { body }` → `pattern -> body` (or `pattern -> { body }`) migrations in 31+ test files
- The parser produces `IRON_NODE_PATTERN` nodes from `EnumName.Variant(bindings)` in match arms
- The parser produces `IRON_NODE_ENUM_CONSTRUCT` from `UppercaseName.Variant(args)` in expression context
- The printer now emits `->` syntax, so round-tripped source will use the new syntax

## Self-Check: PASSED

All task commits confirmed present in git log. Both modified files verified on disk.

- `src/parser/parser.c`: confirmed iron_parse_pattern definition and iron_parse_match_stmt rewrite
- `src/parser/printer.c`: confirmed -> syntax in IRON_NODE_MATCH case

---
*Phase: 32-ast-and-type-system-foundation*
*Completed: 2026-04-02*
