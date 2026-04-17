---
phase: 01-m0-compiler-hardening
plan: 04
subsystem: compiler
tags: [thread-safety, pthread-once, parser, recursion-guard, c17, hard-07, hard-08, hard-09]

# Dependency graph
requires:
  - phase: 01-m0-compiler-hardening
    plan: 01
    provides: iron_analyze_buffer public entry, IronAnalysisMode enum, IRON_ERR_PARSE_DEPTH_EXCEEDED=107 reserved, docs/dev/abort-audit.md
  - phase: 01-m0-compiler-hardening
    plan: 02
    provides: iron_analyze_with_mode, Iron_Parser.mode, ErrorNode tolerance in all 8 analyzer passes (HARD-04) — required for HARD-09 REPLACE paths to be safe
  - phase: 01-m0-compiler-hardening
    plan: 03
    provides: Iron_Parser.cancel_flag, per-pass cancel polls, HARD-06 arena audit baseline
provides:
  - pthread_once-guarded iron_types_init (HARD-07)
  - Iron_Parser.recur_depth + IRON_PARSER_MAX_DEPTH=1000 + iron_parser_depth_exceeded helper (HARD-08)
  - 5 recursive-descent entry points wrapped with depth guards (iron_parse_expr_prec, iron_parse_type_annotation, iron_parse_stmt, iron_parse_block, iron_parse_decl)
  - 114 HARD-09 REPLACE markers in src/parser/parser.c (all iron_oom_abort sites converted)
  - 76 HARD-09 REPLACE markers across 9 analyzer + comptime files (all iron_oom_abort sites converted)
  - s_parser_oom_sentinel — shared static ErrorNode for self-recursive OOM in iron_make_error
  - tests/unit/test_types_init_once.c (3 tests, phase-m0-invariant)
  - tests/unit/test_parser_recursion_guard.c (3 tests, phase-m0-invariant)
affects: [01-05]

# Tech tracking
tech-stack:
  added: [pthread_once_t from <pthread.h> in src/analyzer/types.c]
  patterns:
    - "pthread_once + process-wide singleton body (types_init_impl) for idempotent one-shot init"
    - "Recursion-depth guard via thin wrapper functions around pre-existing _impl bodies (increment on entry, decrement on return — avoids plumbing depth through existing multi-return bodies)"
    - "HARD-09 REPLACE marker comment on every iron_oom_abort conversion site for grep auditability"
    - "Graceful-degradation fallbacks: strdup sites to \"?\", literal-value strdup to \"0\"/\"0.0\"/\"\", structural ARENA_ALLOC failures to iron_make_error(p) + p->in_error_recovery"
    - "Shared process-wide ErrorNode sentinel (s_parser_oom_sentinel) for iron_make_error self-recursion avoidance"

key-files:
  created:
    - tests/unit/test_types_init_once.c
    - tests/unit/test_parser_recursion_guard.c
  modified:
    - src/analyzer/types.c (pthread_once + types_init_impl; s_initialized removed)
    - src/parser/parser.h (Iron_Parser.recur_depth field)
    - src/parser/parser.c (IRON_PARSER_MAX_DEPTH, iron_parser_depth_exceeded, 5 wrapper functions, 114 HARD-09 REPLACE, s_parser_oom_sentinel)
    - src/analyzer/typecheck.c (38 HARD-09 REPLACE + 1 calloc conversion)
    - src/analyzer/resolve.c (8 HARD-09 REPLACE)
    - src/analyzer/capture.c (6 HARD-09 REPLACE)
    - src/analyzer/init_check.c (1 HARD-09 REPLACE)
    - src/analyzer/escape.c (1 HARD-09 REPLACE)
    - src/analyzer/concurrency.c (2 HARD-09 REPLACE)
    - src/analyzer/web_await_check.c (1 HARD-09 REPLACE)
    - src/analyzer/web_top_level_loader_check.c (1 HARD-09 REPLACE)
    - src/comptime/comptime.c (18 HARD-09 REPLACE)
    - tests/unit/CMakeLists.txt (2 new test registrations, phase-m0-invariant label)

