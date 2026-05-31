#!/usr/bin/env bash
# scripts/sign-and-notarize-macos.sh
# Phase 36 Plan 04 (REL-11) — macOS code-sign + notarize + staple for
# `ironls` Mach-O release binaries.
#
# Idempotency
# -----------
# Re-running this script on already-signed-and-stapled binaries MUST be
# safe and produce no observable change beyond a fresh ticket re-fetch:
#
#   - `codesign --force --sign ...`   — `--force` makes re-signing idempotent.
#   - `xcrun notarytool submit ...`   — submitting the same already-notarized
#     binary returns a fresh Accepted UUID; no harm done (just a 5-20 min wait).
#   - `xcrun stapler staple ...`      — stapling an already-stapled binary is a
#     no-op (the staple ticket is replaced with itself).
#
# Re-run discipline: if any step fails mid-binary, re-run the script with the
# same args. It will resume safely.
#
# Required environment variables (fails fast if any is unset)
# -----------------------------------------------------------
#   APPLE_DEV_ID_APP_IDENTITY  — codesign identity string, e.g.
#                                "Developer ID Application: <YOUR NAME> (<TEAM_ID>)"
#   APPLE_ID_USERNAME          — Apple ID login email used with notarytool
#                                (maps to `APPLE_ID_EMAIL` in
#                                docs/dev/apple-notarization-setup.md Step 4)
#   APPLE_ID_APP_PASSWORD      — app-specific password from appleid.apple.com
#                                (Step 4 in apple-notarization-setup.md)
#   APPLE_TEAM_ID              — 10-char Apple Developer Team ID
#                                (Step 5 in apple-notarization-setup.md)
#
# Usage
# -----
#   bash scripts/sign-and-notarize-macos.sh [BINARY ...]
#
# If no binaries are given, defaults to:
#   dist/ironls-macos-arm64/ironls dist/ironls-macos-x86_64/ironls
#
# Exit codes
# ----------
#   0  All binaries signed + notarized + stapled + verified.
#   1  One or more binaries failed (the failing binary name is echoed to stderr).
#   2  Required environment variable missing.
#   3  Required tool missing (codesign / xcrun / ditto).

set -euo pipefail

# ---------------------------------------------------------------------------
# Usage / help.
# ---------------------------------------------------------------------------
print_usage() {
  cat <<'EOF'
Usage: bash scripts/sign-and-notarize-macos.sh [BINARY ...]

Signs, notarizes, and staples each given macOS Mach-O binary for distribution
without Gatekeeper warnings. Idempotent (safe to re-run).

Defaults (when no args given):
  dist/ironls-macos-arm64/ironls dist/ironls-macos-x86_64/ironls

Required env vars (set before invoking):
  APPLE_DEV_ID_APP_IDENTITY   "Developer ID Application: <name> (<TEAM_ID>)"
  APPLE_ID_USERNAME           Apple ID login email
  APPLE_ID_APP_PASSWORD       app-specific password (appleid.apple.com)
  APPLE_TEAM_ID               10-char Team ID

See docs/dev/PHASE-36-MACOS-NOTARIZE-PLAYBOOK.md for the step-by-step
maintainer playbook.
EOF
}

case "${1:-}" in
  -h|--help)
    print_usage
    exit 0
    ;;
esac

# ---------------------------------------------------------------------------
# Verify required env vars (fail fast with a clear message — Pitfall 5).
# ---------------------------------------------------------------------------
require_env() {
  local var="$1"
  if [ -z "${!var:-}" ]; then
    echo "ERROR: required environment variable ${var} is not set." >&2
    echo "       See script header or docs/dev/PHASE-36-MACOS-NOTARIZE-PLAYBOOK.md." >&2
    exit 2
  fi
}
require_env APPLE_DEV_ID_APP_IDENTITY
require_env APPLE_ID_USERNAME
require_env APPLE_ID_APP_PASSWORD
require_env APPLE_TEAM_ID

# ---------------------------------------------------------------------------
# Verify required tools.
# ---------------------------------------------------------------------------
require_tool() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "ERROR: required tool '$1' not found on PATH." >&2
    echo "       Install Xcode Command Line Tools: xcode-select --install" >&2
    exit 3
  fi
}
require_tool codesign
require_tool xcrun
require_tool ditto

# ---------------------------------------------------------------------------
# Default binary list when no positional args.
# ---------------------------------------------------------------------------
if [ "$#" -eq 0 ]; then
  set -- "dist/ironls-macos-arm64/ironls" "dist/ironls-macos-x86_64/ironls"
fi

FAILED=()

# ---------------------------------------------------------------------------
# Per-binary pipeline: sign -> zip -> notarize -> staple -> verify.
# ---------------------------------------------------------------------------
process_binary() {
  local bin="$1"
  local base
  base="$(basename "${bin}")"

  if [ ! -f "${bin}" ]; then
    echo "ERROR [${bin}]: file not found." >&2
    return 1
  fi

  echo ""
  echo "============================================================"
  echo "Processing: ${bin}"
  echo "============================================================"

  # (a) codesign with hardened runtime + secure timestamp + force (idempotent).
  echo "[1/5] codesign ${base}..."
  codesign \
    --sign "${APPLE_DEV_ID_APP_IDENTITY}" \
    --options runtime \
    --timestamp \
    --force \
    --verbose=2 \
    "${bin}"

  # (b) zip the binary for notarytool (it accepts .zip, .dmg, .pkg).
  local zip_path="/tmp/notarize-${base}-$$.zip"
  echo "[2/5] ditto -c -k --keepParent ${base} ${zip_path}..."
  ditto -c -k --keepParent "${bin}" "${zip_path}"

  # (c) submit to Apple notary service and wait.
  echo "[3/5] xcrun notarytool submit ${zip_path} --wait..."
  if ! xcrun notarytool submit "${zip_path}" \
        --apple-id "${APPLE_ID_USERNAME}" \
        --password "${APPLE_ID_APP_PASSWORD}" \
        --team-id "${APPLE_TEAM_ID}" \
        --wait \
        --timeout 30m; then
    echo "ERROR [${bin}]: notarytool submit failed." >&2
    rm -f "${zip_path}"
    return 1
  fi

  # (d) staple the notarization ticket onto the binary.
  echo "[4/5] xcrun stapler staple ${base}..."
  if ! xcrun stapler staple "${bin}"; then
    echo "ERROR [${bin}]: stapler staple failed." >&2
    rm -f "${zip_path}"
    return 1
  fi

  # (e) verify: codesign --verify, spctl --assess, stapler validate.
  echo "[5/5] verify (codesign --verify, spctl --assess, stapler validate)..."
  codesign --verify --verbose=2 "${bin}"
  spctl --assess --type execute --verbose "${bin}"
  xcrun stapler validate "${bin}"

  # Cleanup temp zip.
  rm -f "${zip_path}"

  echo "OK   [${bin}]: signed, notarized, stapled, verified."
  return 0
}

for bin in "$@"; do
  if ! process_binary "${bin}"; then
    FAILED+=("${bin}")
  fi
done

# ---------------------------------------------------------------------------
# Final report.
# ---------------------------------------------------------------------------
echo ""
echo "============================================================"
if [ "${#FAILED[@]}" -eq 0 ]; then
  echo "All $# binaries signed + notarized + stapled successfully."
  exit 0
else
  echo "FAILED: ${#FAILED[@]} of $# binaries:" >&2
  for f in "${FAILED[@]}"; do
    echo "  - ${f}" >&2
  done
  exit 1
fi
