---
phase: 49-loop-fusion-monomorphic-specialization
plan: 01
subsystem: compiler
tags: [loop-fusion, annotations, def-use-analysis, chain-detection, lir, emit_c]

# Dependency graph
requires:
  - phase: 47-collection-methods
    provides: Collection method dispatch infrastructure (Iron_List_*_{map,filter,reduce,forEach,sum})
  - phase: 48-layout-optimizations
    provides: SoA layout, split collection tracking, use_counts computation
provides:
  - "@fusible annotation pipeline from lexer through parser to AST"
  - "Def-use chain detection pre-scan in emit_func_body"
  - "Fusion chain membership and position maps"
  - "--warn-fusion-break CLI flag threaded through to EmitCtx"
  - "FusionChain/FusionChainNode data structures"
  - "docs/fusible-annotation-spec.md specification document"
affects: [49-02 fused-loop-emission, 49-03 monomorphic-specialization]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Annotation parsing: IRON_TOK_AT + identifier lookup before func keyword"
    - "Chain detection pre-scan pattern in emit_func_body (joins existing pre-scan sequence)"
    - "Value-origin propagation through STORE/LOAD for def-use chain linking"

key-files:
  created:
    - docs/fusible-annotation-spec.md
  modified:
    - src/lexer/lexer.h
    - src/lexer/lexer.c
    - src/parser/parser.c
    - src/parser/ast.h
    - src/hir/hir_to_lir.c
    - src/stdlib/list.iron
    - src/lir/emit_c.c
    - src/lir/emit_c.h
    - src/cli/build.h
    - src/cli/main.c
    - src/cli/build.c

key-decisions:
  - "@fusible annotation via IRON_TOK_AT token + identifier check (not a keyword)"
  - "Chain-interior skip disabled until Plan 02 implements fused loop emission"
  - "Fusibility determined by name matching Iron_List_*_{method} not AST flag propagation"
  - "Value-origin map propagates through STORE/LOAD chains for indirect def-use linking"

patterns-established:
  - "FusionChain/FusionChainNode: chain representation with per-node method, self_arg, lambda_args"
  - "fusion_chain_member map: ValueId -> chain index for O(1) membership check"
  - "fusion_chain_position map: ValueId -> position within chain for interior/terminal discrimination"

requirements-completed: [FUSE-01]

# Metrics
duration: 21min
completed: 2026-04-07
---

# Phase 49 Plan 01: Fusible Annotation Pipeline and Chain Detection Summary

**@fusible annotation from lexer to LIR with def-use chain detection pre-scan, --warn-fusion-break flag, and specification document**

## Performance

- **Duration:** 21 min
- **Started:** 2026-04-08T02:25:28Z
- **Completed:** 2026-04-08T02:46:00Z
- **Tasks:** 3
- **Files modified:** 12

## Accomplishments
- Full @fusible annotation pipeline: lexer token, parser consumption, AST flags, stdlib annotations
- Def-use chain detection pre-scan in emit_func_body identifies fusible call sequences with STORE/LOAD propagation and use-count escape analysis
- --warn-fusion-break CLI flag parses, threads through to EmitCtx, and emits diagnostics at chain break points
- docs/fusible-annotation-spec.md: 310-line specification covering syntax, safety requirements, chain rules, body extraction, split collection behavior, SoA awareness, compiler flag, and examples

## Task Commits

Each task was committed atomically:

1. **Task 1: @fusible annotation pipeline from lexer to LIR, plus --warn-fusion-break CLI flag** - `a54768d` (feat)
2. **Task 2: Def-use chain detection pre-scan in emit_func_body** - `1d153fb` (feat)
3. **Task 3: @fusible annotation specification document** - `a75d21e` (docs)

## Files Created/Modified
- `src/lexer/lexer.h` - Added IRON_TOK_AT token kind
- `src/lexer/lexer.c` - Added @ character recognition and kind name
- `src/parser/parser.c` - Added case IRON_TOK_AT in iron_parse_decl for @fusible
- `src/parser/ast.h` - Added is_fusible bool to FuncDecl and MethodDecl
- `src/hir/hir_to_lir.c` - Added comment noting fusibility determined by name matching
- `src/stdlib/list.iron` - Added @fusible annotations to 5 collection methods
- `src/lir/emit_c.c` - FusionChainNode/FusionChain types, EmitCtx fusion fields, chain detection pre-scan, chain-interior skip stub, --warn-fusion-break diagnostics
- `src/lir/emit_c.h` - Added warn_fusion_break parameter to iron_lir_emit_c
- `src/cli/build.h` - Added warn_fusion_break to IronBuildOpts
- `src/cli/main.c` - Parse --warn-fusion-break flag, add to both build opts initializers, add help text
- `src/cli/build.c` - Pass opts.warn_fusion_break to iron_lir_emit_c
- `docs/fusible-annotation-spec.md` - Full specification document

## Decisions Made
- **@fusible via IRON_TOK_AT:** Added `@` as a new lexer token rather than making `@fusible` a keyword. This allows future annotations with the same `@name` pattern.
- **Chain-interior skip disabled:** The skip of chain-interior calls is commented out because the terminal node's fused loop emission (Plan 02) isn't implemented yet. Without it, skipping interior calls breaks the normal emission path. Plan 02 will uncomment this.
- **Name matching over flag propagation:** Fusibility is determined by matching `Iron_List_*_{map,filter,reduce,forEach,sum}` function names in the emitter, not by propagating the AST is_fusible flag through LIR. This is simpler and avoids adding metadata to LIR FUNC_REF instructions.
- **Value-origin propagation:** STORE/LOAD chain propagation enables chain detection even when intermediate values are stored to allocas and reloaded (common in the LIR output pattern).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Chain-interior skip caused undeclared variable errors**
- **Found during:** Task 2 (chain detection pre-scan)
- **Issue:** Enabling chain-interior call skipping prevented intermediate variable declarations, causing C compiler errors for undeclared `_vN` identifiers. The terminal call still referenced these variables through the normal (non-fused) emission path.
- **Fix:** Disabled chain-interior skip and hoisted-value suppression until Plan 02 adds fused loop emission. The chain detection data structures are still built and populated correctly. Added TODO comments marking where Plan 02 will re-enable the skip.
- **Files modified:** src/lir/emit_c.c
- **Verification:** All 257 integration tests pass with 0 failures
- **Committed in:** 1d153fb (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Necessary for correctness. The chain detection analysis is complete and correct; only the emission-time skip is deferred to Plan 02 which adds the replacement fused loop code.

## Issues Encountered
None beyond the deviation noted above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Chain detection data structures (fusion_chains, fusion_chain_member, fusion_chain_position) are ready for Plan 02 to consume
- Plan 02 will implement fused loop emission at terminal nodes and enable chain-interior skip
- --warn-fusion-break infrastructure is functional and ready for diagnostic testing
- docs/fusible-annotation-spec.md provides complete reference for the fusion system

---
*Phase: 49-loop-fusion-monomorphic-specialization*
*Completed: 2026-04-07*