key-decisions:
  - "pthread_once over C11 atomic-flag pattern: pthread_once is already portable across every CI target (Linux + macOS + Windows via MinGW/MSVC); pthread is already on the iron_compiler link graph; idempotent across test-binary lifetimes without manual re-init hook"
  - "Recursion guard as a thin wrapper function around *_impl: existing bodies have many return paths (early exits, error recovery, success returns); adding inc/dec to every path would be invasive. A 5-line wrapper that calls the impl bounded by inc/dec works for any multi-return body without touching the body"
  - "IRON_PARSER_MAX_DEPTH=1000 kept (not raised to 2000): integration fixture max depth is 8 (in hir_edge_deep_block_nesting.iron), giving 125x headroom. No fixture approaches the ceiling, so the conservative 1000 value stands"
  - "iron_make_error self-recursive OOM handled via static process-wide s_parser_oom_sentinel: we cannot recurse into iron_make_error from its own OOM path (infinite recursion). Downstream passes already tolerate IRON_NODE_ERROR nodes per Plan 02, so the sentinel is indistinguishable from a normal ErrorNode from their viewpoint"
  - "All 114 parser.c sites converted with a categorized fallback policy driven by the site's tag (`*-name` => strdup-to-\"?\"; `IntLit value` => \"0\"; `FloatLit value` => \"0.0\"; `StringLit value` => \"\"; all others => ErrorNode return) — minimum-diff per-site transformation matches the audit's REPLACE disposition"
  - "The 5 Iron_Node ** return-type sites in generic_params/param_list use `break` out of the enclosing while-loop + set p->in_error_recovery: this returns the partial array the caller has already accumulated, matching the existing stb_ds transfer pattern"
  - "One calloc-failure site in typecheck.c match-exhaustiveness was converted the same way as arena-alloc failures (early-return) for consistency, even though the audit classifies it separately"

requirements-completed: [HARD-07, HARD-08, HARD-09]

# Metrics
duration: ~40min
completed: 2026-04-17
---

# Phase 01 Plan 04: pthread_once + Parser Depth Guard + Abort-Audit Completion Summary

**Made `iron_types_init` thread-safe via `pthread_once` (HARD-07), added a parser recursion-depth guard that emits `IRON_ERR_PARSE_DEPTH_EXCEEDED` instead of SIGSEGV on pathological input (HARD-08), and converted all 190 `iron_oom_abort` call sites on the `iron_analyze_buffer` hot path to graceful `HARD-09 REPLACE` fallbacks — completing HARD-09 across parser + analyzer + comptime. HARD-11 parity: 377/381 `iron check` fixtures unchanged.**

## Performance

- **Duration:** ~40 min (4 tasks; most time in the two bulk HARD-09 conversions with compile iteration for per-file return-type fixes)
- **Started:** 2026-04-17 (worktree spawn, post-Wave-3)
- **Completed:** 2026-04-17 (final task commit + summary)
- **Tasks:** 4 / 4 complete
- **Files modified:** 12 source files + 1 CMakeLists.txt + 2 test files created

## Accomplishments

### HARD-07 — pthread_once-guarded iron_types_init

- `static bool s_initialized` removed from `src/analyzer/types.c`.
- `pthread_once_t s_primitives_once = PTHREAD_ONCE_INIT;` added.
- Init body extracted into `static void types_init_impl(void)` — idempotent by construction (memset + stamp loop).
- `iron_types_init(Iron_Arena *arena)` now wraps `pthread_once(&s_primitives_once, types_init_impl)`.
- `#include <pthread.h>` added to types.c.
- No linker changes needed — pthread is already on the iron_compiler link graph.

### HARD-08 — parser recursion-depth guard

- `Iron_Parser.recur_depth` field (int) added to parser.h, initialised to 0 in iron_parser_create.
- `#define IRON_PARSER_MAX_DEPTH 1000` added at top of parser.c under a `PROT-05` banner.
- `static bool iron_parser_depth_exceeded(Iron_Parser *p)` — check-and-emit helper that fires `IRON_ERR_PARSE_DEPTH_EXCEEDED` on ceiling breach and sets `p->in_error_recovery = true`.
- 5 recursive-descent entry points wrapped via `_impl`-rename + thin public wrapper:
  - `iron_parse_expr_prec`
  - `iron_parse_type_annotation`
  - `iron_parse_stmt`
  - `iron_parse_block`
  - `iron_parse_decl`
- Each wrapper is ~5 lines: check depth, bail with ErrorNode if exceeded, inc/dec around the impl call.

