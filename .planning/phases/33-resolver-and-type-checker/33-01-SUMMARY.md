---
phase: 33-resolver-and-type-checker
plan: 01
subsystem: analyzer
tags: [resolver, adt, pattern-matching, error-codes]
dependency_graph:
  requires: [32-01]
  provides: [pattern-binding-resolution, enum-construct-resolution, match-case-scoping, adt-error-codes]
  affects: [33-02, hir_lower]
tech_stack:
  added: []
  patterns: [push_scope/pop_scope per arm, iron_scope_lookup parent-chain shadow check]
key_files:
  created: []
  modified:
    - src/diagnostics/diagnostics.h
    - src/analyzer/resolve.c
    - tests/unit/test_resolver.c
decisions:
  - "IRON_NODE_PATTERN and IRON_NODE_ENUM_CONSTRUCT are resolved in resolve_node (not resolve_expr) since resolve_expr delegates to resolve_node"
  - "Shadow check uses iron_scope_lookup on ctx->current_scope->parent so bindings in the same arm scope do not trigger a false shadow"
  - "iron_arena_strdup used for diagnostic message strings (matches existing pattern in resolve.c)"
metrics:
  duration: 18min
  completed: "2026-04-02"
  tasks: 2
  files_changed: 3
---

# Phase 33 Plan 01: ADT Resolver Extension Summary

**One-liner:** ADT resolver support — pattern binding introduction with shadow checks, enum construct validation, and per-arm block scopes backed by 6 new unit tests and 5 new error codes.

## What Was Built

Extended the Iron name resolver (Pass 2) to handle the three new AST node kinds introduced in Phase 32, and added the corresponding error codes used by both resolver and future type checker phases.

### diagnostics.h — ADT error codes 224-228

Added five new semantic error codes after `IRON_ERR_CIRCULAR_TYPE 223`:

- `IRON_ERR_NONEXHAUSTIVE_MATCH 224` — reserved for exhaustiveness checker (Phase 33-02)
- `IRON_ERR_PATTERN_ARITY 225` — reserved for arity check (Phase 33-02)
- `IRON_ERR_UNREACHABLE_ARM 226` — reserved for reachability check (Phase 33-02)
- `IRON_ERR_BINDING_SHADOWS 227` — emitted by resolver when pattern binding shadows outer variable
- `IRON_ERR_UNKNOWN_VARIANT 228` — emitted by resolver for unknown enum or variant name

### resolve.c — Four resolver changes

1. **IRON_NODE_MATCH_CASE**: replaced bare `resolve_expr(mc->pattern)` with `push_scope / resolve_node(pattern) / resolve_node(body) / pop_scope`. Each arm gets its own isolated block scope.

2. **IRON_NODE_PATTERN** (new case): validates enum name (if qualified) and variant name against the enum's decl_node, then introduces each non-wildcard binding via `define_sym` in the current (arm) scope. Shadow check walks `ctx->current_scope->parent` so the arm scope itself doesn't self-block.

3. **IRON_NODE_ENUM_CONSTRUCT** (new case): validates enum and variant names via scope lookup, then resolves arg expressions.

4. `iron_arena_strdup` used for all diagnostic message strings (consistent with existing resolver pattern).

### tests/unit/test_resolver.c — 6 new tests

| Test | What it verifies |
|------|-----------------|
| `test_resolve_adt_pattern_binding` | `Shape.Circle(r)` — binding `r` introduced, no error |
| `test_resolve_adt_wildcard_no_binding` | `Shape.Rect(_, h)` — wildcard skipped, no error |
| `test_resolve_adt_binding_shadows_error` | `r` shadows outer `val r` — produces error 227 |
| `test_resolve_adt_unknown_variant_error` | `Shape.Triangle` — produces error 228 |
| `test_resolve_adt_enum_construct_ok` | `Shape.Circle(1.0)` construction resolves cleanly |
| `test_resolve_adt_same_binding_across_arms` | `r` in two separate arms — no conflict (isolated scopes) |

Added `has_error_code(int code)` helper function alongside the tests.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] resolve_expr delegates to resolve_node**

- **Found during:** Task 1 — reading resolve.c before implementing
- **Issue:** The plan described adding `IRON_NODE_ENUM_CONSTRUCT` to `resolve_expr`'s switch, but `resolve_expr` is a thin wrapper that immediately calls `resolve_node`. There is no separate switch in `resolve_expr`.
- **Fix:** Added `IRON_NODE_ENUM_CONSTRUCT` to `resolve_node`'s switch (the only real switch in the file), consistent with all other expression cases.
- **Files modified:** src/analyzer/resolve.c
- **Impact:** None — behavior is identical; this is the correct implementation approach.

## Verification Results

- Compiler build: PASSED (`cmake --build build --target iron_compiler`)
- Resolver tests: PASSED (21/21 — 15 existing + 6 new, 0 failures)
- Full unit suite: PASSED (17/17 test targets, 0 failures)
- Integration tests: 175/178 passed — 3 pre-existing failures in `test_io`, `test_enum`, `test_match` (unrelated to resolver; involve runtime NULL handling and match lowering not yet implemented)

## Self-Check: PASSED

All expected files exist. Task commits 793e5ed and 9c1142b confirmed in git log.
