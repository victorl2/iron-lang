---
phase: 66-structural-protections-linux-release-ci
plan: 04
subsystem: ci-infrastructure
tags: [github-actions, ci, release-build, integration-tests, bash, regression-template, reg-01, reg-04]

# Dependency graph
requires:
  - phase: 65-correctness-audit
    provides: find_first_last SIGSEGV root-cause analysis — the motivating incident the Linux Release CI job is designed to catch
provides:
  - build-and-test-release (ubuntu-latest) CI job in .github/workflows/ci.yml that builds ironc in Release mode and runs the full ctest + integration suite
  - Exit-code policy in tests/integration/run_integration.sh that distinguishes signal-level ironc exits (>= 128) from clean compile errors and surfaces crashes as [CRASH] instead of swallowing them as [FAIL] (build failed)
  - docs/regression-fixtures.md — contributor-facing 4-section template documentation citing hir_to_lir_elif_mono_walker.iron as the canonical example and listing the 6 Plan 03 PROT-04 fixtures as initial adopters
affects:
  - 66-03-plan (PROT-04 blind-cast rewrites — new fixtures must adopt the 4-section template)
  - Phase 67 REG-02 (crash-canary fixtures will cite docs/regression-fixtures.md)
  - Every future AUDIT-derived regression test

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "GitHub Actions: separate top-level job for a new build type instead of adding a matrix dimension (keeps the Debug matrix simple, lets Release evolve independently as a parallel required status check)"
    - "run_integration.sh crash-exit protocol: temporarily disable set -e around ironc, capture $? explicitly, branch on >= 128 for signal-level exits, branch on != 0 for clean failures, branch on == 0 for success"
    - "Regression-fixture 4-section doc-comment template: Motivating Incident / Layout Diagram / Fix Summary / Severity — formalized in docs/regression-fixtures.md"

key-files:
  created:
    - "docs/regression-fixtures.md — +193 lines: full 4-section template doc with canonical example reference, template skeleton, adopter list, prevent/detect/document rationale"
  modified:
    - ".github/workflows/ci.yml — +52 lines: new build-and-test-release job (ubuntu-latest, clang, Release, no sanitizer step), existing build-and-test Debug job untouched"
    - "tests/integration/run_integration.sh — +46/-3 lines: REG-01 exit-code policy docblock, set +e/-e around ironc invocation, [CRASH] branch on exit >= 128, [FAIL] branch with explicit exit code on clean non-zero exit"

key-decisions:
  - "[Phase 66-04] New Release CI job is a separate top-level job, NOT a new matrix dimension on build-and-test — preserves the Debug job's simple [ubuntu-latest, macos-latest] matrix and lets Release evolve its own configuration independently. Matches the precedent set by .github/workflows/web.yml (also a separate workflow file for a distinct build type)."
  - "[Phase 66-04] Release job passes build/iron (not build/ironc) to run_integration.sh to match the script's declared contract at line 16 (IRON_BIN_ARG default is build/iron). Existing Debug job's build/ironc argument is left unchanged to avoid scope creep; the script's IRON_BIN resolution works with either argument."
  - "[Phase 66-04] run_integration.sh crash detection uses set +e/-e bracket rather than || { ... } because the existing (cd ... && ...) subshell already runs under the outer set -e; capturing $? cleanly across the subshell required the explicit bracket. This pattern is reusable for any future CI script that needs to distinguish signal exits."
  - "[Phase 66-04] Crash branch exits the whole script with code 1 immediately, not via a counter increment — the CI log line [CRASH] ironc crashed on fixture X surfaces the exact fixture that triggered the crash, and the early exit prevents a crashy ironc from spewing 100+ fixture failures that would bury the root cause."
  - "[Phase 66-04] Original CONTEXT comment text 'Verify sanitizers step omitted' was rephrased to 'Sanitizer-verification step omitted' so the new job block does not grep-match the literal string 'Verify sanitizers' (which is the Debug job's step name). The acceptance-criteria grep specifically asserts the new block has zero 'Verify sanitizers' matches to guarantee the step is not accidentally duplicated."
  - "[Phase 66-04] 'do not swallow SIGSEGV' is written in lowercase (twice) in run_integration.sh to match the case-sensitive artifact check in 66-04-PLAN.md must_haves.artifacts — future contributors should feel free to capitalize in new comments; the lowercase strings are load-bearing acceptance-criteria tokens."
  - "[Phase 66-04] docs/regression-fixtures.md lists the 6 PROT-04 fixture names as a forward reference to Plan 03's landings. If Plan 03 chooses different fixture names, this doc must be updated in the same commit to keep the two in sync; the doc explicitly flags this expectation."