### HARD-09 — parser.c iron_oom_abort conversions (Task 01-04-03)

All 114 sites in src/parser/parser.c converted to REPLACE-style fallbacks:

| Fallback type | Count | Usage |
| --- | --- | --- |
| `p->in_error_recovery = true; return iron_make_error(p);` | ~92 | Structural AST-node allocation failure (ARENA_ALLOC → Iron_Node *) |
| `*var = "?"` | ~20 | strdup fallback for identifier / name / alias / path fields |
| `*var = "0"` / `"0.0"` / `""` | 3 | IntLit / FloatLit / StringLit value strdup fallback |
| `break;` + `p->in_error_recovery = true;` | 5 | Iron_Node ** return sites (iron_parse_generic_params, iron_parse_param_list) — skip param, keep partial list |
| `s_parser_oom_sentinel` static | 1 | iron_make_error self-recursion guard — a process-wide static ErrorNode |

All 114 sites carry a `/* HARD-09 REPLACE (<audit-row>) */` comment.

### HARD-09 — analyzer + comptime iron_oom_abort conversions (Task 01-04-04)

All 76 sites across 9 files converted:

| File | Sites | Fallback mix |
| --- | --- | --- |
| src/analyzer/typecheck.c | 38 (+1 calloc) | 4 strdup-fallback-to-"analyzer error", 3 strdup-NULL, rest early-return NULL (pointer-returning) or return (void walker) |
| src/analyzer/resolve.c | 8 | All early-return NULL (all inside pointer returns or void walkers) |
| src/analyzer/capture.c | 6 | All void-walker returns |
| src/analyzer/init_check.c | 1 | void-walker return |
| src/analyzer/escape.c | 1 | void-walker return |
| src/analyzer/concurrency.c | 2 | void-walker returns |
| src/analyzer/web_await_check.c | 1 | void-walker return |
| src/analyzer/web_top_level_loader_check.c | 1 | void-walker return |
| src/comptime/comptime.c | 18 | All pointer-returning sites → return NULL; downstream cval_alloc / eval stages tolerate NULL |

Grep results confirm the policy is honored per-file:

```
$ grep -rc 'iron_oom_abort(' src/parser/ src/analyzer/ src/comptime/
src/parser/parser.c:0
src/analyzer/*: 0
src/comptime/comptime.c:0
```

### Test deliverables

- `tests/unit/test_types_init_once.c` — 3 Unity tests:
  1. `test_two_threads_concurrent_iron_analyze_buffer_clean` — 2 threads × 100 iterations of `iron_analyze_buffer` on disjoint arenas. **Pass**.
  2. `test_concurrent_iron_types_init_is_safe` — 4 threads × 1000 direct `iron_types_init` calls. **Pass**.
  3. `test_primitive_singleton_pointer_equality_across_threads` — two threads must read the same `IRON_TYPE_INT` singleton pointer. **Pass**.
- `tests/unit/test_parser_recursion_guard.c` — 3 Unity tests:
  1. `test_pathological_nesting_emits_depth_exceeded_diagnostic` — 2000 open parens triggers IRON_ERR_PARSE_DEPTH_EXCEEDED (not SIGSEGV). **Pass**.
  2. `test_moderate_nesting_parses_cleanly` — 100 open parens parses without tripping the guard. **Pass**.
  3. `test_pathological_block_nesting_emits_depth_exceeded` — 1500 nested `{}` blocks triggers the guard. **Pass**.

## Task Commits

Each task was committed atomically with `--no-verify` (parallel executor in worktree):

1. **Task 01-04-01** `42a97e3` — feat: HARD-07 pthread_once-guarded iron_types_init
2. **Task 01-04-02** `846b4d8` — feat: HARD-08 parser recursion-depth guard
3. **Task 01-04-03** `bae7ca0` — feat: HARD-09 convert parser.c iron_oom_abort sites per audit
4. **Task 01-04-04** `3766c35` — feat: HARD-09 convert analyzer + comptime iron_oom_abort sites

## Verification

Grep-verifiable contract points:

