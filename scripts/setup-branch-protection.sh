#!/usr/bin/env bash
# scripts/setup-branch-protection.sh
# Phase 14 REL-04 — configure main branch protection via gh api.
# Phase 36 REL-09 — extended to 9 checks (added v4-acceptance milestone-v3.0 gate).
# Idempotent: re-running with the same input produces no observable diff.
#
# Required: authenticated `gh` CLI with administration:write permission on the
# target repository (PAT or GitHub App token).
#
# Usage:
#   bash scripts/setup-branch-protection.sh
#
# Sources the canonical 9-check list from docs/dev/ci-gates.md (Phase 7 HARD-23
# + Phase 36 REL-09). If docs/dev/ci-gates.md is absent, falls back to an
# inline array.
# TODO (if fallback fires): verify check names against actual CI workflows in
# .github/workflows/ci.yml, parity.yml, tsan.yml, slos.yml, v4-acceptance.yml.

set -euo pipefail

# Default owner/repo resolve from the local git remote when unset, so running
# the playbook from a checkout targets the right repository (the previous
# hardcoded default `iron-lang/iron-lang` pointed at a repo that is not this
# one). Env vars still override for CI use.
detect_nwo() { git remote get-url origin 2>/dev/null | sed -E 's#^(git@[^:]+:|https://[^/]+/)##; s#\.git$##'; }
OWNER="${GITHUB_REPOSITORY_OWNER:-$(detect_nwo | cut -d/ -f1)}"
REPO="${GITHUB_REPOSITORY_NAME:-$(detect_nwo | cut -d/ -f2)}"
if [ -z "${OWNER}" ] || [ -z "${REPO}" ]; then
  echo "ERROR: could not determine owner/repo (no origin remote?). Set" >&2
  echo "GITHUB_REPOSITORY_OWNER and GITHUB_REPOSITORY_NAME explicitly." >&2
  exit 2
fi

# Verify gh is authenticated; fail fast with a clear error if not (Pitfall 5).
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: gh CLI is not authenticated. Run 'gh auth login' first." >&2
  echo "Required scope: administration:write on ${OWNER}/${REPO}." >&2
  exit 2
fi

# ---------------------------------------------------------------------------
# Resolve the 9 required check names.
# Primary source: docs/dev/ci-gates.md (Phase 7 HARD-23 + Phase 36 REL-09
# single source of truth).
# Fallback: inline array (see TODO above).
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CI_GATES_FILE="${REPO_ROOT}/docs/dev/ci-gates.md"

if [ -f "${CI_GATES_FILE}" ]; then
  echo "Reading required checks from docs/dev/ci-gates.md..."
  # Extract the check-name (first backtick group) from each numbered table
  # row: lines that begin with `| <digit> |`. Prose backticks elsewhere in
  # the document (e.g. `main`, `ironc`, `parity` in the Purpose section)
  # are intentionally excluded.
  #
  # Phase 36 REL-09 hardening: prior implementation used `grep -oE ... |
  # head -8` and silently emitted junk like `main`, `ironc`, `parity`
  # plus workflow filenames; that worked by accident only because the
  # branch-protection PUT was never re-run with a divergent doc. Use a
  # targeted awk extractor instead.
  mapfile -t CHECKS < <(
    awk -F'`' '/^\| *[0-9]+ *\|/ { print $2 }' "${CI_GATES_FILE}" \
      | head -9
  )
  if [ "${#CHECKS[@]}" -lt 9 ]; then
    echo "WARNING: could not parse 9 checks from ci-gates.md (got ${#CHECKS[@]}). Falling back to inline list." >&2
    CHECKS=(
      "build-and-test (ubuntu-latest)"
      "build-and-test (macos-latest)"
      "vscode-e2e"
      "neovim-e2e"
      "zed-package-validate"
      "parity"
      "tsan"
      "slos"
      # Phase 36 REL-09: v4-acceptance milestone-v3.0 headline metric.
      "v4-acceptance"
    )
  fi
else
  echo "WARNING: docs/dev/ci-gates.md not found. Using inline check list." >&2
  echo "TODO: verify check names against actual CI workflows." >&2
  CHECKS=(
    "build-and-test (ubuntu-latest)"
    "build-and-test (macos-latest)"
    "vscode-e2e"
    "neovim-e2e"
    "zed-package-validate"
    "parity"
    "tsan"
    "slos"
    # Phase 36 REL-09: v4-acceptance milestone-v3.0 headline metric.
    "v4-acceptance"
  )
fi

echo "Configuring branch protection on ${OWNER}/${REPO}/main..."
echo "Required checks (${#CHECKS[@]}):"
for c in "${CHECKS[@]}"; do
  echo "  - ${c}"
done

# ---------------------------------------------------------------------------
# Build the JSON body using jq.
# ---------------------------------------------------------------------------
CONTEXTS_JSON=$(printf '%s\n' "${CHECKS[@]}" | jq -R . | jq -s .)

BODY=$(jq -n \
  --argjson contexts "${CONTEXTS_JSON}" \
  '{
    required_status_checks: {
      strict: true,
      contexts: $contexts
    },
    enforce_admins: true,
    required_pull_request_reviews: {
      required_approving_review_count: 1,
      dismiss_stale_reviews: true
    },
    restrictions: null,
    allow_force_pushes: false,
    allow_deletions: false
  }')

gh api \
  --method PUT \
  -H "Accept: application/vnd.github+json" \
  "/repos/${OWNER}/${REPO}/branches/main/protection" \
  --input - <<<"${BODY}"

echo ""
echo "Branch protection configured on ${OWNER}/${REPO}/main."
echo "Verify with:"
echo "  gh api repos/${OWNER}/${REPO}/branches/main/protection | jq '.required_status_checks.contexts'"
