# Phase 58: Benchmark Stabilization - Context

**Gathered:** 2026-04-10
**Status:** Ready for planning

<domain>
## Phase Boundary

Root-cause the `binary_tree_diameter` performance gap reported in REQUIREMENTS.md BENCH-01 ("Iron 1.9-2.0x slower than C"), and stabilize the benchmark measurement infrastructure so future reports are reproducible (variance <5% across 5 consecutive local runs).

**Phase 58's scope is dominated by one insight:** the alleged gap and the stored baseline contradict each other. `tests/benchmarks/baselines/latest.json` and `tests/benchmarks/results/post-optimization.json` (both commit `61f60f4`, 2026-04-01) report `binary_tree_diameter` at ratio **0.9** (Iron 14-15 ms vs C 15.3-16.3 ms — Iron FASTER). The 1.9-2.0x figure recorded in REQUIREMENTS.md came from CI, and may be pure flakiness.

**The phase must NOT assume a real gap exists until the measurement is trustworthy.** Stabilize first, evaluate second. If after stabilization the ratio is already <1.5x, the "fix" is just lowering thresholds and documenting the original CI figure as quantization noise. If a real gap remains after stabilization, then do the generated-C-vs-solution.c diff and decide fixable-vs-inherent.

**Papered-over state being removed:** Phase 54 Plan 02 raised `max_ratio` from 1.5x to 2.5x across 88 benchmark config.json files in a blanket chore commit (`0e82c71`) "to tolerate CI runner variance." PROJECT.md:31 calls this out explicitly as a target for Phase 58 to root-cause and un-paper. Phase 58 is that work.

**Out of scope (belongs to other phases):**
- New benchmark problems — audit and threshold-tune existing 88, don't add new ones.
- Multi-run averaging inside `run_benchmarks.sh` — local 5-run stability verification is sufficient (see decisions).
- Wall-clock `Time.now()` or `Time.now_us()` additions — only `Time.now_ns()` is in scope.
- CI infrastructure changes (runners, containers, CPU governors) — Phase 58 fixes the Iron-side measurement, not the CI environment.
- `docs/benchmarking.md` canonical methodology doc — a narrative section in `58-VERIFICATION.md` is sufficient.
- Profiling-tool root-cause investigation — only use `perf`/Instruments if the generated C diff is inconclusive. Default to the diff method.

</domain>

<decisions>
## Implementation Decisions

### Scope philosophy: stabilize first, evaluate second
- **Order is locked:** (1) add `Time.now_ns()`, (2) audit 88 problems by running 5x each locally and measuring variance, (3) THEN decide whether `binary_tree_diameter` has a real gap and whether a root-cause investigation (generated C diff) is warranted.
- **Do not assume the 1.9-2.0x figure is real** until step 2 confirms it with a stabilized measurement. It may simply dissolve when quantization noise is removed.
- **Do not attempt codegen changes** (LIR/emitter work) in Phase 58 unless step 3 actively proves a specific, fixable structural delta. Codegen speculation is out of scope.

### `Time.now_ns()` addition (LOCKED)

**Iron API:**
- Add `func Time.now_ns() -> Int {}` as a sibling stub in `src/stdlib/time.iron`, placed immediately after the existing `Time.now_ms()` declaration on line 5.
- Existing `Time.now_ms()` stays unchanged. The two functions coexist.

**Runtime (C) implementation:**
- Add `int64_t Iron_time_now_ns(void);` declaration in `src/stdlib/iron_time.h` immediately after the existing `Iron_time_now_ms` declaration on line 10.
- Add the implementation in `src/stdlib/iron_time.c` immediately after `Iron_time_now_ms` at line 18, matching the existing style:
  ```c
  int64_t Iron_time_now_ns(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
  }
  ```