patterns-established:
  - "Parallel-job CI pattern: a new build type (Debug vs Release, native vs web, etc.) gets a new top-level job, not a new matrix dimension. Copy the reference job's steps verbatim and alter only the build-type-specific bits (CMAKE_BUILD_TYPE, sanitizer env vars, optional verification steps). Keeps matrix jobs minimal and lets each build type evolve independently as its own required status check."
  - "Crash-exit protocol for bash test runners: (1) set +e around the risky call, (2) capture $? into a named variable, (3) set -e again, (4) branch on >= 128 for signal-level exits with a distinct [CRASH] report that fails the whole run, (5) branch on != 0 for clean failures counted per-fixture. This protocol is mandatory for any script that invokes a compiler or interpreter that might segfault."
  - "4-section regression fixture doc-comment: every fixture that protects against a specific bug class opens with Motivating Incident / Layout Diagram / Fix Summary / Severity, in that order, labeled in bold. Makes red tests self-explanatory to the contributor who bisected them."

requirements-completed: [REG-01, REG-04]

# Metrics
duration: 15min
completed: 2026-04-13
---

# Phase 66 Plan 04: Linux Release CI + Regression Fixture Template Summary

**Linux Release CI job (build-and-test-release, ubuntu-latest, clang) running Release-mode ironc against the full ctest + integration suite, plus a crash-exit protocol in run_integration.sh that refuses to swallow SIGSEGV as generic [FAIL] noise, plus docs/regression-fixtures.md formalizing the 4-section Motivating Incident / Layout Diagram / Fix Summary / Severity template adopted by hir_to_lir_elif_mono_walker.iron and the 6 Plan 03 PROT-04 fixtures.**

## Performance

- **Duration:** 15 min
- **Started:** 2026-04-13T00:23:57Z
- **Completed:** 2026-04-13T00:38:43Z
- **Tasks:** 3
- **Files modified:** 2 (ci.yml, run_integration.sh)
- **Files created:** 1 (docs/regression-fixtures.md)

## Accomplishments

- **REG-01 shipped.** New `build-and-test-release (ubuntu-latest)` job in `.github/workflows/ci.yml` builds `ironc` with `CMAKE_BUILD_TYPE=Release` (clang, no sanitizers) and runs both `ctest -E "benchmark"` and `tests/integration/run_integration.sh build/iron`. This is the missing CI signal that would have caught the `find_first_last` SIGSEGV months earlier — the pre-Phase 66 Benchmark workflow reported Release-build compile crashes as "benchmark compilation failed", swallowing the real signal. The new job is a parallel required status check that sits alongside the existing Debug/ASan job without modifying it.
- **Crash-exit protocol hardened.** `tests/integration/run_integration.sh` now captures `ironc`'s exit code explicitly under `set +e`/`set -e` and branches on it: exit `>= 128` (SIGSEGV=139, SIGABRT=134, etc.) prints `[CRASH] ironc crashed on fixture X (exit=N)` to stderr and exits the script immediately with code 1; clean non-zero exits still count as per-fixture `[FAIL] (build failed, exit=N)` and increment the `FAIL` counter as before. The exit-code policy is documented in an 8-line header block that explicitly cites the Phase 65/66 `find_first_last` motivating incident as the anti-pattern it exists to prevent.
- **REG-04 shipped.** `docs/regression-fixtures.md` (193 lines, 1194 words) formalizes the 4-section regression-fixture doc-comment template. Cites `tests/integration/hir_to_lir_elif_mono_walker.iron` as the canonical example, breaks down each of the 4 sections (Motivating Incident / Layout Diagram / Fix Summary / Severity) with contributor-facing rationale, provides a copy-paste template skeleton, and lists all 6 Plan 03 PROT-04 fixtures as initial adopters plus Phase 67 REG-02 crash canaries as the next adopter. Closes with the prevent/detect/document triangle tying PROT-01..04 + REG-01 + REG-04 together.

