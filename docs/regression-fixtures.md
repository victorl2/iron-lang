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

### Phase 67 REG-02 (crash-canary fixtures — planned)

Phase 67 will add one fixture per AST node kind that the HIR-to-LIR walker
handles. Each follows this template with the Motivating Incident section
quoting the specific walker incompleteness the canary targets.

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