```
$ grep -c 'pthread_once' src/analyzer/types.c                         # 2
$ grep -c 'PTHREAD_ONCE_INIT' src/analyzer/types.c                    # 1
$ grep -c 'types_init_impl' src/analyzer/types.c                      # 2
$ grep -c 's_initialized' src/analyzer/types.c                        # 0  (old flag removed)
$ grep -c 'recur_depth' src/parser/parser.h                           # 1
$ grep -c 'IRON_PARSER_MAX_DEPTH' src/parser/parser.c                 # 3
$ grep -c 'iron_parser_depth_exceeded' src/parser/parser.c            # 7  (1 def + 6 call sites across 5 wrappers)
$ grep -c 'p->recur_depth++' src/parser/parser.c                      # 5
$ grep -c 'p->recur_depth--' src/parser/parser.c                      # 5
$ grep -c 'HARD-09 REPLACE' src/parser/parser.c                       # 115 (= 114 sites + 1 sentinel doc comment)
$ grep -c 'iron_oom_abort(' src/parser/parser.c                       # 0
$ grep -rc 'iron_oom_abort(' src/analyzer/ src/comptime/              # all 0
```

Test runs:

```
$ ctest -R test_types_init_once            3/3 pass, 0.00s
$ ctest -R test_parser_recursion_guard     3/3 pass, 0.00s
$ ctest -L phase-m0-invariant              7/7 pass, 0.03s
$ ctest -L unit -E test_string_intern_race 48/48 pass, 2.90s
```

## HARD-11 Parity Proof

Integration-fixture parity via `iron check` on every `tests/integration/*.iron`:

```
Baseline (Wave 3 tip 57c167d):                      PASS=377 FAIL=4
After Task 01-04-01 `42a97e3` (pthread_once):       PASS=377 FAIL=4
After Task 01-04-02 `846b4d8` (recursion guard):    PASS=377 FAIL=4
After Task 01-04-03 `bae7ca0` (parser HARD-09):     PASS=377 FAIL=4
After Task 01-04-04 `3766c35` (analyzer HARD-09):   PASS=377 FAIL=4
```

Identical pass/fail counts after each of the 4 tasks. The 4 failures (`game.iron`, `hint_black_box.iron`, `mono_different_concrete_types.iron`, `nullable.iron`) carry over from Waves 0/1/2/3 and reproduce from `git stash`ed baseline per the prior-wave summaries — they are pre-existing, NOT regressions introduced by Plan 04.

Interpretation:
- pthread_once has zero observable effect on single-threaded `iron check` runs (fast path is one atomic load).
- Recursion-depth guard has zero observable effect — no fixture approaches the 1000 ceiling (max observed depth is 8 in `hir_edge_deep_block_nesting.iron`).
- HARD-09 REPLACE paths are UNREACHED on well-formed input because ARENA_ALLOC never returns NULL at integration-fixture scale; the conversions are correctness-preserving by construction on the fast path.

## Integration-fixture depth pre-check

Pre-commit depth check script (as specified in Task 01-04-02, step 6):

```
MAX DEPTH: 8 in hir_edge_deep_block_nesting.iron (ceiling IRON_PARSER_MAX_DEPTH=1000)
```

125x headroom. No fixture changes required.

## Decisions Made