## Task Commits

Each task was committed atomically:

1. **Task 1: Harden run_integration.sh exit-code propagation** — `4594cfd` (fix)
2. **Task 2: Add build-and-test-release (ubuntu-latest) CI job** — `b4565c6` (feat)
3. **Task 3: Create docs/regression-fixtures.md** — `4384ac3` (docs)

**Plan metadata commit:** _pending — final commit at end of execute-phase_

## Files Created/Modified

- **`.github/workflows/ci.yml`** (+52 lines) — Appended new `build-and-test-release` job after existing `build-and-test` Debug job. New job runs `ubuntu-latest` only (no matrix), uses clang, configures with `CMAKE_BUILD_TYPE=Release`, runs `ctest --test-dir build --output-on-failure -j4 -E "benchmark"` + `tests/integration/run_integration.sh build/iron`. No sanitizer env vars, no Verify-sanitizers step. Header comment cites `REG-01` and the `find_first_last` motivating incident.
- **`tests/integration/run_integration.sh`** (+46 / -3 lines) — Added 8-line REG-01 exit-code policy header block citing `find_first_last` incident. Replaced `if ! (cd ... && ironc ...) 2>stderr; then [FAIL]` with an explicit `set +e`/`ironc_exit=$?`/`set -e` capture, followed by two branches: `>= 128` → print `[CRASH] ironc crashed on fixture X (exit=N)` with 5 lines of stderr context and `exit 1` immediately; `!= 0` → print `[FAIL] (build failed, exit=N)` and `continue`. Both branches preserve the existing `cat "${build_stderr}" >&2` diagnostic output. The phrase `do not swallow SIGSEGV` appears in lowercase twice (once in the header docblock, once in the [CRASH] branch) as an acceptance-criteria grep token and a human-facing anti-pattern reminder.
- **`docs/regression-fixtures.md`** (new file, +193 lines, 1194 words) — Contributor-facing documentation. Six sections: (1) Canonical Example referencing `hir_to_lir_elif_mono_walker.iron`, (2) The 4-Section Template with subsections for Motivating Incident / Layout Diagram / Fix Summary / Severity, (3) Template Skeleton copy-paste block, (4) Fixtures Adopting This Template (Reference + Phase 66 Plan 03 + Phase 67 REG-02 planned), (5) Why This Matters (prevent/detect/document triangle), (6) See Also cross-references to CORRECTNESS-AUDIT.md, REQUIREMENTS.md, ci.yml, run_integration.sh.

## Decisions Made

All 7 key decisions are captured in the frontmatter `key-decisions` block. Summary:

