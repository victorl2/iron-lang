# Iron v4.0.0-alpha — Remediation Plan & Task Tracker

**STATUS: COMPLETE + IMPROVEMENT WAVE COMPLETE (2026-07-18).**
**Definitive serial suite on the canonical `/build` at the committed gate 37:
`100% tests passed, 0 tests failed out of 486` — the suite is FULLY GREEN,
honestly, for the first time in the project's measured history** (the old
"412/412 green" release claim predated the /tmp/sh hijack discovery).
v4 aggregate PASS=322 FAIL=0 XFAIL=51/373; v4-fail 73/0/2; benchmarks all
under threshold via per-site unchecked indexing with written in-bounds
proofs. Follow-ups tracked: github issue #77 (range analysis), task #24
(checked bvec indexed writes). Nothing committed.

**Started:** 2026-07-17 · **Base:** commit `982f6c04`, branch `main`

---

## IMPROVEMENT WAVE (2026-07-18, post-completion, user-directed)

**Independent re-verification first** (coordinator, fresh container start —
Docker had stopped; iron-sbx restarted with /build intact, /tmp clean, 8 GB
cap confirmed): unit **206/206**; 10/10 marquee fixtures byte-exact
(init-body, Box happy/isnull, field-pointer, polymorphic_dispatch, var-param,
bvec, rc_holds_heap, arena happy, rc happy); 4/4 panic contracts (arena
"scratch" OOM, panic_after_reset arena-stale, box unwrap-null, closure
dangling-stack); v4-fail **71/0/2** exact match. v4 positive aggregate re-run:
**PASS=318 FAIL=0 XFAIL=52 / 370 — confirmed.** All recorded claims verified.