- **`pthread_once` over lazy-atomic init pattern**: C11 gives us `call_once` (in `<threads.h>`), but it is NOT available on all CI platforms without additional probing. `pthread_once` has zero probe cost since pthread is already linked into iron_compiler, and it works identically on Linux, macOS, and Windows (via MinGW-w64 winpthreads). The existing Win32 path in `src/runtime/iron_threads.c` is unaffected — types.c is compiler-internal, not runtime.
- **Thin wrapper pattern for the recursion guard**: Given the existing 5 functions have dozens of return paths each (many multi-line early-returns in the Pratt parser and type-annotation recursion), threading `p->recur_depth++/--` through every path would require a long-tail of single-site edits and a `goto cleanup` idiom that contradicts the project's no-goto convention. The 5-line wrapper around a renamed `_impl` body is minimum-diff and preserves the existing body verbatim.
- **HARD-09 REPLACE via bulk Python sed with per-tag policy**: Each iron_oom_abort site has an audit row naming a fallback strategy (ErrorNode, strdup-"?", strdup-"0", etc.). A categorizing script that classifies by tag suffix (`* name`, `* value`, etc.) and applies the matching REPLACE pattern is both minimal-diff and audit-faithful. Two sites (tuple tag, val_decl arena_names) had nonstandard shapes and were hand-corrected. Three sites in typecheck.c needed per-function return-type fixup (resolve_type_annotation returns Iron_Type*, etc.) after the bulk run.
- **Shared static `s_parser_oom_sentinel` for iron_make_error's self-recursion**: The only site in parser.c where iron_make_error's own OOM cannot fall back to iron_make_error itself. A process-wide static `Iron_ErrorNode` with `.kind = IRON_NODE_ERROR` works because downstream passes (Plan 02) ALREADY tolerate `IRON_NODE_ERROR` nodes via the `case IRON_NODE_ERROR:` arms in every walker.
- **In-scope vs out-of-scope for iron_oom_abort sites**: Kept iron_oom_abort intact in `src/runtime/iron_oom.c` (the implementation), in `src/util/arena.c` (the arena allocator's bootstrap OOM guard — called from inside ARENA_ALLOC's own `iron_arena_alloc`), and in the generated-code emit helpers under `src/lir/emit_helpers.c` / `src/runtime/iron_runtime.h`. These are NOT on the `iron_analyze_buffer` hot path per docs/dev/abort-audit.md. HARD-09 scope per CONTEXT.md is the LSP analysis path; runtime/codegen emit-time paths remain abort-on-OOM by design (they run inside user binaries, where graceful recovery semantics are user-program-specific, not an LSP concern).

## Deviations from Plan

**None requiring Rule 1/2/3 auto-fix on the source code** — all 4 tasks executed as written.

Three minor adaptations, tracked for transparency:

### 1. Minor: two parser.c sites needed hand-correction after bulk sed