1. **Separate top-level job, not matrix dimension.** New CI job is `build-and-test-release` alongside `build-and-test`, not a new entry in `[ubuntu-latest, macos-latest]`. Matches `.github/workflows/web.yml` precedent.
2. **`build/iron` not `build/ironc`** as the argument to `run_integration.sh` in the new job — matches the script's declared `IRON_BIN_ARG` default at line 16. Existing Debug job's `build/ironc` left unchanged to avoid scope creep.
3. **`set +e`/`set -e` bracket** to capture `$?` around the ironc subshell rather than `||`-chain, because the existing `(cd ... && ...)` subshell already runs under the outer `set -e` and the bracket pattern surfaces the signal-level exit code cleanly.
4. **Crash branch exits immediately** with code 1, not via the `FAIL` counter — prevents a crashy ironc from spewing 100+ per-fixture failures that would bury the root cause.
5. **Comment reworded** from "Verify sanitizers step omitted" to "Sanitizer-verification step omitted" so the new job block does not grep-match the Debug job's step name (which would fail an acceptance-criteria grep).
6. **Lowercase "do not swallow SIGSEGV"** twice in `run_integration.sh` to match case-sensitive acceptance-criteria grep tokens; future contributors may capitalize in new comments, but these two instances are load-bearing.
7. **Fixture name list in `docs/regression-fixtures.md` is a forward reference** to Plan 03's landings; if Plan 03 uses different names, the doc must be updated in the same commit to stay in sync. The doc explicitly flags this expectation.

## Deviations from Plan

None — plan executed exactly as written. All 3 tasks landed in the order specified, every acceptance-criteria grep passed, `bash -n` on `run_integration.sh` is clean, `python3 + pyyaml` parses `ci.yml` cleanly and reports `jobs: ['build-and-test', 'build-and-test-release']`, the full local integration suite still reports `346 passed, 0 failed, 354 total` (same as pre-Plan-04).

## Issues Encountered

- **Acceptance-criteria grep token case sensitivity.** First pass of `run_integration.sh` wrote `"Do not swallow SIGSEGV"` (capital D) in both the header docblock and the crash-branch echo line, which failed the case-sensitive `grep -q "do not swallow SIGSEGV"` acceptance check. Fixed by lowercasing the `D` in both occurrences. Documented as key-decision #6 so future contributors understand why these two strings are written in lowercase.
- **Comment containing step name of the job it was documenting.** First pass of the `build-and-test-release` header comment contained the literal string `"Verify sanitizers" step omitted`, which matched the Debug job's step name `Verify sanitizers` and failed the acceptance-criteria check `sed -n '/build-and-test-release:/,$p' .github/workflows/ci.yml | grep -c "Verify sanitizers"` must return 0. Fixed by rephrasing to "Sanitizer-verification step omitted (Release builds are not instrumented, so re-running ctest under ASAN/UBSAN halt-on-error flags would be a no-op)". Documented as key-decision #5.

Both issues were caught by the acceptance-criteria grep checks inside the task's `<verify>` block — exactly what those checks are for. No plan regressions, no deferred items.

## Local Verification

Because GitHub Actions workflows cannot be executed offline, the Release CI job's correctness was verified by:

1. **YAML syntax.** `python3 -c "import yaml; d = yaml.safe_load(open('.github/workflows/ci.yml')); print(sorted(d['jobs'].keys()))"` outputs `['build-and-test', 'build-and-test-release']` cleanly — the workflow file parses as two top-level jobs.
2. **Structural grep checks.** All 10 acceptance-criteria grep patterns from Task 2 pass: job counts (1 each for Debug and Release), `CMAKE_BUILD_TYPE=Release` present, `CMAKE_BUILD_TYPE=Debug` preserved, `REG-01` and `find_first_last` comments present, zero `Verify sanitizers` and zero `ASAN_OPTIONS` in the Release block, `runs-on: ubuntu-latest` present once, `clang` present 3 times (install + compile var + comment).
3. **Full integration suite.** `tests/integration/run_integration.sh build/iron` runs to completion and reports `346 passed, 0 failed, 354 total` on the pre-Plan-03 tree, same as before the hardening edits — confirming the crash-exit protocol does not regress any existing fixture.
4. **`bash -n` syntax check.** `run_integration.sh` parses cleanly; the `set +e`/`set -e` bracket around the subshell does not confuse bash's syntax validator.

