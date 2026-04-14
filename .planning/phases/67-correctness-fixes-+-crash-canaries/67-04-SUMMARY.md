---
phase: 67-correctness-fixes-+-crash-canaries
plan: 04
subsystem: correctness
tags: [fix-02, arena, oom, iron_oom_abort, parser, lexer, walkthrough]

# Dependency graph
requires:
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: iron_oom_abort helper (67-02) and diagnostics.h wiring
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: 67-03 integer-safety baseline (357 integration pass count)
provides:
  - Every iron_arena_alloc / ARENA_ALLOC / iron_arena_strdup call-site in parser.c + lexer.c guarded with iron_oom_abort on NULL
  - Location literal at every guard site ("parser.c:<function>" / "lexer.c:<function>") so any OOM abort stderr pinpoints the exact call-site
  - Parser subsystem arena contract upgraded from "silent UB on malloc fail" to "explicit fail-fast with bisectable location"
affects: [67-05 analyzer + comptime walkthrough, 67-06 hir + lir + runtime walkthrough, 67-07 cross-arena walkthrough, 67-08 crash canaries]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "iron_oom_abort idiom A: ARENA_ALLOC → if (!var) iron_oom_abort(\"file.c:function\") → dereference"
    - "Location literal convention: \"<file>:<enclosing_function>\" (with sub-site disambiguator if multiple allocs share a function)"
    - "Dead code elimination on arena pre-fallbacks: replace 'if (!buf) buf_cap = 0' dead-code patterns with iron_oom_abort"

key-files:
  created: []
  modified:
    - src/parser/parser.c (117 insertions / 3 deletions — 114 guard sites across all parser functions)
    - src/lexer/lexer.c (9 insertions / 11 deletions — 9 guard sites + removed 10-line pre-Phase-67 OOM error-token fallback)

key-decisions:
  - "Guard-on-OOM (iron_oom_abort) is the default treatment for the entire parser subsystem; SAFETY annotations were not applied to any site because every parser alloc is on a user-reachable token-consumption path with no compile-time bound"
  - "Location literals include the enclosing function name so OOM-abort stderr output bisects to the exact call-site (114 distinct literals in parser.c alone)"
  - "iron_snake_to_camel: replaced silent name-fallback-on-NULL (return original name) with iron_oom_abort — graceful degradation on allocation failure was masking a correctness bug (C-name conversion silently skipped)"
  - "iron_lex_string pre-fallback buf_cap=0 dead-code (line 252-253) and 4 KB scratch IRON_ERR_UNTERMINATED_STRING OOM fallback (line 265-275) both replaced with iron_oom_abort — the dead 256-byte scratch is still allocated but now aborts rather than sets a swallowed buf_cap; the 4 KB scratch no longer misrepresents OOM as 'unterminated string'"
  - "No new fixtures, no new error codes, no REG-02 canaries — plan is strictly walkthrough-and-guard with zero behaviour changes on the green path"

patterns-established:
  - "Parser/lexer FIX-02 idiom: every arena alloc produces a location literal of form \"<file>:<function>\" (optionally with sub-site disambiguator) so stderr OOM aborts bisect cleanly"
  - "Grep-auditable coverage: the final grep count (iron_oom_abort + SAFETY: cannot fail) must equal or exceed the alloc site count for each file, so review via `grep -c` is deterministic"

requirements-completed: [FIX-02]

# Metrics
duration: 38 min
completed: 2026-04-13
---

# Phase 67 Plan 04: FIX-02 Parser Subsystem Arena Walkthrough Summary

**Every iron_arena_alloc / ARENA_ALLOC / iron_arena_strdup call-site in src/parser/parser.c (114 sites) and src/lexer/lexer.c (9 sites) now routes NULL through iron_oom_abort with a function-qualified location literal — 123 total guards, zero SAFETY annotations, zero behaviour changes, zero regressions.**

## Performance