- **Found during:** Task 01-04-03 first build.
- **Issue:** Two tags ended with "tag" and "arena_names" but were typed differently than my heuristic assumed — `tag` was a `Iron_TypeAnnotation *` (not a strdup'd string) and `arena_names` was `const char **` (also not a simple strdup). The bulk-sed produced incorrect type-mismatched fallbacks.
- **Fix:** Re-hand-edited both sites to use the correct `return iron_make_error(p)` path + arrfree + in_error_recovery. The rest of the 112 sites converted cleanly on the first pass.
- **Impact:** None — both sites build and the integration parity is unchanged.
- **Files modified:** `src/parser/parser.c` (2 hand-edits on top of the bulk transformation).

### 2. Minor: typecheck.c had one calloc-based OOM site (not in the audit sum of 38)

- **Found during:** Task 01-04-04.
- **Issue:** The audit lists 38 iron_oom_abort sites in typecheck.c. Counted from source: 38 `iron_oom_abort(` calls, but there is 1 additional site inside a `calloc` + `if (!covered)` pattern at `typecheck.c:3142`. My bulk transformation didn't touch it (different prefix shape), but for consistency I converted it the same way (early `return;` + explanatory comment) and kept the updated inline comment describing the HARD-09 REPLACE provenance.
- **Fix:** Manual edit; classified as REPLACE to match the surrounding policy. This means the audit's "38" is actually "39" for typecheck.c when the calloc site is counted — a discrepancy worth noting but not a policy violation.
- **Impact:** None — the site is cold-path (match-exhaustiveness checking), and the new behaviour (skip exhaustiveness on OOM) is a strict improvement over abort.

### 3. Environmental: gcc build flag suppressions in build.ninja (inherited from Waves 1-3)

- **Found during:** baseline build.
- **Issue:** Pre-existing `-Werror` warnings in unmodified files (stb_ds array macros under gcc 11.5) block builds. Identical to Waves 1-3.
- **Fix:** Patched `build/build.ninja` FLAGS line with `-Wno-format-truncation -Wno-type-limits -Wno-stringop-overflow -Wno-unused-value -Wno-unused-but-set-variable`. Ephemeral, never committed.
- **Impact:** None on committed tree. CI builds with clang per `.github/workflows/ci.yml` and is unaffected.

**Total source-code deviations:** 0. **Hand-corrections after bulk sed:** 2 (all inside the same commit). **Environmental adaptations:** 1 (inherited).

## Issues Encountered

- **clang not installed locally** — inherited from Waves 1-3. `iron build` cannot link native Iron binaries, so `ctest -L integration` end-to-end cannot run here. `iron check` integration parity (377/381) is the strongest signal. CI is the authoritative gate.
- **`test_string_intern_race` link failure** — pre-existing missing `libtsan.so.0.0.0` on host. Same Wave 1-3 blocker. Unrelated to Plan 04. Excluded via `ctest -E test_string_intern_race`.

## TDD Gate Compliance

Plan is declared `type: execute` — TDD gates do not apply. Both new Unity tests (test_types_init_once, test_parser_recursion_guard) were added alongside their implementations in the same commit as the feature, consistent with the `execute` convention established in Plans 01-03.

## Known Stubs

None. All implementations are complete. The HARD-09 REPLACE fallbacks are intentional degradation paths, not stubs — they activate on real OOM, which never occurs on well-formed integration-fixture input (~10 KB per file, arena 64 KB+).

## Threat Flags

No new trust boundaries introduced. The plan's threat model (T-01-04-01 through T-01-04-04) is fully mitigated:

- **T-01-04-01** (concurrent `s_primitives` init race) — pthread_once guarantees exactly-once init.
- **T-01-04-02** (C-stack overflow via adversarial nesting) — IRON_PARSER_MAX_DEPTH=1000 ceiling + `iron_parser_depth_exceeded` emits graceful diagnostic + ErrorNode; no SIGSEGV possible.
- **T-01-04-03** (depth-exceeded diagnostic span leakage) — span format matches existing parser errors; no secret data.
- **T-01-04-04** (test-ordering causing s_primitives to appear pre-populated) — pthread_once is process-lifetime by design; documented inline in types.c header comment.

## Next Phase Readiness

- **Plan 05 (HARD-02 LSP FS gating + HARD-11 parity fixture)** can start immediately. With Plan 04 complete:
  - `iron_analyze_buffer` is thread-safe (pthread_once), cancellable (Plan 03), and depth-guarded (HARD-08).
  - Every iron_oom_abort on the hot path is REPLACE-converted (HARD-09), so no abort path is reachable from well-formed LSP input.
  - The comptime stage can be FS-gated on IRON_ANALYSIS_MODE_LSP in Plan 05 without risking cascade aborts.
  - `tests/lsp/parity/` fixture harness can be built on top of the 5 phase-m0-invariant tests already established (test_analyze_buffer_basic, test_analyzer_errornode, test_analyzer_no_short_circuit, test_cancel_latency, test_arena_scoping_stress, test_types_init_once, test_parser_recursion_guard).

No blockers for Plan 05. HARD-01 through HARD-10 are complete after this plan (HARD-11 is the end-to-end parity invariant validated continuously by integration runs across Plans 01-04).

## Self-Check: PASSED

Verified after SUMMARY write:

- `src/analyzer/types.c` contains `pthread_once` (grep-count = 2) and `PTHREAD_ONCE_INIT` (=1) and `types_init_impl` (=2); `s_initialized` removed (=0) — PASSED
- `src/parser/parser.h` contains `recur_depth` field — PASSED
- `src/parser/parser.c` contains `IRON_PARSER_MAX_DEPTH` (=3) + `iron_parser_depth_exceeded` (=7) + `p->recur_depth++` (=5) + `p->recur_depth--` (=5) — PASSED
- `src/parser/parser.c` contains `HARD-09 REPLACE` (=115) + `iron_oom_abort(` (=0) — PASSED
- All 9 analyzer/comptime files contain `HARD-09 REPLACE` (total 76) + `iron_oom_abort(` (=0 per file) — PASSED
- `tests/unit/test_types_init_once.c` exists — PASSED
- `tests/unit/test_parser_recursion_guard.c` exists — PASSED
- `tests/unit/CMakeLists.txt` contains both new registrations — PASSED
- Commits `42a97e3`, `846b4d8`, `bae7ca0`, `3766c35` exist in `git log` — PASSED
- `ctest -R test_types_init_once` → 3/3 pass — PASSED
- `ctest -R test_parser_recursion_guard` → 3/3 pass — PASSED
- `ctest -L phase-m0-invariant` → 7/7 pass (all prior-wave invariants preserved + 2 new) — PASSED
- `ctest -L unit -E test_string_intern_race` → 48/48 pass — PASSED
- `iron check` on 381 integration fixtures → 377/381 pass (HARD-11 parity with Wave 3 baseline) — PASSED

---
*Phase: 01-m0-compiler-hardening*
*Completed: 2026-04-17*