- **Clock source:** `CLOCK_MONOTONIC` — identical to the existing `Iron_time_now_ms` implementation. This gives us monotonic, NTP-immune timing on Linux. macOS's `clock_gettime(CLOCK_MONOTONIC, ...)` is supported natively in modern SDKs (Apple added it in macOS 10.12 Sierra). No `mach_absolute_time` fallback required.
- **Return type:** `int64_t` (Iron's `Int`). `tv_sec * 1e9 + tv_nsec` comfortably fits in int64 since `CLOCK_MONOTONIC`'s reference point is system boot, so `tv_sec` is seconds-since-boot (modest values, not Unix epoch).

**Behavior contract (the regression test enforces these):**
- Monotonic: two successive calls return values `b >= a`.
- Nonzero delta across a small workload: a loop of work between two calls produces a delta `> 0`.
- Nanosecond granularity: the delta for a no-op loop of N iterations must be within an order of magnitude consistent with nanoseconds on any reasonable CPU (lower bound N*1, upper bound N*10000 — catches accidental unit regressions such as returning microseconds or milliseconds by mistake).

**Scope boundary:** extend the existing Time module minimally. Do NOT refactor the Time module or rename anything. Single-file additions to `time.iron`, `iron_time.h`, `iron_time.c`. No other files touched by the ns addition itself.

### `Time.now_ns()` regression test

- Location: `tests/integration/time_now_ns.iron` + `.expected` (follow project convention from other `time_*` tests if any exist, otherwise use this path).
- Three assertions enforced by print-and-match OR generated-C grep:
  1. **Monotonic:** call `Time.now_ns()` twice back-to-back, verify second >= first, print `monotonic ok` if so.
  2. **Nonzero delta:** run a short busy-loop (e.g., 10,000 iterations of trivial work) between two calls, verify `delta > 0`, print `nonzero ok` if so.
  3. **Ns granularity:** take the delta across a 10,000-iteration no-op loop, verify `delta > 10000 && delta < 10000 * 10000` (i.e., between 1 ns/iter and 10 µs/iter — catches accidentally returning ms or s units). Print `ns_granularity ok` if so.
- `.expected` output combines all three:
  ```
  monotonic ok
  nonzero ok
  ns_granularity ok
  ```
- The test uses integer comparisons only — no floating-point, no timing sensitivity beyond the order-of-magnitude bounds. Should not flake on slow CI runners.

### Benchmark measurement stabilization

- **Iteration counts stay as-is across all 88 problems.** With `Time.now_ns()` available, the ms-integer quantization that caused `iron_ms=0` and 1-ms-rounding artifacts simply doesn't apply anymore. For `binary_tree_diameter` specifically, 500K iterations → ~14 million ns → <0.001% quantization. No per-problem iteration bumps.
- **`binary_tree_diameter/main.iron` rewrite:** swap `val start = Time.now_ms()` and `val elapsed = Time.now_ms() - start` to use `Time.now_ns()`. Print the elapsed time — planner decides whether to keep printing ms (via division) to preserve `run_benchmarks.sh`'s `extract_time_ms` regex, or update the runner to parse ns/ms pairs. **Runner compatibility is a first-class concern** — see the next decision.
- **`run_benchmarks.sh` compatibility:** `tests/benchmarks/run_benchmarks.sh:170` extracts `Total time: <number> ms` via grep. Two options for the planner to pick:
  1. **Preferred:** Iron benchmarks keep printing `Total time: X ms` as the primary line (compute from ns by dividing by 1,000,000 with 3 decimal places), matching C's `%.3f ms` format. This is backwards-compatible — no runner changes needed.
  2. **Alternative:** extend the runner's regex to also accept `Total time: <number> ns` and normalize internally. Only use if (1) proves hard to pipe through Iron's string formatting.
- **Apply the rewrite to all 88 Iron benchmark sources**, not just `binary_tree_diameter`. The quantization problem affects every benchmark where the result rounds to an integer ms. This is a mechanical change: `Time.now_ms()` → `Time.now_ns()` + `/ 1000000` at print time. Planner creates one task per logical group of benchmarks OR one task for all 88 if the mechanical transformation is uniform.

### Per-problem max_ratio audit (all 88 benchmarks)

- **Every single `tests/benchmarks/problems/*/config.json` file gets audited and re-justified.** This is the "no-excuses defensible baseline" path. No blanket values.
- **Audit procedure:**
  1. After `Time.now_ns()` + Iron benchmark rewrite lands, run the full benchmark suite locally 5 times (`bash tests/benchmarks/run_benchmarks.sh` with fresh builds).
  2. For each of the 88 problems, compute: min/max/mean ratio across 5 runs, variance (stddev / mean as percentage).
  3. Pick `max_ratio` as `max(1.5, round_up_to_1_decimal(mean_ratio * 1.15))` — 15% headroom over the observed mean, floor at 1.5x.
  4. Special cases (e.g., benchmarks where Iron is genuinely faster and the ratio is already <1.0) still get 1.5x as the floor — don't set thresholds below 1.5x because future regressions should trip them.
  5. Problems showing variance >5% across the 5 runs get a higher `max_ratio` (enough to cover the observed variance + 15% headroom) AND a `rationale` field explaining the variance source.
- **`rationale` field format (NEW JSON field in each `config.json`):**
  ```json
  {"iterations": 500000, "timeout_sec": 60, "max_ratio": 1.5, "rationale": "Stable ratio 0.9 across 5 runs after ns timing; 1.5x floor per project policy (2026-04-10 audit)"}
  ```
  - String field, single line, includes the audit date. Every config gets one — no exceptions.
  - Must be human-readable and cite the evidence briefly.
  - If a problem still legitimately needs >1.5x after stabilization, the rationale must state WHY specifically (e.g., "Sub-ms C runtime (0.7 ms) amplifies any measurement noise; 2.0x provides 1-ms absolute headroom").
- **VERIFICATION.md audit table (MANDATORY):** `.planning/phases/58-benchmark-stabilization/58-VERIFICATION.md` includes a markdown table listing all 88 problems with columns: `{problem_name, iron_ms_mean, c_ms_mean, ratio_mean, variance_pct, new_max_ratio, rationale_abbrev}`. This is the review artifact — the durable record lives in the `rationale` field of each config.json, the phase doc provides the aggregated view.
- **Rationale field and VERIFICATION.md table must match.** Any divergence between the two is a plan bug.

### Stored baseline update
- After the audit lands and all configs have new `max_ratio` values, regenerate `tests/benchmarks/baselines/latest.json` from the final stabilized run. This is the regression guard — future CI runs compare against this.
- Commit the updated baseline in the same plan as the audit (one atomic change: new configs + new baseline).
- Do NOT touch `tests/benchmarks/baselines/v0.0.6-alpha.json` — that's a versioned historical baseline, must stay frozen.

### Root-cause investigation (conditional)
- **Only run if** the stabilized `binary_tree_diameter` ratio is still >=1.5x after the measurement fix. If the ratio is already <1.5x, skip this step entirely and document in VERIFICATION.md that "the 1.9-2.0x figure dissolved with ns timing; root cause was ms-quantization noise in the measurement."
- **If investigation is needed, use the generated-C diff method:**
  1. Compile `tests/benchmarks/problems/binary_tree_diameter/main.iron` through the Iron compiler.
  2. Locate the generated C output (in the Iron build cache — typically `.iron-build/*.c`).
  3. Diff the generated `diameter()` function against the hand-written `tests/benchmarks/problems/binary_tree_diameter/solution.c` `diameter()` function.
  4. Document structural differences in a before/after table: Iron-generated code on one side, hand-written C on the other, with observations about bounds checks, array accesses, arena allocations, function call overhead, inlining decisions.
  5. For each delta, classify as: (a) fixable in Iron's codegen without phase scope creep, (b) inherent to Iron's semantics (safety checks, memory model), or (c) minor enough to ignore.
- **Profiling escalation:** only use `perf` (Linux) or Instruments (macOS) if the generated C diff is inconclusive. Default to diff-only.
- **Fixable deltas are out of scope for Phase 58.** Phase 58 documents the findings and raises a follow-up requirement. Actual codegen fixes belong in a future phase. Phase 58's deliverable is the audit + ns timing + documented root cause, NOT a codegen change.

### Verification: 5-run local stability
- **Final check before closing Phase 58:** run the full benchmark suite 5 times consecutively on the developer's local machine (not CI), capture variance per problem, verify `binary_tree_diameter` specifically shows <5% variance across the 5 runs.
- **No CI reproduction required.** Local stability is the verification bar per ROADMAP SC3.
- **If local stability is achieved but CI still flakes** in the future, that's a CI environment issue to be handled separately — Phase 58 has fixed the Iron-side measurement, which is all it promised.

### Documentation artifacts
- **58-VERIFICATION.md narrative section:** 1-2 paragraphs explaining: (a) the original 1.9-2.0x figure came from ms-integer quantization noise in the benchmark measurement, (b) adding `Time.now_ns()` removed the noise, (c) the stabilized ratio is X, (d) thresholds are now individually justified across all 88 problems. This is the "root cause documented" deliverable for SC1.
- **No new canonical doc in `docs/`.** The VERIFICATION narrative is sufficient. `docs/benchmarking.md` is explicitly deferred.
- **REQUIREMENTS.md cleanup:** after the phase closes, update BENCH-01's text to reflect actual findings (e.g., "The 1.9-2.0x CI figure was ms-integer quantization noise; stabilized ratio is X after ns timing."). Also mark BENCH-01 and BENCH-02 as complete in REQUIREMENTS.md via the standard `requirements mark-complete` CLI.

### Claude's Discretion
- Exact iteration count for the `Time.now_ns()` regression test's no-op loop (decisions say 10,000; planner can adjust if Iron's loop codegen makes that unreliable).
- Exact format of the rationale strings beyond the general shape above — wording per problem is planner's judgment during the audit.
- Whether to write the audit table by hand during execution or generate it from a short post-run script that parses the runner's JSON output. Planner picks based on what's easiest at the time.
- File/path for the audit-runner helper if one is needed (e.g., `scripts/audit_benchmarks.sh` or inline bash in the plan) — wherever fits existing conventions.
- How to express the 5-run stability verification: single bash loop, a helper script, or five manual invocations. Planner picks.
- Exact wording of the VERIFICATION narrative section — the content is locked, the prose is planner's.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Iron runtime Time module (ns timing addition site)
- `src/stdlib/time.iron` — Iron-side Time module stubs. Line 5 is the existing `Time.now_ms()` declaration; `Time.now_ns()` gets added immediately after.
- `src/stdlib/iron_time.h` — C header. Line 10 declares `Iron_time_now_ms`; add `Iron_time_now_ns` sibling immediately after.
- `src/stdlib/iron_time.c` — C implementation. Lines 14-18 implement `Iron_time_now_ms` using `CLOCK_MONOTONIC`; the `_ns` sibling follows the same pattern with `tv_sec * 1e9 + tv_nsec`.

