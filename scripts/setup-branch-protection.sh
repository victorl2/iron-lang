#!/usr/bin/env bash
# scripts/setup-branch-protection.sh
# Phase 14 REL-04 — configure main branch protection via gh api.
# Idempotent: re-running with the same input produces no observable diff.
#
# Required: authenticated `gh` CLI with administration:write permission on the
# target repository (PAT or GitHub App token).
#
# Usage:
#   bash scripts/setup-branch-protection.sh
#
# Sources the canonical 8-check list from docs/dev/ci-gates.md (Phase 7 HARD-23).
# If docs/dev/ci-gates.md is absent, falls back to an inline array.
# TODO (if fallback fires): verify check names against actual CI workflows in
# .github/workflows/ci.yml, parity.yml, tsan.yml, slos.yml.

set -euo pipefail

OWNER="${GITHUB_REPOSITORY_OWNER:-iron-lang}"
REPO="${GITHUB_REPOSITORY_NAME:-iron-lang}"

# Verify gh is authenticated; fail fast with a clear error if not (Pitfall 5).
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: gh CLI is not authenticated. Run 'gh auth login' first." >&2
  echo "Required scope: administration:write on ${OWNER}/${REPO}." >&2
  exit 2
fi

# ---------------------------------------------------------------------------
# Resolve the 8 required check names.
# Primary source: docs/dev/ci-gates.md (Phase 7 HARD-23 single source of truth).
# Fallback: inline array (see TODO above).
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CI_GATES_FILE="${REPO_ROOT}/docs/dev/ci-gates.md"

if [ -f "${CI_GATES_FILE}" ]; then
  echo "Reading required checks from docs/dev/ci-gates.md..."
  # Extract the backtick-quoted check names from the table rows.
  # Each row has a leading backtick: | # | `check-name` | ...
  mapfile -t CHECKS < <(
    grep -oE '`[^`]+`' "${CI_GATES_FILE}" \
      | head -8 \
      | tr -d '`'
  )
  if [ "${#CHECKS[@]}" -lt 8 ]; then
    echo "WARNING: could not parse 8 checks from ci-gates.md (got ${#CHECKS[@]}). Falling back to inline list." >&2
    CHECKS=(
      "build-and-test (ubuntu-latest)"
      "build-and-test (macos-latest)"
      "vscode-e2e"
      "neovim-e2e"
      "zed-package-validate"
      "parity"
      "tsan"
      "slos"
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
