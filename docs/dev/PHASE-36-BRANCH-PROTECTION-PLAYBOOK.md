# Phase 36 Branch-Protection Playbook (REL-09)

> Phase 36 REL-09. Human-gated playbook for applying the refreshed
> 9-check branch-protection on `main`. The agent prepared the canonical
> list in `docs/dev/ci-gates.md` and the PUT-script in
> `scripts/setup-branch-protection.sh`. This file is what the maintainer
> runs from an authenticated terminal.

The actual `gh api PATCH` call requires repo administrator credentials
and cannot be automated by an agent. Every command below is copy-paste
ready; expected outputs are listed so deviations are immediately visible.

## 1. Prerequisites

- `gh` CLI installed (`gh --version` >= 2.0).
- Authenticated as a repo administrator with `administration:write` scope:
  ```bash
  gh auth login --scopes "repo,admin:repo_hook,administration"
  gh auth status
  ```
  Expected `gh auth status` output includes a line like
  `Token scopes: 'admin:repo_hook', 'administration', 'repo'`.
- `jq` installed (`jq --version` >= 1.6).
- Working directory is the repo root (the script resolves
  `docs/dev/ci-gates.md` relative to its own location, so any cwd works,
  but the verify-step `gh api` commands below assume `iron-lang/iron-lang`).

## 2. Dry-run — preview the JSON that will be PUT

Run the script with `set -x` to surface the body without sending it:

```bash
bash -x scripts/setup-branch-protection.sh 2>&1 | grep -A 40 '"required_status_checks"' | head -60
```

Confirm the `contexts` array contains all 9 names in this exact order:

1. `build-and-test (ubuntu-latest)`
2. `build-and-test (macos-latest)`
3. `vscode-e2e`
4. `neovim-e2e`
5. `zed-package-validate`
6. `parity`
7. `tsan`
8. `slos`
9. `v4-acceptance`

If any name is missing or misspelled, STOP — re-check `docs/dev/ci-gates.md`
table rows and the awk extractor in `setup-branch-protection.sh`.

## 3. Snapshot current protection (for rollback)

```bash
gh api repos/iron-lang/iron-lang/branches/main/protection \
  > "/tmp/branch-protection-snapshot-$(date +%Y%m%d-%H%M).json"
```

Keep this file in a safe place. To roll back later:

```bash
gh api --method PUT \
  -H "Accept: application/vnd.github+json" \
  /repos/iron-lang/iron-lang/branches/main/protection \
  --input /tmp/branch-protection-snapshot-YYYYMMDD-HHMM.json
```

(Substitute the real timestamp from your snapshot filename.)

## 4. Apply

```bash
bash scripts/setup-branch-protection.sh
```

Expected exit code: `0`. Expected last line of stdout:

```
Branch protection configured on iron-lang/iron-lang/main.
```

The script is idempotent — re-running with the same input produces no
observable diff.

## 5. Verify

Confirm the contexts array now contains all 9 names:

```bash
gh api repos/iron-lang/iron-lang/branches/main/protection \
  | jq '.required_status_checks.contexts'
```

Expected output (exact 9-element JSON array):

```json
[
  "build-and-test (ubuntu-latest)",
  "build-and-test (macos-latest)",
  "vscode-e2e",
  "neovim-e2e",
  "zed-package-validate",
  "parity",
  "tsan",
  "slos",
  "v4-acceptance"
]
```

Also verify zero-bypass discipline survived the PUT:

```bash
gh api repos/iron-lang/iron-lang/branches/main/protection \
  | jq '{enforce_admins: .enforce_admins.enabled, allow_force_pushes: .allow_force_pushes.enabled, allow_deletions: .allow_deletions.enabled}'
```

Expected:

```json
{
  "enforce_admins": true,
  "allow_force_pushes": false,
  "allow_deletions": false
}
```

Per `docs/dev/ci-gates.md` zero-bypass section: administrators are NOT
excluded from the Core-Value gate. If `enforce_admins` reads `false`,
re-run the script — something downgraded the bypass setting.

## 6. Failure modes

- **HTTP 401 from `gh api`**: the gh CLI is not authenticated. Run
  `gh auth login --scopes administration`.
- **HTTP 403** ("Resource not accessible by integration"): the token
  lacks `administration:write`. Re-run `gh auth login` with the scope
  flag from §1.
- **HTTP 422** ("Invalid request"): a context name doesn't match any
  actual check name on `main`. The most likely cause is that
  `v4-acceptance` has not yet appeared as a check on `main` because no
  PR has run since the Task-1 `.github/workflows/v4-acceptance.yml`
  change merged. Land a no-op PR first to seed the check name into
  GitHub's known-checks index, then re-run this script.
- **Script exits 2 from the `gh auth status` guard**: same as 401 —
  authenticate.
- **Parser parses fewer than 9 checks from ci-gates.md**: script
  automatically falls back to the inline 9-check array. If the warning
  fires, the doc-side table format drifted — fix the table layout in
  `docs/dev/ci-gates.md` and re-run.

## 7. Commit reference

Task 1 of plan `36-01` (commit `6b9e5ac`) updated the canonical doc and
the script, and added the dedicated `v4-acceptance.yml` workflow so the
check name resolves to a real GitHub status. This playbook references the
post-commit state.