### Benchmark infrastructure
- `tests/benchmarks/run_benchmarks.sh` — runner script. Line 170 contains the `Total time: <number> ms` regex that Iron benchmarks must keep matching (backwards-compat), or the regex must be extended.
- `tests/benchmarks/problems/binary_tree_diameter/main.iron` — the target benchmark. Currently uses `Time.now_ms()` at lines 61 and 66.
- `tests/benchmarks/problems/binary_tree_diameter/solution.c` — hand-written C reference. Uses `clock_gettime(CLOCK_MONOTONIC, ...)` for timing — Iron's `Time.now_ns()` matches this exactly.
- `tests/benchmarks/problems/binary_tree_diameter/config.json` — currently `{"iterations": 500000, "timeout_sec": 60, "max_ratio": 2.5}`. Phase 58 adds the `rationale` field and re-justifies `max_ratio`.
- `tests/benchmarks/problems/*/config.json` — 88 per-problem config files, all touched by the audit. All 88 get `rationale` fields.
- `tests/benchmarks/baselines/latest.json` — stored baseline for regression comparison. Updated after the audit.
- `tests/benchmarks/baselines/v0.0.6-alpha.json` — versioned historical baseline. **Do NOT modify.**
- `tests/benchmarks/results/post-optimization.json` — evidence that the stored local baseline shows ratio 0.9 for `binary_tree_diameter` (contradicting the REQUIREMENTS.md 1.9-2.0x claim).

