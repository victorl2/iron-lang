# Deferred Items - Phase 53

## Emitter Inline Scoping Bug

**Found during:** 53-01 Task 1 (integration test design)
**Severity:** Medium (workaround available)

**Issue:** When the function inliner expands a small pure function at a call site, the inlined blocks (containing variable declarations) appear after the blocks that USE those variables in the linearized C output. Since C's goto-based control flow doesn't carry scope forward, the C compiler reports "use of undeclared identifier" errors.

**Reproduction:** Any test with a polymorphic array literal (split collection) where constructor arguments include calls to small pure functions (< 30 instructions, no calls, non-recursive). Example: `[Dot(get_x(), get_y()), ...]` where `get_x()` returns a constant.

**Workaround:** Make functions non-inlineable by adding an internal call (has_call=true blocks inlining), or pre-construct objects in local vals before the array literal.

**Suggested fix:** In the C emitter's block linearization, ensure inlined blocks appear before the blocks that reference variables they define. Alternatively, hoist variable declarations to function scope.

**Affected files:** src/lir/emit_c.c (block emission ordering), src/lir/lir_optimize.c (inline_call_site)