The Release job itself cannot be executed locally without reproducing a GitHub Actions runner, but its steps are a near-duplicate of the already-green Debug job's steps with only `CMAKE_BUILD_TYPE=Release` and the removed sanitizer step as differences. The combination of the pyyaml structural check + the grep structural checks + the identical-to-Debug step content gives high confidence that the job will run cleanly on first push.

## Pitfalls Recorded

- **Bash `set -e` interaction with subshells.** The existing invocation pattern `if ! (cd ... && ironc ...) 2>stderr; then ... fi` works because `if` disables `set -e` for its condition expression; replacing it with a direct call requires explicit `set +e`/`set -e` bracketing to capture `$?` without the script exiting. Do not use `||` because the subshell return value gets conflated with the OR chain.
- **GitHub Actions comment strings that match step names.** Comments inside a new job block will be grep-matched by acceptance-criteria checks that look for step names. Phrase comments to avoid accidental matches: use "Sanitizer-verification step omitted" instead of "`Verify sanitizers` step omitted", use "build-and-test" only as a code-block identifier not inside explanatory prose.
- **Markdown internal links to `../tests/integration/*.iron`.** The `docs/regression-fixtures.md` file lives in `docs/` and links to fixture files in `tests/integration/` via relative `../tests/integration/...` paths. These resolve correctly when rendered on GitHub but will not preview in some local Markdown viewers that do not handle parent-directory relative paths. Not a correctness issue, just a viewer caveat for contributors editing the doc locally.
- **Acceptance-criteria grep tokens are case-sensitive and load-bearing.** When a task specifies `contains: "string"` in its `must_haves.artifacts` block, the executor must write that exact string (including case) into the file. Two occurrences of `do not swallow SIGSEGV` and one occurrence of `ironc crashed` in `run_integration.sh` are load-bearing tokens; do not capitalize or rephrase them without updating the plan's acceptance criteria.

## Next Phase Readiness

- **Plan 04 REG-01 + REG-04 complete; ready for Plan 05 (M/L severity `IRON_NODE_ASSERT_KIND` walkthrough)** which is the original sequential next plan per the 66-05-PLAN.md that already exists in the phase directory.
- **Plan 03 dependency satisfied.** Plan 03 (PROT-04 H-severity blind-cast rewrites) can reference `docs/regression-fixtures.md` when it writes its 6 new regression fixtures. The fixture names listed in `docs/regression-fixtures.md` are the target names; if Plan 03 chooses different names, the doc must be updated in the same commit to stay in sync (explicitly flagged in the doc).
- **No blockers.** The new Release job does not depend on Plan 03 to pass — it can run immediately on the existing tree with the existing fixtures and will catch any Release-only regressions that land on future PRs. Plan 03's new fixtures will run in both the Debug job and the new Release job once they land.
- **Motivating incident closed.** The `find_first_last` SIGSEGV story — find_first_last hit in Benchmark, reported as `find_first_last: Iron compilation failed`, hid for months — can no longer recur. If a Release-only crash lands today, `build-and-test-release` runs on every push and PR, and `run_integration.sh` refuses to mask the signal-level exit.

## Self-Check

Per execute-plan self-check protocol, verified that all SUMMARY.md claims correspond to files and commits that exist on disk.

**File existence:**
- `.github/workflows/ci.yml` — FOUND (modified)
- `tests/integration/run_integration.sh` — FOUND (modified)
- `docs/regression-fixtures.md` — FOUND (new)
- `.planning/phases/66-structural-protections-linux-release-ci/66-04-SUMMARY.md` — this file, being written now

**Commit existence:**
- `4594cfd` (Task 1) — FOUND on main
- `b4565c6` (Task 2) — FOUND on main
- `4384ac3` (Task 3) — FOUND on main

## Self-Check: PASSED

---

*Phase: 66-structural-protections-linux-release-ci*
*Plan: 04*
*Completed: 2026-04-13*