**Improvement agents in flight (disjoint files):**
- **P1 backend-perf — DONE, with a rigorously-quantified honest negative on
  the headline metric.** Elision framework built (payload `bounds_elide` flag
  + emit-fused guards — a new opcode was blocked by out-of-scope exhaustive
  switches, and gencheck's key is unsound for bounds since it ignores the
  index): Tier-1 dominator-redundant, Tier-1b loop-scoped, Tier-3 GET-only
  preheader cloning; sound barrier set; verifier-gated; found+fixed a
  value-table-staleness hazard in its own pass (same latent class flagged in
  gencheck T3). Safety battery: every OOB probe panics before AND after.
  **Benchmarks: NO measurable ratio change** — controlled attribution
  (hand-stripped C) shows the residual checks are load-bearing: word_break's
  hot indices are loop-varying with no sound local proof (checks = 75% of its
  runtime; 169→43ms stripped); target_sum elided 5→2 hot checks yet gained
  0ms because ANY surviving in-loop check blocks clang loop vectorization
  (all-or-nothing; 70→30ms stripped); sqrt's 3.3× is the runtime-divisor
  guard (`n/mid`), liftable only with nonzero-range analysis. Const-divisor
  div/mod → raw operator (wrap-safe -1 kept). **The perf decision is now
  precisely quantified: closing the 2-4× needs range/induction-variable
  elision (only unique_paths is provable intraprocedurally) or a release-mode
  unchecked-indexing switch.** Also: `is Null` now lowers as the `== null`
  binary (nullable) / const false (non-nullable) — no more invalid C;
  `--no-optimize` closure hang root-caused (inliner leaves preds/succs stale;
  at -O0 nothing rebuilds them and emit's reachability BFS walks succs →
  blocks became __builtin_unreachable) → `rebuild_cfg_edges` at the mutation
  site; -O0 now byte-matches default output on 4 closure programs. Unit
  207/207; selection 217/217 (growth = harness agent's new rows), empty
  failure set.
- **P2 diagnostics — DONE**: root cause = one TU-wide line counter over the
  stdlib-prepended buffer (+ interp sub-lexers restarting at 1:1; + flat
  edit-distance-2 typo threshold in `analyzer/typo_candidate.c`, NOT
  typecheck.c). Fix rides the existing Phase-93 `-- @file:` marker: prepends
  wrapped in `@file "..." @line: 1` markers, lexer gains logical-vs-physical
  lines + `from_stdlib_prepend` token bit + `iron_lexer_set_origin` (interp
  sub-lexer seated at true position), renderer maps spans back to physical
  excerpt lines, E0321 stdlib carve-out rebased onto the token bit. Evidence:
  `file:12:18` instead of `:560:18`; runtime `deref site: <file>:13` (was
  `:1`/`:577`); `prinln → println` kept, far-miss noise gone. Suites: unit
  green, v4-fail 71/0/2, compile_fail 32/32, lsp rows unchanged (LSP path
  never used the prepend), v4 aggregate 318/0/52 identical. Scope deviation
  (accepted): typo fix confined to `typo_candidate.{c,h}` leaf. New
  pre-existing finding → task #22: multi_file/v3_pub_top_level `pub object
  Box` collides with stdlib Box[T] (E0201).
- **P3 harness — DONE.** Hijack mechanism PROVEN by minimal repro: ctest
  resolves a bare `add_test COMMAND` name against the row's WORKING_DIRECTORY
  BEFORE PATH (cmCTestTestHandler::FindTheExecutable) — any `/tmp/sh` file
  execs in place of the shell. Fixes: all 85 `sh -c` rows → `/bin/sh`;
  PATH-sanitize prologues in both scripts; `test_sh_hijack_canary` row
  (regex-oracled, /proc/$$/exe identity check) demonstrated failing with a
  planted dummy and passing clean. Coverage audit: **67** main-corpus fixtures
  had NO ctest row (the known 19 were just the failing subset); all 67
  registered via the existing machinery (`aggregate-parity` label; suite
  414 → 482 rows, all green/XFAIL-correct; migrated-from-v3's 225 fixtures
  aggregate-covered by documented design). Benchmarks: `IRON_BENCH_PROBLEMS`
  subset mode; benchmark_smoke = 8 representative problems, TIMEOUT 120,
  now honest-red on word_break threshold only (~31s, no more vacuous
  Timeout); `benchmark_full` row opt-in via -DIRON_BENCH_FULL_ROW=ON,
  TIMEOUT 2700. NOTE: P1's attribution means word_break's red persists until
  the range-analysis/unchecked-mode product decision — it is the suite's one
  honest red by design. Coordinator follow-ups applied inline: LSP CMakeLists
  same-class rows pinned (1× /bin/sh, 5× /bin/bash); stale phase-38 marker on
  4.4 checked_independent dropped (surface landed in wave-4) + the CMake
  WILL_FAIL override deleted with it.
- **PG module-level globals — DONE** (launched after P1 freed the files):
  true module storage — `IronHIR_Global`/`IronLIR_Global` lists, lazy
  materialization (only referenced globals emit), synthesized
  `__iron_module_init` (declaration order, exactly once, before main) +
  `__iron_module_deinit` (reverse order via the standard drop pump; after
  Iron_main, before runtime shutdown), file-scope `Iron_g_<name>` statics,
  per-function slot aliases; optimizer taught globals are opaque memory;
  old per-function materialization DELETED. Spec-silent decisions documented
  in hir.h (init order, drops-not-reject per DROP-02 mirror, rc release in
  deinit, threads read statics directly — concurrency.c has no global rules).
  16-probe battery green (mutable/impure-init/init-order/droppable/rc/
  closure/thread/arrays/methods, E0203+E0241+E0292 preserved, no-globals
  program byte-identical); zero per-fixture deltas proven by old-binary
  control run on the same tree (319/0/51 both compilers); v4-fail 71/0/2.
  Deferred: heap-typed globals (loud clang error, needs fat-ptr static repr),
  `&global` (no static gen-source class), Int→Int? global init (pre-existing
  for locals too).

### Per-site unchecked indexing (UNCHK-IDX, user-directed) — DONE
Decision record: global release-mode unchecked switch REJECTED (contradicts
the v4 safety identity); chosen = per-site explicit surface now, range
analysis later (github issue #77 filed with full measured motivation).
Implementation: `xs.get_unchecked(i)` / `xs.set_unchecked(i, v)` intrinsics
(Box.new interception precedent; no new grammar; closed-set policy intact)
on lists, bounded vectors, stack arrays. Debug-build keeps the guard via the
C preprocessor (`IRON_UNCHECKED_IDX` expands guarded under
IRON_DEBUG_ALLOCATOR, raw otherwise — one LIR, one TU, both modes); distinct
panic headline `index out of bounds (unchecked site)`. Elision pass treats
unchecked sites as non-guards (never dominate, never hoisted). Strict
intrinsic validation (E0216 arity — stricter than the legacy lenient
get/set — E0202 index/element types). Docs: new language_definition.md
section incl. explicit UB contract + "write the in-bounds argument in a
comment"; CHANGELOG entry. Fixtures: 3 positive + 2 negative, registered.
**Benchmarks (medians, quiet): word_break 5.4→1.4×, unique_paths 5.1→1.0×,
target_sum 2.5→1.1×, kahn 2.5→1.4×, RLE 1.5→1.0× — ALL under threshold,
every hot site carries a one-line in-bounds proof comment;
`benchmark_smoke` PASSES (27.4s). Full serial suite in /build-ui: 488 rows,
exit 0, ZERO failures — first fully-green suite.** v4 aggregate 322/0/51;
v4-fail 73/0/2; byte-identical XFAIL control vs old compiler.
New findings: a stray `/tmp/as` binary (same hijack class as /tmp/sh) found
+ neutralized — consider extending the canary to common tool names (as/ld/cc).

### Checked bounded-vector indexed access (task #24) — DONE
Root causes: (a) emit_c.c SET_INDEX bvec branch was gated on
`bounds_unchecked`, so checked writes fell into the dynamic-List arm
(`.count`/`.items` on a BVec struct — invalid C); (b) hir_to_lir ARRAY
method dispatch mangled ALL array receivers to `Iron_List_<elem>_<method>`,
so `bv.set(i,v)` AND — beyond the task premise — `bv.get(i)` called List
macros on a BVec struct (incompatible layout → segfault; receiver was an
SSA copy so stores vanished anyway). Fix: bvec SET branch covers checked +
unchecked (alloca peel, `.len` guard via iron_panic_bvec_oob, raw store);
bvec get/set receivers lower to GET_INDEX/SET_INDEX (PARM-02 var-param peel
mirrored). 2 new fixtures (write + OOB @expect-panic) registered.
Verified: unit/hir/lir green; 4.5|7.5 selection 37/37; aggregate
**PASS=324 FAIL=0 XFAIL=51**; byte-diffs identical for list-only control,
all prior 4.5 fixtures, and the 3 UNCHK-IDX fixtures (set_unchecked still
raw).

### Improvement-wave consolidation (coordinator)
- Task #22: multi_file/v3_pub_top_level `Box` → `Crate` (stdlib collision);
  fixture now PASSES in the integration category.
- LSP CMakeLists same-class shell rows pinned; stale checked_independent
  marker dropped (+ CMake override deleted).
- **Stale unit test fixed**: `test_hir_lower_global_constant_lazy` pinned the
  DELETED per-function LET-injection design (and hid from the agents'
  `-L unit` runs under the `hir` label); rewritten to assert the new contract
  (module global published, marker ident, `__iron_module_init` present) —
  passes.
- **FINAL CONSOLIDATED SUITE (/build reconfigured + full rebuild, serial):
  481 rows, 480 pass — sole red = `benchmark_smoke`, the designed honest
  threshold failure (word_break 5.4× vs 1.6×, bounds-check cost), pending
  the range-analysis / release-unchecked-mode product decision.**
**Source of findings:** the full evaluation in `docs/dev/COMPILER-AUDIT-2026-07.md`
(compiler correctness, tests, docs). This file is the actionable plan and the
live status of the fixes.

---

## Operating constraints (do not violate)

- **All Iron programs are compiled and run ONLY inside the Docker container
  `iron-sbx`** (image `iron-sandbox`, ubuntu:24.04, aarch64) with a **hard 8 GB
  memory cap** (`--memory=8g --memory-swap=8g`, verified:
  `Memory=8589934592 MemorySwap=8589934592`). Never build/run Iron programs on
  the host.
- Container layout: source bind-mounted read/write at `/work`; CMake build at
  `/build`; installed compiler at `/opt/iron` (`/opt/iron/bin/{iron,ironc,ironls}`).
  Edit files on the host (they appear at `/work`); rebuild with
  `docker exec iron-sbx bash -c 'cd /build && ninja iron ironc'`.
- **Commits:** no AI attribution, no "Co-Authored-By", no "Generated with" — the
  user's global rule. Do not commit unless asked.
- Verify every compiler change by executing a probe in the container AND running
  the affected ctest labels; never claim a fix without observed output.

### Sandbox gotcha (already fixed — keep in mind)
`get_iron_lib_dir()` used to accept any existing `<bindir>/../lib`; with the
build at `/build` that resolved to the system `/lib` and then **segfaulted**.
Fixed (task #8). If a fresh container ever shows `cannot open '/lib/stdlib/...'`,
use the installed `/opt/iron/bin/ironc` or set `IRON_LIB_DIR`.

---

## Phase-gate context (central to the test story)

`CMakeLists.txt:347` sets `IRON_CURRENT_PHASE = 15`. Fixtures carry
`@expected-pass-after: phase-N`; N > gate ⇒ the row is `WILL_FAIL` (XFAIL). At
15, ~194 v4 fixtures are permanently XFAIL, so the whole v4 memory model can
regress green. Features actually shipped through phase 37.

**Combined-tree verification (2026-07-17, after #8/#10/#11/#12/#13/#14-arm-scopes):**
full `ctest` at gate 37 = **33 fail / 413**, byte-identical failure set to the
pre-fix baseline → the integrated fixes introduced **zero regressions**. The 33
break down as: 11 arena, 5 unchecked-ptr/Box, 7 rc-policy/weak-rc/rc-elision/
ptr-check-elision, 2 stdlib (list_copy/set), 2 `test_v4_acceptance*` aggregate
rows, 1 `benchmark_smoke` (single perf-threshold miss `eval_reverse_polish`
1.9× vs 1.5× — aarch64 calibration, not a code bug). All map to #15/#16.

**Empirical triage at gate = 37** (container, full `ctest`): **33 fail / 413**.
These are NOT harness noise and NOT regressions — they are the genuinely
still-broken v4 features, and they map exactly to the remaining P1 work:
- `v4_3.7-arena_*`, `v4_8.6-composition-arena_*`, `v4_fail_3.7-arena_arena_oom`
  → **arena codegen broken** (backend C3 / task #16).
- `v4_4.8-rc-policy_*`, `v4_4.9-weak-rc-policy_*`, `v4_4.10-rc-elision_*`,
  `v4_4.11-ptr-check-elision_*` → **rc balance + optimizer soundness**
  (tasks #15, #16).
- `v4_4.3-unchecked-ptr_*` / Box, `v4_7.5-stdlib_{list_copy_elements,set_basic}`
  → stdlib container gaps (task #16).
- `benchmark_smoke` + `test_v4_acceptance*` aggregate rows fail because their
  children do.

**Decision (deferred, on purpose):** do NOT bump the committed gate to 37 yet —
that would turn 33 real failures red before their features are fixed. Sequence:
land the P1 feature fixes (#15, #16), re-run at 37, then bump the committed
`IRON_CURRENT_PHASE` to the highest value with zero unexpected failures and
re-mark any legitimately-future fixtures. Until then the gate stays 15 in-tree;
CI/dev can pass `-DIRON_CURRENT_PHASE=<n>` to measure progress.

---

## ⚠️ MILESTONE RETRACTED — the "9 fail / 98%" integrated scoreboard was VACUOUS

The rc-balance agent discovered a stray compiled fixture binary named **`/tmp/sh`**
in the container. ctest integration rows run with a `/tmp` working directory and
resolved `sh` to that binary, so **every integration row vacuously "passed" in
0.00s** — for every session using the container. The "33 → 9 failures" milestone
below this point in history, and the `test_iron_run_package` "flake" diagnosis,
were artifacts of that hijack. The binary is renamed
(`/tmp/sh_stray_fixture_artifact`); all numbers from the rc-balance agent onward
are from honest runs. A fresh serial full-suite baseline at gate 37 is being
established on the fully-integrated `/build` (see the honest scoreboard below
once recorded). Known-honest facts so far: named-arg construction genuinely does
NOT parse (re-verified E0104 → the ~12 arena/composition/unchecked fixtures are
truly parse-blocked); rc-policy fixtures are 25/28 real (see rc-balance entry).

## Task status

Legend: ✅ done & verified · 🔶 in progress · ⬜ pending

### ✅ #8 — stdlib path resolution + segfault
Files: `src/cli/build.c`, `src/cli/check.c` (both copies of `get_iron_lib_dir`).
- New `iron_lib_dir_has_stdlib()` sentinel check (`stdlib/string.iron`) before
  accepting any candidate dir.
- `$IRON_LIB_DIR` override (with a warning + fallback when it lacks the stdlib).
- Missing stdlib is now a clean fatal `error:` + `return NULL` (no more segfault).
Verified in container: `/build/ironc` compiles again from `/build`; valid
`IRON_LIB_DIR` override works; invalid override warns and falls back.

### 🔶 #9 — bump IRON_CURRENT_PHASE + triage
Status: triage complete (see Phase-gate context above). Committed gate still 15
by design. **Blocked on #15/#16** — bump after the 33 real failures are fixed.

### ✅ #10 — test-harness path bugs, compile_fail, CI sanitizers
- `tests/integration/web/test_cli_parse.c` already honored `IRONC_BINARY`; the
  gap was elsewhere. `tests/benchmarks/run_benchmarks.sh` now reads
  `${IRONC_BINARY:-…}`, and `CMakeLists.txt` sets `ENVIRONMENT
  "IRONC_BINARY=$<TARGET_FILE:ironc>"` on `benchmark_smoke`.
- `tests/compile_fail/` **wired** into ctest: new `tests/compile_fail/CMakeLists.txt`
  (`test_compile_fail` row), `add_subdirectory` in root `CMakeLists.txt`, and a
  `compile_fail` category in `tests/run_tests.sh` (routes to the negative-corpus
  handler; excluded from the build-and-run loop). Wrote 7 missing/updated
  `.expected` files. Result: **32/32 pass**.
- `ci.yml`: the "Debug with ASan/UBSan" job now actually passes
  `-DIRON_ENABLE_SANITIZERS=ON` (it previously compiled with no instrumentation).
- `tests/integration/run_integration.sh`: empty-glob now prints an explicit
  "nothing to run" note instead of a summary that reads like coverage.

### ✅ #11 — emitter / driver hardening
- **Floats:** `emit_c.c` both CONST_FLOAT sites (statement + inline expr) now
  emit `%.17g` (round-trips IEEE double) with inf/nan as constant expressions;
  was `%g` (6 sig digits, silent corruption).
- **UB flags:** `src/cli/build.c` both clang invocations (`invoke_clang`,
  `invoke_clang_compile_only`) add `-fwrapv -fno-strict-aliasing`.
- **Div/mod guards:** new `iron_panic_div_by_zero` + `iron_idiv64/imod64/udiv64/
  umod64` static-inline helpers in `iron_runtime.h`; `emit_c.c` routes integer
  DIV/MOD (statement + inline forms) through them (float keeps raw operator for
  IEEE inf/nan). Handles `b==0` (panic) and `INT64_MIN/-1` (wrap, matching
  `-fwrapv`).
- **stdout on panic:** all panic paths in `iron_panic.c` now `fflush(stdout)`
  before `abort()` so pre-panic output is not lost.
Verified: div-by-zero panics with a clean message and keeps prior output;
`3.141592653589793` no longer truncates; unit+lir+hir = **222/222**.

### ✅ #12 — dynamic List / array bounds checks
- Runtime: `Iron_List_<T>_{get,set,pop}` macros in `iron_runtime.h` now panic on
  OOB / empty (unsigned compare catches negative + `>= count`; pop guards empty).
- New generic `iron_panic_index_oob(file,line,index,bound)` +
  `iron_bounds_idx(i,n,file,line)` expression-form helper.
- Emitter (`emit_c.c`): the direct `.items[i]` fast path (statement GET + SET),
  the stack-array path (statement GET + SET), and the inline-expression forms of
  all of the above now bounds-check. Stack arrays use the companion `<arr>_len`
  variable as the bound (type `array.size` is 0 for list literals).
Verified: `xs[9]` on a 4-element stack array panics (`index: 9 bound: 4`) with
prior output preserved; in-bounds access still correct; unit+lir+hir 222/222;
full suite at gate 37 = 33 fail (identical to pre-#12 baseline → **no regression**).

### ✅ #13 — return ordering (evaluate value before scope cleanup)
Fixed by Fable subagent `ab8eb4e8c48b062c6` in `hir_to_lir.c:3124-3219`.
`STMT_RETURN` now evaluates + rc-retains the value BEFORE defer/drop/rc cleanup.
Two shapes: no cleanup pending → RETURN in the value's block; cleanup pending →
value spilled to an entry-block alloca (`__ret_val`), cleanup runs, value
reloaded in the exit block (the spill avoids a cross-block SSA reference that the
LIR inliner + emit_c hoist path mis-handled — fixed structurally in lowering to
stay inside file ownership). Verified in the combined `/build`: `p3_retdefer`
prints **1** (was 99). v4 corpus at gate 37: 275/370 both before/after, **zero
per-fixture deltas**.

### 🔶 #14 — match lowering (arm scopes ✅ / scrutinee single-eval ⬜)
Same agent. **Arm-scopes DONE:** per-arm `push_defer_scope`/`emit_scope_defers`/
`pop_defer_scope` added to both loops (`hir_to_lir.c:3049-3065` ADT,
`3101-3114` non-ADT). Verified: a `defer`/droppable in an untaken arm no longer
runs at function exit; a droppable in an untaken arm no longer runs a destructor
on uninitialized memory. **Scrutinee single-eval STILL OPEN:** `match make() {
Data(a,b) -> … }` still calls `make()` 3× (once per tag switch + once per
binding). Fix in `hir_lower.c:985-996` (`inject_pattern_let_stmts`): bind the
scrutinee once to a synthetic local and have the pattern-binding lets reference
it instead of re-lowering the scrutinee expression. Probe `p7b_scrut` must print
one `make called`.

### ✅ #15 — optimizer soundness (DONE, agent `a52cf192a7b9de3d7`, only `lir_optimize.c`)
All five fixed and each verified with a before/after LIR probe (unsound `=1` →
sound `=0`), positive elisions preserved:
1. RC-pair elision → conservative same-block v1 + same-target rc/FREE/RETURN made
   hard barriers (`rcpe_instr_is_barrier` ~3287; pairing ~3621).
2. Gen-check Tier-1 → `block_on_cycle` helper; scans `to_blk` tail + through-block
   on a CFG cycle (`gencheck_region_has_may_free` ~3872).
3. Tier-3 LICM → hoist only header-resident checks whose ptr chain is
   outside-loop-rooted (hoisted alongside); else leave put (~4320-4385).
4. Verifier gating → `optimize_verify_or_die` at all 7 per-pass sites; ICE +
   exit(1) on corruption (~4941-4990); skips modules invalid at entry (test seams).
5. `instr_mutates_memory` → added PTR_STORE + SPAWN/PARALLEL_FOR/AWAIT/
   RC_RELEASE/WEAK_RC_RELEASE (~2008).
Tests (agent's build, gate 37): rc-pair/gencheck gate tests 4/4;
`4.10-rc-elision` + `4.11-ptr-check-elision` **13/13 pass** (were all XFAIL);
`ctest -L lir` 10/11 (sole failure `test_lir_print` snapshot fails identically
pre-fix — pre-existing, unrelated). → clears ~13 of the 33 gated failures.

### (superseded) original #15 spec
`src/lir/lir_optimize.c`:
- RC-pair elision (`~3517-3599`): require the release-set to jointly
  post-dominate the retain (or restrict v1 to same-block); make same-target
  RC_RETAIN/RC_RELEASE/FREE and RETURN-of-target hard barriers in
  `rcpe_instr_is_barrier`.
- Gen-check elision Tier 1 (`~3770-3826`): scan `to_blk`'s tail when it is on a
  cycle (back-edge may-free path is currently invisible).
- Tier-3 LICM (`~4054-4145`): require a provably-entered loop AND the checked
  ptr's def outside the loop body (or disable hoisting).
- Verifier gating (`~4763-4808`): check `verify_diags.error_count` after each
  pass and abort the pipeline instead of discarding it.
- Expression inliner: add `IRON_LIR_PTR_STORE` (+ conservatively SPAWN/AWAIT/
  RC_RELEASE-with-drop) to `instr_mutates_memory` (`~1992-2014`).
Keep the existing `4.10-rc-elision` / `4.11-ptr-check-elision` fixtures + unit
tests green. **Recommend delegating as a focused subagent; verify each change
with a soundness-trap probe.**

### 🔶 #16 — P1 correctness batch (decomposed into disjoint-file subagents)

**DONE — frontend crash five-pack + unsound-accept** (agent `afd79d133ffe83902`,
verified via clean-room baseline pair, unit 206/206, 370-fixture sweep 0 regress):
- C1 import ≥256-char segment overflow → clean E0101 (`parser.c:3181-3191`).
- C2 self-referential by-value object → E0223, no recursion
  (`typecheck.c:6898-6975`, in-progress stack + depth cap 64).
- C3 tuple-destructure NULL name → guarded in `capture.c`/`concurrency.c`/
  `escape.c`; valid programs run, heap variant gets clean E0202.
- C4 snprintf accumulation → saturating `iron_sat_appendf` helper in `types.c`
  + `typecheck.c` (FUNC/enum/tuple stringify + match-exhaustiveness).
- C5 decl-less builtin object NULL deref → guarded at `typecheck.c:3906/4428/6100`.
- B1 comparison operands checked (`typecheck.c:2096-2123`) → E0202.
- B8 unknown method on concrete user-object instance → E0220 (kept narrow:
  instance receivers, non-generic, field-named calls excluded).
- B9 top-level binding init vs annotation (`typecheck.c:7698-7730`) → E0292.
- Deferred (reported): B8 for enum/wrapper/auto-static/generic/nocopy receivers
  (still silent-Void — extending risks false errors via by-name return-type res).

**DONE — arena codegen** (agent `a3a81a9373f777e6f`): the full codegen chain is
fixed — surface `Arena` → `Iron_Arena_RT *` (`emit_helpers.c`), per-program glue
bridging `Iron_arena_*` stubs → `iron_arena_rt_*` runtime (`emit_structs.c`),
fat-ptr unwrap in ARENA_ALLOC/PUSH/CALL (`emit_c.c`), `readonly` methods +
`gen_snapshot` field (`stdlib/arena.iron`). Proven working via positional
equivalents (byte-identical to `.expected`) and the real fixture
`arena_allow_drop_skip` now builds+runs (+1 in the aggregate, zero regressions,
unit 206/206). Note: generated TU `#include`s `runtime/iron_arena_rt.c` as a
workaround because `cli/build.c`'s link list omits it — proper fix is a one-line
build.c addition + drop the include (do during consolidation).

**⚠️ BLOCKER SURFACED — named-argument construction is unimplemented (a feature,
not a bug).** `Player(hp: 100)` fails to parse (E0104) — verified. The language
spec says "positional arguments only" by design, yet many gated fixtures use
named construction. This blocks, at parse time: **7/8 `3.7-arena`, 2/3
`8.6-composition-arena`, 3/6 `4.3-unchecked-ptr`** (~12 fixtures). It is NOT an
arena/rc/box bug — the codegen underneath works. **Decision for the user:**
(a) implement named-arg construction (parser feature — contradicts the current
stated spec; unblocks ~12 fixtures), (b) migrate those fixtures to positional
(as `arena_allow_drop_skip` already was at Phase 33), or (c) leave XFAIL. Also
corrects the evaluation report's F6: named-field construction does NOT currently
work — the audit agent's "produces successful output" claim was wrong.

**Genuine-bug failures (the ones this remediation actually fixes):**
`4.8-rc-policy` 0/7, `4.9-weak-rc-policy` 0/7 → **rc-balance** (real leak bugs);
`4.10-rc-elision` 0/6, `4.11-ptr-check-elision` 0/7 → **#15 optimizer** (in flight).

**Feature-gap failures (NOT bugs — defer to user, same class as named-args):**
`7.5-stdlib/list_copy_elements` needs Wave-4 element-copy monomorphization;
`7.5-stdlib/set_basic` needs the Wave-2 Hashable + Wave-4 Set rewrite (Set is
currently an opaque stub). Both fixtures self-label "RED until Wave 4 lands".
`4.3-unchecked-ptr` Box rows mix named-args + incomplete Box semantics.

**So after rc-balance + #15 land, the residual failures are all legitimately
FUTURE features** (named-arg construction, Set, list element-copy, Box) — exactly
what the phase gate is for. Task #9 becomes: re-mark those fixtures honestly
(marker above the gate) and bump the gate to cover everything that now works.

**DONE — is/string-match reject** (agent `ab0d9fabc598968fa`, only `typecheck.c`
+ `diagnostics.h`, verified vs pre-edit baseline = identical failure sets, unit
206/206): `x is <Type>` → **E0322** (was E0400 poison-ICE); non-int/non-enum
`match` subject → **E0323** (was miscompile — bool-match ran the wrong arm).
Corpus grep confirmed nothing that works today is now rejected. Pre-existing
out-of-scope bug flagged: capital `is Null` still has a bad lowering
(`(!NULL.has_value)`), fix needs hir_to_lir.c — deferred.

**DONE — rc balance** (agent `a428a38add7e0d9e4`, files `hir_to_lir.c`,
`emit_c.c` rc/closure arms, `iron_rc.c`, `iron_threads.c` + runtime.h shims):
- Convention: **BORROW for call args, OWNED (+1) for producer expressions**
  (fresh `rc T(...)`, call results, `downgrade()`/`upgrade()`, `weak rc null`);
  only aliases retain. Call-site arg retain deleted (was pure leak — surface
  can't even pass rc args yet, E0297/E0217).
- M1 nullable-rc: conditional `if (v.has_value)` retain/release emission;
  NULLABLE(rc) registered for scope drops. M3: capturing-closure `env_drop`
  registered + CALL arm passes `(&closure).env`. M4: mutable rc `var` release at
  scope exit (LOAD from slot) + release-old on reassignment; fixed pre-existing
  RC(RC(T)) alloca double-wrap. STMT_EXPR releases discarded owned rc results.
- `iron_rc.c`: debug leak detector under `IRON_DEBUG_ALLOCATOR` (atexit report
  of live rc blocks). M5 runtime side: `Iron_channel/mutex_destroy_with(elem_drop)`
  (old arities preserved as shims).
- **Honest results** (post-/tmp/sh-rename): 4.8/4.9 rc-policy **25/28** (was
  19/28); flips: `closure_captures_rc`, 4.9 `happy`, 4.10 `weak_upgrade_pair`.
  The 3 residual failures all need `rc` in type annotations (E0297) which the
  v4-fail corpus REQUIRES rejected — genuinely-future, re-mark in #9. Leak
  detector: 3 baseline leaks → 0; 11/11 buildable fixtures + probes clean;
  unit 206/206, no elision regressions.
- Handoffs: (1) M5 glue in `emit_helpers.c` (synthesize `elem_drop` + call
  `_destroy_with`; RWLock is glue-only); (2) stale XFAIL prose + phase-37
  markers on the now-passing rc fixtures; (3) rc-into-container-slot has no
  slot-release path (surface forbids it today; detector catches it).

**Wave-2 delegation (in flight).** Decision made on named-args: the project
already resolved this once — v4.0.0-alpha release notes document migrating 27
fixtures to "spec-legal positional construction" because `Type(field: value)`
is a deliberate compile error (language_definition.md:16/570). The ~12 remaining
named-arg fixtures were missed by that migration. So: **migrate fixtures, do NOT
implement named args.**
- **Agent A — fixture reconciliation** (owns tests/integration/v4/** fixtures
  only): migrate 3.7-arena (7), 8.6-composition-arena (3), 4.3-unchecked-ptr (3)
  + sweep 7.5-stdlib and the rest of v4/ for missed sites; reconcile the
  arena drop-skip policy against spec + the passing arena_allow_drop_skip
  fixture; honest markers for genuinely-missing surfaces (Set, element-copy).
- **Agent B — backend consolidation + Box** (owns build.c, emit_c.c,
  emit_helpers.c, emit_structs.c, typecheck.c, box.iron, runtime extend-only):
  (1) link iron_arena_rt.c properly + drop the generated-TU include workaround;
  (2) M5 elem-drop glue for Channel/Mutex/RWLock destroy (runtime side landed);
  (3) Box[T] semantics so the 4.3 fixtures' contracts work (is_null/unwrap/
  null-panic/takes_ownership), verified by positional probes + leak detector.
## HONEST BASELINE (serial full ctest, /build fully integrated, gate 37):
**28 fail / 414 (386 pass, 93.2%)** — every failure triaged and accounted for:
- **13 named-arg parse-blocked** (7× 3.7-arena, 3× 8.6-composition-arena,
  3× 4.3-unchecked-ptr) + **v4_fail_3.7-arena_arena_oom** (same cause, wrong
  error preempts the expected OOM outcome) → **agent A** migrating to positional.
- **5× 4.3-unchecked-ptr** overlap Box semantics (2 pure-Box rows:
  boundary_box_isnull, panic_box_unwrap_null) → **agent B**.
- **3 rc-policy rows** (boundary_field_holds_rc, boundary_atomic_refcount,
  upgrade_returns_nullable): E0297 `rc` in annotation — surface the v4-fail
  corpus REQUIRES rejected → **re-mark in #9**.
- **6 elision rows triaged — 5 are surface rejections, NOT optimizer bugs**
  (the old "13/13 elision pass" claim was vacuous under /tmp/sh):
  spawn_pair (E0297 + unsupported spawn-name form), concurrent_mutation
  (spawn-name form), stale_after_grow (`List` type-name not a surface),
  defer_panic + drop_on_assignment (E0273 `heap` in annotation) → **re-mark
  in #9**. The 6th, **polymorphic_dispatch, is a GENUINE BUG**: fat-ptr reaches
  FIELD_GET emission unwrapped (`_v12 = _v2.level` on `Iron_FatPtr` → clang
  error) → **handed to agent B** (same unwrap treatment as the arena agent's
  ALLOC/PUSH/CALL arms).
- **2× 7.5-stdlib** (list_copy_elements, set_basic) → agent A classifying
  parse-blocked vs genuine Wave-4 gap; expected → re-mark.
- **test_lir_print** → ✅ FIXED: snapshot diff reviewed = exactly the intended
  #13 return reshape (no-cleanup `ret` stays in-block; exit-label renumbering);
  regenerated via self-heal, **14/14 pass**, minimal diff (+2/−4).
- **benchmark_smoke (Timeout)** → re-measure serially in final verification
  (baseline overlapped the rc agent's tail activity; previously a single
  1.9×-vs-1.5× threshold miss on aarch64 = calibration).

**After wave-2 agents land:** integrate + final serial ctest → phase-gate
re-mark + bump (#9) → benchmark re-measure → completion summary.

### Wave-2 agent A (fixture migration) — DONE, and it found a second iceberg
- **3.7-arena 7/7 migrated, build + byte-match .expected** (incl. panic_after_reset
  via the real `stale pointer dereference` message). `allow_drop_skip: true`
  added only where honest (no .expected ever had drop lines; W0605/ARENA-09
  contract is bulk-free WITHOUT drop). `{field}` → `{self.field}` in drop blocks
  (bare field refs are E0200; matches the canonical fixture).
- **8.6-composition-arena**: syntax migrated; 2 fixtures build+run but .expected
  demands per-object drop at arena exit — a surface ARENA-09 deliberately
  excludes → honest `phase-38` XFAIL markers naming it. `happy.iron` is a
  doc-verbatim sample resting on non-surfaces (bare `!` not lexable, phantom
  "freed" output lines) → phase-38 marker.
- **7.5-stdlib classified: genuine Wave-4 gaps, NOT parse** (element-copy never
  invoked; `Set.new()` returns Void stub) → phase-38 markers.
- **4.3-unchecked-ptr**: E0104 gone; three precise Box defects documented and
  handed to agent B: (1) unwrap() inference ignores the `Box[T]` annotation
  (E0202), (2) unwrap result lowered as value → `.` on pointer type in C,
  (3) Box scope-exit never runs inner drop.
- **v4-fail arena_oom**: migrated; now fails HONESTLY on a real runtime gap —
  OOM panic omits the arena surface name (`arena: arena` vs expected
  `"scratch"`) → name-plumbing task handed to agent B (Task 5). Also flagged:
  `run_tests.sh` v4-fail handler is check-only, so runtime-panic negatives can
  never pass through it — only their dedicated ctest rows exercise them.
- **⚠️ SWEEP: 45 MORE named-arg fixtures** across 3.1/3.2/3.3/3.4/3.6/4.2/4.4/
  5/6/8.x/10-tooling (+5 v4-fail negatives whose named args preempt their
  intended diagnostics). Hidden from the honest baseline because the
  `test_v4_acceptance` aggregate runs `run_tests.sh` at its **env-default gate
  15** (`IRON_CURRENT_PHASE:-15`) while per-fixture ctest rows use the
  configured gate — verified by probe (3.1-stack + 3.3-rc happy = E0104 at
  /build). **Wave-3 delegated to agent A** (migration of all 50).
  **#9 addition:** wire the configured gate into the aggregate row's
  ENVIRONMENT so it stops silently running at 15.

### #9 groundwork done inline (coordinator, while wave-3 runs)
- **Aggregate gate wiring**: `test_v4_acceptance`, `test_v4_acceptance_fail`,
  `test_v4_migrated_from_v3` rows now set
  `ENVIRONMENT "IRON_CURRENT_PHASE=${IRON_CURRENT_PHASE}"` — the harness no
  longer silently runs at its env-default 15 while per-fixture rows use the
  configured gate. (v3-archive row left alone: archaeology-only, opt-in.)
- **8 surface-rejection fixtures re-marked phase-37 → phase-38** (marker + XFAIL
  prose; each already named its missing surface): 4.8 boundary_field_holds_rc,
  boundary_atomic_refcount; 4.9 upgrade_returns_nullable; 4.10 spawn_pair;
  4.11 concurrent_mutation, stale_after_grow, defer_panic, drop_on_assignment.
- **Stale XFAIL prose removed** from the three fixtures the rc work fixed
  (4.9 happy, 4.8 closure_captures_rc, 4.10 weak_upgrade_pair) — markers stay
  phase-37 (they now genuinely pass at gate 37). Diffs reviewed, prose-only.
- `test_lir_print` snapshot regenerated (reviewed = intended #13 reshape).

### Wave-3 (agent A, the 45-fixture sweep + 5 v4-fail negatives) — DONE
**Score: 13/45 positives now PASS outright** (12 byte-MATCH + 8.3
extends_drop_ordering after an explained .expected reorder to the implemented
scope-exit drop rule); **28 carry precise phase-38 markers** naming their
missing surface (rc/heap-in-annotation, explicit-self signatures, top-level
readonly (E0245 — whole 6-readonly dir authored against a defunct design),
prefix `!`, List[T]/Map.new/Set.new, spawn-handle form, `leak` keyword,
debug-allocator tooling, E0264 var-field-no-init, T? optional-payload
lowering); `demote()`→`downgrade()` applied corpus-wide.
**4 left honestly red on NEW real bugs:**
1. heap value into plain `T` field: typecheck ACCEPTS, emits Iron_FatPtr into
   int64_t field — invalid C (3.6 rc_holds_heap, stack_holds_heap).
2. `&container.field` field-pointer addressing emits an undeclared `_v6` temp —
   invalid C (4.2 boundary_field_pointer).
3. **user `init` bodies are silently skipped by `T(args)`/`heap T(args)`
   construction** — fields land, side effects never run (8.2
   extends_subsystem_lifecycle; 3.2-heap/happy passes only because its init is
   side-effect-free). Significant correctness bug.
4. (T? optional-payload lowering — classified missing-surface, marked, per the
   4.9 precedent.)
v4-fail negatives: copy_duplicate/drop_duplicate/drop_early_return now hit
their exact intended E0285/E0284/E0288; arena_nontrivial_dtor emits W0605
verbatim (needed the local FileHandle object removed — it E0201-clashed with
the Phase-33 stdlib FileHandle and preempted the warning forever).
**Docs/impl divergence adjudicated:** compiler + corpus say `downgrade()`;
migration guide said `demote()` → guide fixed (4 sites).

### Coordinator inline (post-wave-3)
- `heap_in_type.expected` → `E0273` (stale draft prose → actual code).
- `arena_nontrivial_dtor.expected` → verbatim `warning[W0605]: ...` line.
- **run_tests.sh v4-fail handler extended** (intent-preserving): warning-only
  negatives (`.expected` pins a W-code/warning[) accept exit-0 + substring;
  `@expect-panic` negatives are built+run and must abort with the .expected
  substring (command-substitution invocation to suppress bash signal noise).
  Verified: heap_in_type PASS, nontrivial_dtor PASS, arena_oom clean FAIL
  pending the name-plumbing fix (agent B Task 5).
- v4-fail category at gate 37 after fixes: 53 PASS / 20 FAIL / 73.
- **Agent C launched — v4-fail reconciliation** (owns tests/integration/v4-fail/
  only): adjudicate the 19 remaining substring-stale rows (rule 1 re-pin to
  actual diagnostic; rule 2 fix fixture to reach intended check; rule 3 genuine
  unsound-accept → leave red + report). arena_oom excluded (agent B).

### Diagnostics-quality bugs recorded by agent A (not blocking, for the report)
Error headers under stdlib-prepend point at `rawptr.iron:1:1` with
concatenated-TU line numbers; runtime `deref site` records line 1; E0200 help
suggestions are noise ("Set"/"Map"). Deserves its own follow-up task.

### Wave-2 agent B (backend consolidation + Box) — DONE
- **Task 1 arena link**: iron_arena_rt.c properly in build.c's runtime link
  list (both platforms, all clang call sites); generated-TU `#include .c`
  workaround removed from emit_c.c. Byte-diff of 4 programs: only the include
  line. All 11 runnable 3.7-arena fixtures verified end-to-end after.
- **Task 2 M5 elem-drop glue**: emit_helpers.c synthesizes per-type
  `elem_drop` + calls `Iron_{channel,mutex}_destroy_with`; RWLock glue-only
  destroy runs the element drop before free. Probes: Channel/Mutex/RWLock with
  droppable elements each drop exactly once (recv keeps move-out semantics);
  leak detector silent; non-droppable elements byte-identical C.
- **Task 3 Box[T]**: 3 of 5 behaviors fixed — `Box[T]` annotation now carries
  the elem through resolve_type_annotation; `Box.null()` elem backfill from the
  binding annotation; statement GET/SET_FIELD mirror the `->` rule for
  `*unchecked T`; Box `_free` runs the element dtor before heap free.
  4.3 happy / boundary_box_unwrap / boundary_box_takes_ownership now MATCH
  .expected end-to-end. Remaining 2 (boundary_box_isnull, panic_box_unwrap_null)
  are PARSER-blocked: `null`/`free` keyword tokens not accepted as method names
  at the expression `.`-postfix site (parser.c:375/1986) → wave-4 parser agent.
- **Task 4 fat-ptr FIELD access**: checked-ptr-typed values with no revealing
  producer (params/loads) now unwrap `((T*)v.addr)->f` in inline + statement
  GET_FIELD and SET_FIELD; polymorphic_dispatch passes end-to-end. Param
  generation-check remains an open design note (documented in code).
- **Task 5 arena OOM name: NOT done** — name is erased before LIR (STMT_LET
  direct-value binding; no name field on LIR CALL) and the panic format lives
  in iron_panic.c; precise 3-file fix design recorded → wave-4 backend agent.
- No-regression: unit 206/206; fixture-group failures 10 → 3 (all three
  expected: the 2 parser-blocked Box rows + arena_oom); byte-diffs clean.
- New pre-existing findings: **panic_after_reset runs to completion instead of
  panicking** (gen check uses the heap checker on an arena pointer;
  contradicts agent A's earlier abort observation — bisect in wave-4);
  4.3 boundary_unchecked_ffi fails on `[U8; 14]` + `as *unchecked U8` surface;
  4.11 getter_chain/tight_loop leak 1 heap alloc each (debug-only).
- Coordinator inline: removed nested_in_arena's literal
  `@expect-panic: none (positive run fixture)` header line (the extended
  harness would have parsed it as a real panic expectation).

### Wave-4 (in flight)
- **Parser agent — DONE** (parser.c, two hunks): new
  `iron_check_method_name_expr` predicate (= name_or_block_kw + NULL_KW/FREE)
  used ONLY at the single expression `.`-postfix name slot (~2011); decl-side
  predicates and all other sites untouched; free-statements dispatch at
  statement head before the Pratt loop so cannot be shadowed; bare `null`
  primary unchanged. Verified: boundary_box_isnull prints `null` (matches
  .expected), panic_box_unwrap_null aborts 134 with the pinned substring;
  unit 206/206; target ctest selection 17/17 (was 2 failures); heap/free/defer
  free/`!= null` probes unchanged. **Newly-reachable defect flagged:** explicit
  `b.free()` + scope exit double-frees (Box drop glue unaware of the explicit
  free) — probe-only, no fixture pins it → handed to wave-4 backend agent as
  stretch item 7.
- **Backend agent (wave-4) — DONE** (hir_lower.c, hir_to_lir.c, emit_c.c,
  lir_optimize.c, lir.h, iron_panic.c; unit 206/206; selection 63/63 from a
  60/63 baseline; full serial in its dir 411/413; zero regressions,
  byte-diff clean):
  1. **init bodies now run on construction** — both AST shapes routed through
     the same static-call machinery named inits use (Phase-85 anonymous-init
     bypass closed); 8.2 extends_subsystem_lifecycle byte-matches.
  2. **heap-into-plain-field**: fat-ptr adapted to a deref-copy for value
     slots (pointer-typed fields verbatim); both 3.6 fixtures build+run with
     correct values. Full byte-match was blocked by stale .expected ordering
     (contradicted locked DEFER-04/DROP-02) → coordinator adjudicated below.
  3. **`&obj.field`**: ADDR_OF targets excluded from inline-eligibility +
     in-place address emission with generation keyed to the parent allocation;
     4.2 boundary_field_pointer byte-matches.
  4. **arena OOM name**: LIR call gains `arena_binding_name`, STMT_LET tags
     arena ctors, emitted C stamps the handle name, panic prints
     `iron: arena "scratch" out of memory` → v4-fail arena_oom PASSES.
  5. **stale-arena checker was wholesale broken**: IRON_LIR_GEN_ARENA existed
     but nothing ever set it — arena derefs used the HEAP checker, which
     misreads the arena header and panicked on EVERY deref (valid ones too).
     Now tagged via def-chain root walk; valid derefs pass, post-reset deref
     panics `stale pointer dereference (arena)`; heap stale panics unaffected.
  6. 4.11 getter_chain/tight_loop leaks = fixture-authoring gaps (heap roots
     never freed; auto-free deliberately never built per locked PHASE-31) —
     no compiler change; coordinator fixed fixtures below.
  7. **Box explicit-free double-free fixed**: receiver-form Box calls now pass
     the promoted drop alloca itself, so explicit free nulls the slot scope
     exit reads; guarded no-op after; leak detector silent.

### Coordinator adjudications (post-wave-4)
- **3.6 rc_holds_heap**: .expected re-pinned to the verified DEFER-04/DROP-02
  ordering (defer-freed heap original drops first; field copy cascades after
  the holder body) + header prose rewritten. Now expected to PASS.
- **3.6 stack_holds_heap**: .expected pinned to the SPEC-correct output incl.
  the field-cascade line, and marked phase-38: a stack container with a drop
  body does not run the DROP-02 field cascade (emitted for rc, not stack) —
  honest gap, not pinned in.
- **4.11 getter_chain/tight_loop**: `defer free` added (behavioral fixtures;
  elision counts pinned in the unit oracle, outputs unchanged) → leak-clean.
- diagnostics.h:773 prose updated to the new arena-stale headline.
- **Gate bumped: committed IRON_CURRENT_PHASE 15 → 37** (CMakeLists.txt) with
  a pointer to this plan. /build reconfigured + rebuilt.

### FINAL SERIAL SUITE AT COMMITTED GATE 37: **412/414**
Every per-fixture ctest row passes. Remaining 2: `benchmark_smoke` (Timeout —
being re-measured) and the `test_v4_acceptance` aggregate, which at gate 37
exposes a THIRD hidden layer: **19 aggregate-only fixtures** (no dedicated
ctest rows, invisible at the old gate 15): 4.12-debug-leak W0606 family (5),
4.2 var/element-pointer + closure-capture (4), 4.3 boundary_unchecked_ffi,
4.4-readonly trio, 4.4 unchecked_independent, 4.5 list_of_bvec (clang exited 1
— likely genuine bug), 5-mutability pair, 7.5 map_hashable_ok (Map stub),
4.13 defer_block (output mismatch). Aggregate: PASS=305 FAIL=19 XFAIL=46 / 370.

### Wave-5 (aggregate-only fixtures) — DONE: aggregate 305→**314 PASS / 4 FAIL / 52 XFAIL**
Rule 1 mechanical fixes ×9 (incl. 4.12-debug-leak family — their W-code
contracts live in dedicated `ironc check|grep` ctest rows, all passing; the
aggregate handler can only see runtime output, now documented in headers).
Rule 2 phase-38 markers ×6 (`:=` store-through-pointer non-surface, scalar
checked-ptr deref-read non-surface with deliberate `<ptr>` interpolation,
`as` cast not a token, fixed-size array returns E0215/E0305, Map.new stub).
Rule 3 genuine bugs left red (4 rows / 3 bugs, minimal repros in
/sbx/probes-w5): (1) two-hop capture-closure return degrades to
`void(*)(void)` → invalid C; (2) 2nd call-site use of a bounded-vector
binding splats the dynamic-List ABI → invalid C; (3) `var` Int param reads
interpolate empty + fixture pins the defunct pass-by-value design vs
language_definition.md:587-590 mutable-reference contract.
### Wave-6 (final backend pass) — DONE: aggregate **FAIL=0**
1. **Two-hop closure**: `needs_env_arg` was gated on the callee's producer
   instruction kind — CALL results fell to a dead cast fallback. Every lifted
   lambda already takes `void *_env` first (uniform convention), so dispatch
   is now unconditional for FUNC-typed callees. Follow-on: checked-ptr capture
   loads inside lifted lambdas now get their STACK-source GENCHECK (fixtures
   correctly panic `dangling stack pointer to frame`).
2. **Bounded-vector splat**: `analyze_array_param_modes` treated `[T; <=N]`
   like a dynamic array (CONST_PTR mode → `.items/.count` splat); bvec (and
   var) params now excluded — pass by value. First call had only looked right
   because the inliner absorbed it.
3. **var-param mutable-reference ABI implemented per spec** (the fixture's
   pass-by-value pin was the defunct design): `param_is_var` arrays on
   HIR/LIR funcs; callee copy-in alloca + IDENT reads via LOAD + write-back
   before every RETURN (after defers) + SET_FIELD/INDEX in place; caller
   passes `&alloca` (new `args_by_addr`); optimizer guards (no store-fwd
   through copy-in, inliner skips var-param callees). Also fixed the read
   bug (params interpolated empty — INTERP arm used a raw value_table lookup).
   boundary_param_var re-pinned to spec (`inside: 11 / outside: 11`).
   Regression caught+fixed in verification: test_composite game loop
   (SET_FIELD landed on a dead copy → infinite loop) — green now.
Verification: unit 206/206; selection 25/25; **aggregate PASS=318 FAIL=0
XFAIL=52 / 370**; v4-fail 71/0/2; full serial ctest in /build-w6: only
benchmark_smoke (Timeout); byte-diffs identical ×3; leak detector silent.
New flags: (a) pre-existing `--no-optimize` hang on closure programs (both
pre/post compilers — inliner-shaped empty blocks + __builtin_unreachable at
-O0); (b) var-field-source var-param call sites fall back to by-value (no
fixture); (c) heap-rooted checked ptr captured into a closure would use the
STACK checker (no fixture).

### Benchmark reality check (contended run — serial re-measure pending)
Full corpus 21m45s wall (ctest row TIMEOUT=300 → why it "Timeouts");
119/139 pass, 17 threshold misses concentrated where the new SAFETY checks
landed: sqrt_integer 3.3× (div guards), word_break 5.4× / unique_paths 5.0×
(unconditional emit-level bounds checks in hot list loops), etc. This is the
cost of memory safety, not calibration noise: the emit-level bounds checks
BYPASS the LIR check-elision framework (they're emitted after optimization).
Product decision for the user: accept new thresholds / wire bounds checks
into the elision tiers / add release-mode unchecked indexing. Do NOT silently
inflate thresholds.
**Serial confirmation (quiet container, 2026-07-18): 120/139 pass, 16
threshold misses + 2 errors — numbers match the contended run (word_break
5.4× both times), so the misses are stable safety-check costs, not noise.**
- **Agent C (v4-fail reconciliation) — DONE**: 19 rows adjudicated.
  Rule 1 (re-pin to actual diagnostic, mostly bare codes): 9 rows — incl.
  weak_rc_in_arena whose old pin failed only on `E0301:` vs `E0301]:`
  bracketing. Rule 2 (fixture unblocked to reach its contract, then re-pinned):
  8 rows — E0296/E0297/E0289/E0239/E0238/E0278/E0277 + double_free_warning now
  a proper warning-only negative pinning verbatim W0607. Rule 3 (deliberate
  surface changes, parked phase-38 with evidence): defer_non_free_form
  (general defer shipped Phase 32 per DEFER-SEMANTICS.md; positive canonical
  test exists) and readonly_returns_string (String whitelisted in
  is_readonly_compatible_type; stdlib relies on it). **Zero unsound-accepts.**
  Category at gate 37: **PASS=70 FAIL=1 (arena_oom, wave-4) XFAIL=2 / 73**;
  default gate: 70/0/3. All 4 dedicated ctest row greps verified passing.

Remaining detail for the not-yet-done items:
- **rc balance:** pick one call convention; wire nullable-rc drops (fixes
  `weak.upgrade()` leak); call the synthesized closure `env_drop`; release
  mutable rc vars + old value on reassignment; element-drop hooks for
  Channel/Mutex/RWLock. Add an rc-balance check to `iron_lir_verify` to lock it.
- **arena:** map surface `Arena` → `Iron_Arena_RT *` (mirror the Mutex arm at
  `emit_helpers.c:202-205`), emit per-program glue like `emit_ensure_mutex`, pass
  the handle unchanged in ARENA_ALLOC/PUSH, rework the FREE arm.
- **globals:** real module-level `val`/`var` lowering (currently uncompilable
  across two functions — `hir_lower.c:1305-1327`).
- **is / string-match:** typecheck-reject (or implement) `x is Type`
  (`hir_to_lir.c:2414-2420`, poison today) and string-subject `match`
  (`hir_to_lir.c:3082-3085`, invalid C today).
- **frontend crash five-pack:** import-path stack overflow (`parser.c:3180`);
  self-referential by-value object infinite recursion (`typecheck.c:6791`);
  tuple-destructure NULL-name derefs (`parser.c:2784` + capture/concurrency/
  escape); `snprintf`-accumulation overflow (`types.c:457-538`); decl-less object
  NULL derefs (`typecheck.c:3870/4326/5994`).
- **unsound acceptance family:** comparisons, unknown methods (emit
  `IRON_ERR_NO_SUCH_METHOD`), generic-enum payloads, top-level-annotation
  mismatch, narrowing invalidation across functions/reassignment.
- **misc:** value-table growth asserts (pins the arm64 hang); octal literals
  (`hir_lower.c:1211`); lowering shadow scopes (`hir_lower.c:752-798`);
  compound-assign single-eval (`hir_lower.c:726-739`).
Large; **decompose into per-area subagents**, each verified in the container.

### ✅ #17 — docs (delegated, complete)
16 files corrected to v4 (INSTALL, homepage `__IRON_VERSION__` placeholder,
CHANGELOG v4 entry, three editor READMEs, both migration guides, site reference
v3 banner + `impl`, LSP page, versioning table, `game.iron` rewrite,
help_registry, install.sh, compiler_architecture C17). See the agent summary in
session history.

---

## Working-tree changes so far (uncommitted)

Compiler/runtime: `src/cli/build.c`, `src/cli/check.c`, `src/cli/help_registry.c`,
`src/lir/emit_c.c`, `src/hir/hir_to_lir.c`, `src/runtime/iron_panic.c`,
`src/runtime/iron_panic.h`, `src/runtime/iron_runtime.h`.
Tests/CI: `CMakeLists.txt`, `.github/workflows/ci.yml`, `tests/run_tests.sh`,
`tests/integration/run_integration.sh`, `tests/benchmarks/run_benchmarks.sh`,
`tests/compile_fail/CMakeLists.txt` (new) + 7 `.expected` files.
Docs: the 16 files from #17.
New: `docs/dev/COMPILER-AUDIT-2026-07.md`, this file.

Nothing committed yet (per the no-commit-without-asking rule).

---

## Suggested execution order from here

1. Fold in the HIR agent's report for #13/#14; verify `p3_retdefer` and
   `p7b_scrut` in the container; run full ctest and confirm no regression.
2. #15 optimizer soundness (delegate; soundness-trap probes) — P0-severity.
3. #16 P1 batch (delegate per area).
4. Re-run at `-DIRON_CURRENT_PHASE=37`; when the 33 clear, bump the committed
   gate and re-mark any truly-future fixtures (#9).
5. Only then consider committing, in logically-grouped commits.