- **Duration:** 38 min
- **Started:** 2026-04-13T18:31:56Z
- **Completed:** 2026-04-13T19:09:58Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- 114 parser.c arena call-sites walked top-to-bottom and guarded — every AST node kind (Iron_ErrorNode, Iron_TypeAnnotation × 5 contexts, Iron_Ident × 4, Iron_Param, Iron_Block × 3, Iron_LambdaExpr, Iron_IntLit/FloatLit/StringLit/BoolLit × 2/NullLit, Iron_UnaryExpr × 3, Iron_ArrayLit × 2, Iron_HeapExpr, Iron_RcExpr, Iron_ComptimeExpr, Iron_AwaitExpr, Iron_EnumConstruct × 3, Iron_MethodCallExpr × 3, Iron_FieldAccess, Iron_SliceExpr, Iron_IndexExpr, Iron_CallExpr, Iron_IsExpr, Iron_BinaryExpr, Iron_IfStmt, Iron_WhileStmt, Iron_ForStmt, Iron_Pattern, Iron_MatchCase × 2, Iron_MatchStmt, Iron_SpawnStmt, Iron_InterpString, Iron_ValDecl × 2, Iron_VarDecl, Iron_ReturnStmt, Iron_DeferStmt, Iron_FreeStmt, Iron_LeakStmt, Iron_AssignStmt, Iron_ImportDecl, Iron_FuncDecl × 4 contexts, Iron_MethodDecl × 2, Iron_ObjectDecl, Iron_Field, Iron_InterfaceDecl, Iron_EnumVariant, Iron_EnumDecl, Iron_Program) plus all associated iron_arena_strdup sites (names, paths, aliases, type names, binding names, etc.) now have NULL-check + iron_oom_abort
- 9 lexer.c arena call-sites walked — iron_lex_string scratch buffer (2 sites), final string value, hex/binary/decimal number values, identifier text, and 2 diagnostic-message strdups — all routed through iron_oom_abort with "lexer.c:<function>" location literals
- Two pre-existing dead/buggy OOM paths cleaned up:
  - parser.c iron_snake_to_camel: silent graceful-degradation (returned the un-converted name on alloc fail, silently masking a C-name bug) → now iron_oom_abort
  - lexer.c iron_lex_string: 10-line "OOM → return ERROR token with IRON_ERR_UNTERMINATED_STRING" fallback that misrepresented OOM as a lexical error → now iron_oom_abort
- Grep census: parser.c 114 sites ↔ 114 guards ↔ 114 iron_oom_abort calls (all prefixed "parser.c:"); lexer.c 9 sites ↔ 9 guards ↔ 9 iron_oom_abort calls (all prefixed "lexer.c:"); phase-level gate total 123 ≥ 123 required

## Task Commits

Each task was committed atomically:

1. **Task 1: FIX-02 parser.c arena walkthrough** — `6fbc3e8` (fix)
2. **Task 2: FIX-02 lexer.c arena walkthrough** — `9e55b09` (fix)

**Plan metadata:** _pending — will be recorded by docs(67-04): complete FIX-02 parser walkthrough plan_

## Files Created/Modified

- `src/parser/parser.c` — 114 guard sites inserted across every arena alloc call-site; iron_snake_to_camel dead-fallback replaced with abort
- `src/lexer/lexer.c` — 9 guard sites inserted; 2 legacy NULL-handling paths (dead buf_cap=0 + 4 KB OOM→ERROR-token-with-wrong-code) replaced with iron_oom_abort

## Decisions Made

1. **Guard-by-default over SAFETY annotations** — Zero SAFETY annotations were applied. Every parser/lexer allocation is on a user-reachable input-consumption path with no compile-time size bound, so the plan's "when in doubt, guard" rule resolved to "always guard". No stretches of 10+ consecutive allocations shared a single leading SAFETY comment either.

2. **Function-qualified location literals with sub-site disambiguators** — For functions containing multiple allocations (e.g., iron_parse_primary has ~20), each call-site gets a distinct literal: `"parser.c:iron_parse_primary IntLit"`, `"parser.c:iron_parse_primary IntLit value"`, etc. The disambiguator names either the AST node kind or a semantic fragment (e.g., "array elem name", "tuple tag"). An OOM stderr grep will resolve to the exact alloc without ambiguity.

3. **Clean up pre-existing buggy OOM paths inline** — The iron_snake_to_camel silent-fallback and iron_lex_string IRON_ERR_UNTERMINATED_STRING misclassification were both latent correctness bugs that the walkthrough surfaced. Rather than preserve them as "not in scope", they were replaced with iron_oom_abort consistent with the plan's uniform treatment. Both are covered under Deviations below (Rule 1: Bug).