### Phase and project context
- `.planning/REQUIREMENTS.md` lines 93-94 — BENCH-01 and BENCH-02 requirement statements. BENCH-01 specifically references the "1.9-2.0x slower" figure that Phase 58 must verify or refute.
- `.planning/PROJECT.md` line 31 — "binary_tree_diameter benchmark flakiness root-caused and stabilized (not just threshold-papered-over)" — the no-paper-over mandate for Phase 58.
- `.planning/ROADMAP.md` lines 1177-1187 — Phase 58 section, 4 success criteria.
- `.planning/phases/54-test-hardening/54-02-SUMMARY.md` — Phase 54 Plan 02, the origin of the 1.5x→2.5x blanket threshold raise (commit `0e82c71`). Line 68 states the rationale: "to tolerate CI runner variance." Phase 58 un-papers this.
- `.planning/phases/54-test-hardening/54-VERIFICATION.md` line 34 — verification of the 113 configs (Phase 54's count; current repo has 88, some benchmarks have been added/removed since).

### Existing ms implementation to mirror
- The `Iron_time_now_ms` function body in `src/stdlib/iron_time.c:14-18` is the template for the new `Iron_time_now_ns` function. Same clock source, same error handling (none), same declaration style, same file location.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`CLOCK_MONOTONIC` via `clock_gettime`** is already used in `src/stdlib/iron_time.c:16` for the ms implementation and also in `src/stdlib/iron_math.c:59`. The ns implementation reuses the exact same plumbing — just different arithmetic on the returned `timespec`. No new `#include` or platform detection needed.
- **Iron stdlib module pattern** — `src/stdlib/time.iron` defines stubs, `src/stdlib/iron_time.h` declares the C side, `src/stdlib/iron_time.c` implements. The naming convention is `Iron_<module>_<function>` (e.g., `Iron_time_now_ms`). New `Iron_time_now_ns` fits this exactly.
- **Benchmark runner's dual-format regex** at `tests/benchmarks/run_benchmarks.sh:170` already handles both integer (`14`) and decimal (`15.335`) "Total time: X ms" formats via `[0-9]+(\.[0-9]+)?`. Iron prints integer ms today; after the rewrite to ns/1e6 with `%.3f` formatting (if Iron supports it) or manual fraction formatting, the runner works unchanged — no extract_time_ms changes needed in the common case.

### Established Patterns
- **Sibling C stdlib function emission** — `Iron_time_now`, `Iron_time_now_ms`, `Iron_time_sleep`, `Iron_time_since` all live side-by-side in `iron_time.c`. Adding `Iron_time_now_ns` as a peer is the established convention.
- **Phase 54/57 atomic commit pattern** — per-task commits with clear `feat/fix/test/chore/docs` conventional-commit prefixes. Phase 58 follows the same pattern: `feat(58-N)` for the ns timing addition, `chore(58-N)` for the audit/config changes, `test(58-N)` for the regression test, `docs(58-N)` for the VERIFICATION narrative.
- **Phase 54/56/57 CONTEXT-to-PLAN flow** — concrete C snippets and exact file paths get copied verbatim from CONTEXT.md into plan `<action>` blocks. Phase 58's ns implementation snippet above is designed to be copy-pasted directly into the plan.

### Integration Points
- **`Time.now_ns()` Iron-side stub → HIR lowering** — `src/hir/hir_to_lir.c:1052` and `:1100` reference the existing `Time.now_ms()` HIR handling. Planner verifies whether the HIR path for `Time.now_ms()` picks up `Time.now_ns()` automatically (likely yes, since it's a sibling stub with the same signature shape), or whether any method-table / builtin registration needs a parallel entry. This is a research question for the planner to resolve via codebase inspection — not a user decision.
- **Benchmark runner → per-problem config.json** — the runner reads `max_ratio` at `tests/benchmarks/run_benchmarks.sh` (pattern appears at line 338 for the `pass` check). Adding a `rationale` field is read-transparent — JSON parsing ignores unknown keys, so the new field doesn't require runner changes. Verify with a single test run after the first config.json gets the field.
- **Baseline comparison → CI workflow** — `tests/benchmarks/baselines/latest.json` is the comparison target. No CI workflow changes in Phase 58; the baseline update is self-contained.

### What NOT to touch
- `src/stdlib/iron_time.c:1-9` — `Iron_time_now` (wall-clock). Out of scope. No changes to wall-clock time.
- `Iron_time_sleep`, `Iron_time_since`, `Iron_Timer` structure — out of scope. Phase 58 touches only the monotonic reading functions.
- `tests/benchmarks/baselines/v0.0.6-alpha.json` — versioned historical baseline, must stay frozen.
- Any benchmark problem's `main.iron` logic beyond swapping `Time.now_ms()` → `Time.now_ns()` + format adjustment. The computation inside each benchmark stays byte-identical.
- `run_benchmarks.sh` (unless the backwards-compat "print ms from ns" approach proves infeasible during planning — then a minimal regex extension is allowed, but still avoid restructuring the script).
- Phase 54's `0e82c71` commit or any git history. Phase 58 makes forward-only changes that supersede the 2.5x blanket raise through explicit per-problem justification.

</code_context>

<specifics>
## Specific Ideas

- **The exact ns implementation line**, ready to paste into `iron_time.c` after line 18:
  ```c
  int64_t Iron_time_now_ns(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
  }
  ```
- **The exact header line**, ready to paste into `iron_time.h` after line 10:
  ```c
  int64_t Iron_time_now_ns(void);      /* monotonic nanoseconds */
  ```
- **The exact Iron stub**, ready to paste into `time.iron` after line 5:
  ```
  func Time.now_ns() -> Int {}
  ```
- **The `binary_tree_diameter/main.iron` swap pattern:**
  ```
  -- BEFORE (lines 61-66):
  val start = Time.now_ms()
  var result = 0
  for it in range(iterations) {
      result = diameter(tree_vals, tree_size)
  }
  val elapsed = Time.now_ms() - start
  ...
  println("Total time: {elapsed} ms")

  -- AFTER:
  val start_ns = Time.now_ns()
  var result = 0
  for it in range(iterations) {
      result = diameter(tree_vals, tree_size)
  }
  val elapsed_ns = Time.now_ns() - start_ns
  val elapsed_ms = elapsed_ns / 1000000
  ...
  println("Total time: {elapsed_ms} ms")
  ```
  Backwards-compatible with `run_benchmarks.sh:170` regex. Trades ns precision for ms print format, but the stored value used for comparison stays at ns internally if the planner wants finer bookkeeping. The minimum viable change is just replacing the two `now_ms()` calls with `now_ns() / 1000000`.
- **Alternative print format if Iron supports float formatting** (planner verifies in research): print `"Total time: {elapsed_ns / 1000000.0} ms"` with a decimal. The runner's regex already accepts decimals.
- **Example `rationale` field values** for the audit:
  - Stable-fast case: `"rationale": "Stable ratio 0.9 across 5 runs after ns timing; 1.5x floor per project policy (2026-04-10 audit)"`
  - Variance-bounded case: `"rationale": "Sub-ms C runtime (0.7 ms mean) amplifies measurement noise; 2.0x covers observed variance of 8% + 15% headroom (2026-04-10 audit)"`
  - Inherent-overhead case: `"rationale": "Iron bounds-check overhead adds ~40% vs hand-written C on tight inner loop; 1.5x covers stable 1.4 ratio across 5 runs (2026-04-10 audit)"`
- **Expected audit-table row format in VERIFICATION.md:**
  ```markdown
  | Problem | iron_ms | c_ms | ratio | var % | max_ratio | rationale |
  |---------|--------:|-----:|------:|------:|----------:|-----------|
  | binary_tree_diameter | 14 | 15.3 | 0.92 | 1.2% | 1.5 | Stable, 1.5x floor |
  ```

</specifics>

<deferred>
## Deferred Ideas

- **CI-specific investigation** — The user said local 5-run stability is sufficient. If CI still shows flakiness after Phase 58, that's a separate CI environment issue (runner noise, CPU governor, containerization). Track as a future CI-hardening phase if it recurs.
- **Multi-run averaging inside `run_benchmarks.sh`** — the user picked "stored baselines update" over runner changes. If Phase 58's per-problem audit shows that statistical averaging would materially improve results, that's a future runner-hardening phase.
- **`docs/benchmarking.md` canonical methodology doc** — user picked the VERIFICATION-narrative-only option. If the project grows more benchmarks and needs durable authoring guidance, add this later.
- **`Time.now_us()` microsecond timing** — user said ms + ns, not us. If a future phase wants microsecond granularity for a different reason, add then. Phase 58 is ns-only.
- **Profiling-based root-cause** — only escalate to `perf`/Instruments if the generated C diff is inconclusive. Not expected to be needed. Future phases may adopt profiling as a standard technique if Iron develops a performance-work discipline.
- **Codegen fixes for real Iron/C gaps** — if Phase 58's generated-C diff reveals actionable differences, the *fixes* belong in a future codegen phase. Phase 58 documents, the next phase (if needed) fixes.
- **REQUIREMENTS.md BENCH-01 text rewrite** — after Phase 58 closes, the "1.9-2.0x slower" wording becomes stale. A cleanup edit is tracked in decisions but the rewrite itself happens at phase close, not as a separate deferred item.

</deferred>

---

*Phase: 58-benchmark-stabilization*
*Context gathered: 2026-04-10*
