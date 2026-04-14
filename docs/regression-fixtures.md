# Regression Fixture Template

Every regression fixture in `tests/integration/` that protects against a
specific class of bug (blind cast, enum switch incompleteness, layout
assumption, use-after-free, etc.) follows a 4-section doc-comment template.
This document formalizes the template and lists the fixtures that adopt it.

## Canonical Example

The reference fixture is
[`tests/integration/hir_to_lir_elif_mono_walker.iron`](../tests/integration/hir_to_lir_elif_mono_walker.iron).
Read it first before adding a new regression fixture. Its top-of-file
doc-comment establishes the pattern and explains every section with a real
motivating bug: the Phase 37 `collect_mono_enums_node` SIGSEGV that hid in
the Iron compiler for months because the Linux Release CI path reported
compile crashes as "benchmark compilation failed".

## The 4-Section Template

Every regression fixture begins with an Iron-comment doc block at the top
of the file. The block has exactly four labeled sections in this order:

### 1. Motivating Incident

**What happened, where, and when.** A short paragraph identifying:

- The phase / commit / date the bug was introduced.
- The file and function where the bug lived.
- The specific wrong assumption the code made (e.g., "local typedef
  guessed at the layout of `Iron_IfStmt` and was missing the `elif_conds[]`
  and `elif_bodies[]` fields").
- The user-visible symptom (silent wrong output, SIGSEGV on Linux
  x86_64 but not macOS arm64, wrong codegen, etc.).
- How the bug was discovered (CI report, grep, audit row, user bug report).

This section is the "story" — when a contributor sees a red test in the
future, they should be able to read this paragraph and immediately
understand which bug class they are staring at.

### 2. Layout Diagram

**The structural picture of the bug.** An ASCII diagram or short
code block showing:

- The memory layout of the structs or data involved.
- What the buggy code was reading or writing at which offset.
- How the real layout differs from the assumed layout.

For blind-cast regressions, this is typically a side-by-side of the
"assumed struct" vs. the "real struct" with the diverging field
highlighted. For enum-switch regressions, this might be a list of
enum values the switch handles vs. the ones it misses. For layout
assumption regressions, this is a sizeof / offsetof table.

### 3. Fix Summary

**Which file:line was changed and how.** A short paragraph identifying:

- The file(s) and line number(s) of the fix.
- The high-level change (e.g., "added `node->kind` guard before cast",
  "converted in-place reinterpret to fresh allocation", "added missing
  `case IRON_NODE_FOR:` to the walker switch").
- Which `PROT-0x` / `FIX-0x` / `REG-0x` requirement the fix covers.
- Cross-reference to the phase and plan that landed the fix.

This section makes the fix grep-able — a future contributor searching
for "where did we fix the Phase 37 SIGSEGV?" can grep the integration
directory for `Fix Summary` and find this entry.

### 4. Severity

**H / M / L from CORRECTNESS-AUDIT.md.** A short justification of the
blast radius:

- **H (High)** — crash, UB, data corruption, or security risk in the
  default execution path. Every user program hits this bug.
- **M (Medium)** — correctness risk under abnormal conditions (OOM,
  unusual input shapes, specific compiler optimization modes).
- **L (Low)** — theoretical or cosmetic. No known user-visible failure.

The severity rating must match the row's severity in
`.planning/research/CORRECTNESS-AUDIT.md`. If the audit does not list
the bug, cite the phase that discovered it instead.

## Template Skeleton

Copy this skeleton when adding a new regression fixture. Replace the
placeholder text and the Iron source below:

```
-- Regression: <short title of the bug class>.
--
-- **Motivating Incident.** <what phase/commit introduced the bug, what
-- function was wrong, what the wrong assumption was, what the user-visible
-- symptom was, how the bug was discovered>
--
-- **Layout Diagram.** <ASCII diagram or short description showing the
-- wrong vs. correct memory layout, or the missing enum cases, or the
-- sizeof/offsetof mismatch>
--
-- **Fix Summary.** <which file:line was changed, what the high-level
-- change was, which PROT-0x / FIX-0x / REG-0x requirement this covers,
-- which phase and plan landed the fix>
--
-- **Severity.** <H / M / L from CORRECTNESS-AUDIT.md with a one-sentence
-- blast-radius justification>

<Iron source code that exercises the bug>
```

## Fixtures Adopting This Template

The following fixtures follow the 4-section template. This list grows as
each phase adds regression tests derived from audit rows or real-world bug
reports.

### Reference

- `tests/integration/hir_to_lir_elif_mono_walker.iron` — The canonical
  example. Protects against a future regression in
  `collect_mono_enums_node`'s AST walker switch coverage.

### Phase 66 Plan 03 (PROT-04 blind-cast rewrites)

Landed alongside the typecheck.c / resolve.c / escape.c rewrites. Fixture
names correspond to the `target_regression_fixture_name` column in
`.planning/research/CORRECTNESS-AUDIT.md`. If Plan 03 ships with
different fixture names (e.g., because an audit row was split or merged),
update this list in the same commit so the two sources stay in sync.

- `tests/integration/blind_cast_type_sym_decl.iron` — SYM_TYPE->decl_node
  cast to Iron_ObjectDecl without kind check (typecheck.c:1366, :1791).
- `tests/integration/blind_cast_expr_resolved_type.iron` — Iron_IntLit
  aliasing to access resolved_type on a generic expression node
  (typecheck.c:2707, :3070).
- `tests/integration/blind_cast_owner_decl.iron` — owner_sym->decl_node
  cast in super handler without kind check (resolve.c:236, :251).
- `tests/integration/enum_construct_reinterpret.iron` — in-place
  reinterpretation of Iron_EnumConstruct as Iron_MethodCallExpr /
  Iron_FieldAccess (resolve.c:674, :680).
- `tests/integration/blind_cast_expr_common_layout.iron` — runtime
  safety-net for the Iron_ExprNode common prefix assumption
  (hir_lower.c:109; primary defense is PROT-01's `_Static_assert` in
  `src/parser/ast.h`).
- `tests/integration/blind_cast_leak_ident.iron` — Iron_Ident cast in
  leak statement without kind check (escape.c:251).

### Phase 67 Plan 02 (FIX-01 ranks 3 + 4 — generated-C OOM guards)

Landed with the `emit_c.c` HEAP_ALLOC / RC_ALLOC malloc-guard rewrites and
the `emit_web.c` FrameState wrapper guard (Wasm-W1 from 67-01's re-audit).
Each fixture exercises a minimal Iron program whose compiled binary's
generated C contains an `iron_oom_abort` call on the allocation failure
path. Run the fixture, verify the expected stdout, then grep the cached
intermediate C under `.iron-build/` (or build with `IRON_KEEP_C=1`) to
confirm the guard string is present in the emitted code.

- `tests/integration/null_heap_alloc_malloc.iron` — `heap` keyword HEAP_ALLOC
  malloc guard (emit_c.c IRON_LIR_HEAP_ALLOC case, post-Phase-67-02 drift).
- `tests/integration/null_rc_alloc_malloc.iron` — `rc` keyword RC_ALLOC
  malloc guard (emit_c.c IRON_LIR_RC_ALLOC case, post-Phase-67-02 drift).

### Phase 67 Plan 03 (FIX-01 ranks 14 + 15 + 19 — integer safety)

Landed with the comptime arithmetic overflow rewrite, the comptime
unary-negation INT64_MIN guard, and the parser enum-variant `atoi -> strtol`
replacement. Each fixture exercises the non-overflow code path with values
deliberately close to the limit so the rewritten branches run on every
compiler build; the diagnostic paths are verified by source-level grep on
the acceptance criteria in 67-03-PLAN.md (run_integration.sh has no
`.expected-error` mode, so the safe-path + grep split is the canonical
structure for integer-safety canaries this phase).

- `tests/integration/int_comptime_arith_overflow.iron` — comptime
  `+`, `-`, `*` rewritten to `__builtin_add/sub/mul_overflow` with
  `IRON_ERR_COMPTIME_ERROR` emission on overflow (comptime.c:410-412 site;
  FIX-01 rank 14).
- `tests/integration/int_comptime_neg_min.iron` — comptime unary `-`
  guarded against INT64_MIN before the C negation (comptime.c:493 site;
  FIX-01 rank 15).
- `tests/integration/int_enum_value_overflow.iron` — parser enum variant
  explicit-value path rewritten from `atoi(num->value)` to
  `strtol + errno/ERANGE + INT_MIN/INT_MAX + endptr-consumed` with
  `iron_emit_diag` on out-of-range (parser.c:2585 site; FIX-01 rank 19).

### Phase 67-07 (FIX-03 cross-arena pointer ownership)

Landed with the 16-row AUDIT-04 walkthrough.

- `tests/integration/arena_cross_arena_fix.iron` — exercises the fused
  `.map().map().filter().sum()` code path whose per-map `cur_var` calloc
  scratch buffers (`emit_fusion.c:173` flat branch + `emit_fusion.c:366`
  split branch) are now tracked in two stb_ds `char **` arrays and
  explicitly `free`'d at function exit. Pre-67-07 each `map` node in a
  fused chain leaked a 32-byte scratch buffer; the fixture's three-map
  chain forces three calloc calls per compile, all three of which are
  now reclaimed. AUDIT-04 §8 closed. Matching fixture expected output
  `102` is derived from `[1..10] → +1 → *2 → filter(>10) → sum`.

### Phase 67-08 (REG-02 crash-canary fixtures)

Phase 67-08 added 15 crash-canary fixtures, one per AST node kind family
the HIR-to-LIR walker handles. Each compiles cleanly in both Debug and
Release CI and round-trips a deterministic integer-only output. Fixtures
exercising partial features ship with a `# KNOWN GAP: <future-phase>`
header documenting the gap and pointing at the phase that will close
it. These canaries are the runtime regression net complementing Phase
66's structural protections (PROT-01..04, REG-01 Linux Release CI job,
`-Werror=switch-enum`, `IRON_NODE_ASSERT_KIND` debug guards). Any future
change to `src/hir/hir_to_lir.c` that regresses the walker's handling of
a specific node kind family turns the corresponding canary red
immediately.

- `tests/integration/hir_canary_if_elif_else.iron` — `IRON_HIR_STMT_IF`
  with `elif` chain (catalogue entry complementing the original
  `hir_to_lir_elif_mono_walker.iron` Phase 59 fixture).
- `tests/integration/hir_canary_while.iron` — `IRON_HIR_STMT_WHILE`
  (classic sum-1-to-N accumulator).
- `tests/integration/hir_canary_for.iron` — `IRON_HIR_STMT_FOR`
  (factorial + sum-of-squares over `range(n)`).
- `tests/integration/hir_canary_assign.iron` — `IRON_HIR_STMT_ASSIGN`
  including the compound forms `+=`, `-=`, `*=`.
- `tests/integration/hir_canary_nested_if.iron` — nested
  `IRON_HIR_STMT_IF` at depth 2, exercising walker recursion into an
  IfStmt whose body/else\_body both contain further IfStmt nodes.
- `tests/integration/hir_canary_match.iron` — `IRON_HIR_STMT_MATCH`
  with four arms over an integer scrutinee.
- `tests/integration/hir_canary_spawn.iron` — `IRON_HIR_STMT_SPAWN`
  single-spawn-with-await pattern returning a deterministic Int.
- `tests/integration/hir_canary_parallel_for.iron` —
  `IRON_HIR_EXPR_PARALLEL_FOR` (outer-scope-free body + sequential
  aggregate for deterministic output).
- `tests/integration/hir_canary_tuple_destructure.iron` — 2-tuple
  destructure only, with a `KNOWN GAP` header noting that 3+ tuple
  destructure is a pre-existing gap (Iron v0.1.4-alpha 2-tuple-only
  codegen, not a Phase 67 bug).
- `tests/integration/hir_canary_enum_construct.iron` —
  `IRON_HIR_EXPR_ENUM_CONSTRUCT` with a 3-variant enum + method-on-self
  match (complements `enum_construct_reinterpret.iron`).
- `tests/integration/hir_canary_method_call.iron` —
  `IRON_HIR_EXPR_METHOD_CALL` (instance method on an extends-less
  object); ships with a `KNOWN GAP` header noting `super.method()`
  dispatch is deferred to a future phase.
- `tests/integration/hir_canary_static_call.iron` — static/namespace
  call dispatch via `Math.sign(x)` (the only stdlib static call that
  returns an integer, making it the one deterministic integer-only
  option).
- `tests/integration/hir_canary_lambda.iron` —
  `IRON_HIR_EXPR_CLOSURE` lambda without capture (`func(x: Int) -> Int`).
- `tests/integration/hir_canary_closure_capture.iron` — closure
  capture of mutable outer state via `+=` inside the closure body;
  complements the Phase 67-02 FIX-01 rank-3 closure-env malloc guard.
- `tests/integration/hir_canary_heap_expr.iron` —
  `IRON_HIR_EXPR_HEAP` via `heap Point(x, y)`; overlaps deliberately
  with `null_heap_alloc_malloc.iron` (that one is the Phase 67-02
  FIX-01 rank-3 OOM-guard fixture; this is the REG-02 catalogue entry).

### Phase 68 — Fuzzing Infrastructure

- `tests/integration/fuzz_crash_*.iron` — Auto-generated by
  [`scripts/fuzz_crash_to_fixture.sh`](../scripts/fuzz_crash_to_fixture.sh)
  from libFuzzer crashes discovered by the nightly
  [`.github/workflows/fuzz.yml`](../.github/workflows/fuzz.yml) workflow.
  Follow the 4-section template with the FUZZ-06 variant documented in
  the "Fuzz-Discovered Fixtures" section below. Expected to grow over
  time as the nightly fuzz workflow discovers new bug classes. **No
  fixtures at Phase 68 landing** — the machinery ships empty; organic
  nightly runs populate it.

## Fuzz-Discovered Fixtures (FUZZ-06 Auto-Stub Format)

Phase 68 adds a nightly libFuzzer workflow
(`.github/workflows/fuzz.yml`) that runs three targets — `fuzz_parser`,
`fuzz_typecheck`, and `fuzz_hir_to_lir` — for 10 minutes each against
the `main` branch. Any discovered crash in the parser target is
automatically wrapped as a permanent integration-test regression
fixture at `tests/integration/fuzz_crash_NNN.iron` with a companion
`tests/integration/fuzz_crash_NNN.expected` by the
`scripts/fuzz_crash_to_fixture.sh` harness.

The auto-stub uses a minor variant of the 4-section template above.
Sections 1 and 4 stay the same; sections 2 and 3 are re-labeled for
the fuzz-crash use case:

1. **Motivating Incident.** `libFuzzer nightly run at commit <SHA>
   produced this crash (target=parser, seed=1, signature=<sig>,
   input-hash=<short-sha>, run=<run-id>).` — auto-filled by the
   harness from `github.sha`, `github.run_id`, and the minimized
   input.
2. **Symptom.** The first ~5 frames of the ASan/libFuzzer crash
   stack trace captured by running `fuzz_parser -runs=1 <minimized>`.
   This replaces the "Layout Diagram" section because a fuzz-found
   bug rarely has a known struct-layout cause until a human
   investigates.
3. **Protection.** `Fixture pins the minimized input verbatim;
   tests/integration/run_integration.sh exits non-zero on regression
   (Phase 66 REG-01 crash-swallow prevention).` — constant. This
   replaces the "Fix Summary" section because the fix hasn't landed
   yet at the moment the fixture is born.
4. **Remediation Path.** `TODO: fill in after the fix lands.` — the
   one human-polished section. When a maintainer lands the fix for
   the bug, they update this line to a one-sentence summary of the
   file:line that was changed and the rationale, matching the
   standard "Fix Summary" semantics in the original template.

The fixture is **red the moment it lands** — the `.expected` file
is the current crashing stderr, so `run_integration.sh` exits
non-zero the next time it runs. When the bug is fixed, the fix
author updates `.expected` to the post-fix output (typically a
clean compile) as part of the fix commit. This matches the
"fixture is red until fixed" decision in
`.planning/phases/68-fuzzing-infrastructure/68-CONTEXT.md`.

**Typecheck and hir_to_lir targets** (which consume IRTB binary
blobs, not Iron source text) do NOT emit fixture files. Their
crashes are uploaded as workflow artifacts via
`actions/upload-artifact@v4` and a `fuzz-crash`-labeled issue
is opened via `scripts/fuzz_crash_to_fixture.sh --open-issue`.
A human can reproduce those crashes locally by downloading the
artifact and running `./build/tests/fuzz/fuzz_<target> <input>`.

The harness dedupes issues by crash signature (sha1 of the top 3
stack frames, truncated to 8 hex chars). A repeated crash in
successive nightly runs appends a comment to the existing issue
rather than opening a new one; closed issues are **never reopened
automatically** — a human decides whether a resurfaced crash after
a close is a real regression.

## Why This Matters

The v0.1.4-alpha Compiler Correctness milestone exists because a single
SIGSEGV in `collect_mono_enums_node` lived in the tree for months. The
Phase 59 Benchmark workflow caught it intermittently but reported it as
"benchmark compilation failed" — a generic failure class that hid the
signal. The response is threefold:

1. **Prevent.** PROT-01..04 structural protections make the bug class
   impossible to reintroduce silently. PROT-01 publishes `Iron_ExprNode`
   in `src/parser/ast.h` with compile-time `_Static_assert`s on the
   common prefix; PROT-02 enables `-Werror=switch-enum` globally; PROT-03
   adds the debug-build `IRON_NODE_ASSERT_KIND` macro; PROT-04 rewrites
   every AUDIT-01 H-severity blind-cast site to use the real struct and
   a preceding kind check.
2. **Detect.** REG-01 runs the Linux Release CI job that would have
   caught the original crash, and `tests/integration/run_integration.sh`
   now refuses to swallow SIGSEGV as generic test failure noise — a
   signal-level exit from ironc is reported as `[CRASH]` and fails the
   script immediately, not silently counted as a per-fixture build
   failure.
3. **Document.** Every fix lands with a regression fixture that cites the
   motivating incident. Future contributors reading a red test know
   exactly which bug class they are staring at — and why the fix looks
   the way it does.

This template is the third leg. Do not skip it.

## See Also

- `.planning/research/CORRECTNESS-AUDIT.md` — The ranked audit findings
  every PROT-04 / FIX-01..04 regression fixture cites.
- `.planning/REQUIREMENTS.md` — Requirements PROT-01..04, REG-01, REG-04
  for v0.1.4-alpha.
- `.github/workflows/ci.yml` — The `build-and-test-release` CI job that
  runs these fixtures in Release mode to catch optimization-dependent
  regressions.
- `tests/integration/run_integration.sh` — The integration test runner
  with Phase 66 REG-01 exit-code policy: signal-level exits (>= 128) are
  reported distinctly and do not get swallowed as generic test failures.