4. **Zero behaviour changes on the green path** — Every guard is strict defensive code on a path that previously dereferenced NULL unconditionally. No test fixtures touched, no error codes added, no parse tree shape changes. The 357-test integration baseline from 67-03 is preserved exactly.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] iron_snake_to_camel silent graceful-degradation on OOM**
- **Found during:** Task 1 (parser.c walkthrough, line 2048)
- **Issue:** `if (!buf) return name;` returned the pre-conversion snake_case name on arena alloc failure, silently skipping the C-name conversion. This would produce wrong emitted C function names for extern funcs under OOM pressure — a correctness bug masked as graceful degradation.
- **Fix:** Replaced the silent fallback with `if (!buf) iron_oom_abort("parser.c:iron_snake_to_camel");` — consistent with the uniform plan treatment.
- **Files modified:** src/parser/parser.c
- **Verification:** grep confirms the site now aborts; ctest + integration suite green (no test exercises the OOM path, but no regression on the happy path either)
- **Committed in:** 6fbc3e8 (Task 1)

**2. [Rule 1 - Bug] iron_lex_string OOM misrepresented as IRON_ERR_UNTERMINATED_STRING**
- **Found during:** Task 2 (lexer.c walkthrough, lines 265-275 pre-edit)
- **Issue:** On 4 KB scratch-buffer allocation failure, the lexer emitted `IRON_ERR_UNTERMINATED_STRING` with message "out of memory" and returned an error token. This misclassified an OOM as a lexical syntax error — the user's source was perfectly valid, but they'd see an E-code pointing at a nonexistent string-termination problem. Additionally, the preceding 256-byte `buf_cap=0` fallback at line 252 was dead code (overwritten immediately by the 4 KB alloc at 265, with `(void)buf_cap;` swallowing the fallback value).
- **Fix:** Both sites now route through iron_oom_abort with distinct location literals (`"lexer.c:iron_lex_string scratch"` and `"lexer.c:iron_lex_string 4K scratch"`). The 10-line `if (!buf) { ... return ERROR; }` block was replaced with a single-line abort. No fixture was added because 67-08 (REG-02 canaries) owns OOM behavior tests.
- **Files modified:** src/lexer/lexer.c
- **Verification:** grep shows the misclassified IRON_ERR_UNTERMINATED_STRING + "out of memory" block is gone; ctest + integration suite green
- **Committed in:** 9e55b09 (Task 2)

---

**Total deviations:** 2 auto-fixed (both Rule 1 - Bug, both pre-existing latent bugs surfaced by the walkthrough)
**Impact on plan:** Both deviations are within scope — they're bug fixes to incorrect OOM handling on paths the plan explicitly targets. The walkthrough's "every site gets uniform treatment" goal naturally exposed the two outlier sites with ad-hoc (and wrong) fallbacks. No new tests, no scope creep, no file touches outside the two plan-designated files.

## Issues Encountered

None. The walkthrough proceeded linearly through each file, with per-batch grep census checks confirming guard counts matched alloc counts before build + test runs.

## Authentication Gates

None — fully autonomous walkthrough against local source tree.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- parser.c + lexer.c FIX-02 coverage is complete and grep-auditable. The `iron_oom_abort("parser.c:` / `iron_oom_abort("lexer.c:` prefix convention is established for 67-05 + 67-06 to extend to analyzer + comptime + hir + lir.
- 67-05 (analyzer + comptime, ~100 sites) and 67-06 (hir + lir + runtime/stdlib, ~105 sites) can now proceed in parallel — they are file-disjoint and depend only on the iron_oom_abort helper already shipped in 67-02.
- No blockers. Integration baseline 357/357 preserved; ctest 74/74 preserved.

---
*Phase: 67-correctness-fixes-+-crash-canaries*
*Completed: 2026-04-13*

## Self-Check: PASSED

- `.planning/phases/67-correctness-fixes-+-crash-canaries/67-04-SUMMARY.md` exists on disk
- `src/parser/parser.c` exists on disk
- `src/lexer/lexer.c` exists on disk
- commit `6fbc3e8` (Task 1 parser.c walkthrough) present in git log
- commit `9e55b09` (Task 2 lexer.c walkthrough) present in git log
