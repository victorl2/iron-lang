# CI Gates and Branch Protection

> Phase 7 Plan 07-08 (HARD-24). This document is the single source of truth
> for the GitHub branch-protection configuration on `main`. If this file
> drifts from the actual UI state, the file is authoritative — fix the UI.

## Purpose

**The Core Value is enforced by branch protection.** A PR that would cause
LSP diagnostics, formatting, or any other facade output to diverge from
what `ironc` produces on the CLI cannot merge. Every other gate (build,
sanitizers, SLOs, fuzzing, editor e2e) protects secondary invariants;
`parity` protects the *defining property* of the project.

Quoting CONTEXT §D-11 verbatim:

> Divergence means the LSP would lie to the user. There is no legitimate
> scenario where merging a divergent change is correct. If you think
> there is, open an issue explaining why; don't bypass the gate.

## Required status checks (paste these exact names into the GitHub UI)

As of Phase 7 close the full required-status-check list is **8 checks**:

| # | Check name                         | Phase | Workflow file                    | What it proves                                                                 |
|---|------------------------------------|-------|----------------------------------|--------------------------------------------------------------------------------|
| 1 | `build-and-test (ubuntu-latest)`   | 2+    | `.github/workflows/ci.yml`       | Linux Debug+ASan build passes all unit + integration tests                     |
| 2 | `build-and-test (macos-latest)`    | 2+    | `.github/workflows/ci.yml`       | macOS Debug+ASan build passes all unit + integration tests                     |
| 3 | `vscode-e2e`                       | 6     | `.github/workflows/ci.yml`       | VSCode extension launches + attaches + receives diagnostics end-to-end         |
| 4 | `neovim-e2e`                       | 6     | `.github/workflows/ci.yml`       | Neovim plenary.nvim harness attaches + receives diagnostics end-to-end         |
| 5 | `zed-package-validate`             | 6     | `.github/workflows/ci.yml`       | Zed extension compiles to `wasm32-wasip2` + extension.toml validates           |
| 6 | `parity`                           | 7     | `.github/workflows/parity.yml`   | **Core Value**: `ironc` output ≡ `ironls` facade output across every fixture   |
| 7 | `tsan`                             | 7     | `.github/workflows/tsan.yml`     | ThreadSanitizer mini-soak reports zero data races                              |
| 8 | `slos`                             | 7     | `.github/workflows/slos.yml`     | Per-request p50 SLOs enforced (hover < 20ms, completion < 100ms, diag < 500ms) |

The job/check name on the GitHub Status API side matches the `name:` field
of the job in the workflow YAML. The `parity` check in particular is a
**dedicated single-job workflow** (not a matrix job) precisely so the name
is stable and wire-able into the required-checks list. See Research
Dimension 9 — required-checks wiring.

### Not yet required (but CI-green)

| Check name                     | Why not required yet                                                     |
|--------------------------------|--------------------------------------------------------------------------|
| `soak-short`                   | 30-minute run; too long for PR gate. Runs nightly on `main` as monitor. |
| `soak-nightly`                 | 8-hour run; informational. Regressions are investigated, not blocking.  |
| `build-time`                   | 1.15× baseline fires an issue; not a merge block (tunable to block v1.x). |
| `fuzz-*`                       | libFuzzer cron job; findings are triaged, not merge-blocking.            |
| `coverage`                     | Non-regressing metric; tracked, not gated.                               |
| `vscode-package-dryrun`        | Already covered transitively by `vscode-e2e`.                            |
| `tree-sitter-wasm`             | Covered by `vscode-e2e` bundle which contains the wasm.                  |

These can be promoted to required as signal matures post-v1 — document the
decision in this file before editing the GitHub UI.

## Configuration walkthrough (GitHub UI)

1. **Repository → Settings → Branches**.
2. Under **Branch protection rules**, click **Edit** next to `main` (or
   **Add branch protection rule** if missing; pattern `main`).
3. Check **Require a pull request before merging**.
   - Sub-check: **Require approvals** ≥ 1 (v1 baseline).
   - Sub-check: **Dismiss stale pull request approvals when new commits are pushed**.
4. Check **Require status checks to pass before merging**.
   - Sub-check: **Require branches to be up to date before merging**.
   - In the search box, add each of the 8 names from the table above. Copy
     them verbatim — spelling must match exactly.
5. Check **Require conversation resolution before merging**.
6. Check **Require signed commits** (optional v1; recommended v1.x).
7. **Do not allow bypassing the above settings** — **MUST be checked**.
   This removes the administrator override that would otherwise let a
   repository admin click "merge anyway" on a red check. Administrators
   are not excluded from the Core-Value gate. See Threat T-07-08-03.
8. **Allow force pushes**: **never** on `main`.
9. **Allow deletions**: **never** on `main`.
10. Click **Save changes**.

## Zero-bypass policy

No one — including administrators — merges a PR with a red `parity` check.
No `--no-verify`, no admin override, no "I'll fix it in a follow-up PR",
no "the test is flaky" (fix the flake, then merge).

If `parity` fires and you believe it is a false positive:

1. Download the `parity-output` artifact attached to the failed run.
2. Reproduce locally: `cmake --build build -j && ctest --test-dir build -R 'test_parity_ironc_lsp(_fmt)?$' --output-on-failure`.
3. If the local reproduction passes but CI fails, open an issue tagged
   `parity-flake` with both logs attached. Do not merge.
4. If the local reproduction also fails, you have a real divergence.
   Fix the divergence in the same PR. This is non-negotiable per D-11.

## Drift monitoring

`tests/lsp/invariant/test_phase7_audit.sh` runs as a ctest invariant on
every PR (label `phase-m6-invariant`). It verifies:

- `.github/workflows/parity.yml` still exists and is a regular file.
- Every Phase 7 artifact from D-14 (supervisor.c, crash_dump.c, rss.c,
  soak harness, tsan driver, slos measurement, fuzz harnesses, notarize
  script, version.c, this file, etc.) still exists.
- Every `phase-m{1..6}-invariant` CTest label is populated.
- `test_parity_ironc_lsp` and `test_parity_ironc_lsp_fmt` are both
  registered under `phase-m6-invariant` (dual-labelled alongside their
  original labels).

If somebody deletes `parity.yml` in a PR, `test_phase7_audit` fires red
and the PR cannot merge — even before branch protection would catch the
missing check, the audit's own gate blocks it.

## Adding a new required check

1. Land the new workflow on `main` as **not-required**. Let it run on
   5-10 real PRs.
2. If it stays green and useful, update the table above in this file
   with a new row.
3. Only after this file is updated, add the check name to the GitHub UI
   required-checks list.

This ordering guarantees the UI state is always a subset of, or equal
to, what this file documents.

## Removing a required check

1. Open a PR that edits this file to delete the row **and** explains why
   (outdated coverage, replaced by a stronger check, etc.).
2. Merge the documentation PR.
3. Only then remove from the GitHub UI.

Never remove a required check ahead of the documentation PR.

## Related documents

- `docs/dev/release-runbook.md` — the v1.2.0-alpha.7 tag-cut procedure
  depends on branch protection being configured per this file.
- `docs/dev/publisher-namespace-checklist.md` — VSCode Marketplace + Zed
  Registry publish is human-gated, outside CI.
- `docs/dev/apple-notarization-setup.md` — 5 GitHub secrets that
  `release.yml` consumes when tagged.
- `.planning/phases/07-m6-production-hardening/07-CONTEXT.md` §D-11 —
  the rationale behind promoting parity to a blocking gate.
