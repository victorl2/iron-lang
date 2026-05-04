#!/usr/bin/env bash
# HARD-21 (Phase 7 Plan 07-06, CONTEXT D-09): macOS Developer-ID signing
# + notarization + stapling of the `ironls` binary attached to a GitHub
# Release.
#
# Called from .github/workflows/release.yml `sign-and-notarize-macos` job
# AFTER the per-platform `build` job has produced `build/ironls` on a
# macOS runner. Runs ONLY on macos-latest (or equivalent) — requires Xcode
# Command Line Tools (`codesign`, `xcrun notarytool`, `xcrun stapler`,
# `security`, `ditto`) that are not available on Linux/Windows runners.
#
# Usage: sign_and_notarize_macos.sh <path-to-ironls-binary> <version> <arch>
#   <path-to-ironls-binary> — POSIX path to the unsigned ironls binary
#   <version>               — release version string (with or without `v`
#                             prefix; normalized to v-prefixed for filenames
#                             to match Phase 6 06-05 asset naming)
#   <arch>                  — one of: x86_64, aarch64 (Zed extension's
#                             current_platform() arch naming; matches the
#                             existing Phase 6 06-05 ironls tarball layout)
#
# Required env vars (sourced from GitHub Secrets via the workflow's
# `env:` block — never interpolated into `run:` scripts per T-07-06-01):
#   APPLE_DEVELOPER_ID_P12_BASE64   — base64-encoded .p12 export
#   APPLE_DEVELOPER_ID_P12_PASSWORD — password used on P12 export
#   APPLE_ID_EMAIL                  — Apple ID login email
#   APPLE_ID_APP_PASSWORD           — app-specific password from
#                                     appleid.apple.com (NOT the account
#                                     password; notarytool rejects account
#                                     passwords)
#   APPLE_TEAM_ID                   — 10-char Team ID from Apple Developer
#                                     account membership page
#
# Optional env vars:
#   DRY_RUN=1                       — echo commands without executing
#                                     (local testing on non-macOS hosts)
#   KEYCHAIN_NAME_OVERRIDE=<name>   — use a specific keychain name instead
#                                     of `build-$$.keychain` (testing)
#
# Exit codes:
#   0   success
#   2   missing required env var or argv
#   3   no Developer ID Application identity found in keychain
#   4   notarization rejected/invalid (Apple verdict)
#   5   stapling verification failed
#
# Threat-model references (07-06-PLAN.md):
#   T-07-06-01  credential leak via log                 → env-only; no set -x
#   T-07-06-02  P12 leak into default keychain          → per-run keychain
#   T-07-06-03  wrong-binary notarize                   → pre/post SHA logged
#   T-07-06-04  notarytool hang                         → --timeout 1200 + poll
#   T-07-06-06  keychain password brute-force           → 256-bit openssl rand
#   T-07-06-07  P12 on disk                             → rm immediately

set -euo pipefail

# Never enable `set -x` — command traces would echo secret bash variables
# that appear on codesign/notarytool argv. Instead, emit specific INFO
# messages at each step for auditability.

IRONLS_BINARY="${1:?missing IRONLS_BINARY arg}"
VERSION="${2:?missing VERSION arg}"
ARCH="${3:?missing ARCH arg}"

# Normalize version to v-prefixed form so the produced tarball matches
# Phase 6 06-05 asset naming (`ironls-v<version>-<os>-<arch>.tar.gz`).
if [[ "${VERSION#v}" == "${VERSION}" ]]; then
  VERSION_TAG="v${VERSION}"
else
  VERSION_TAG="${VERSION}"
fi

# T-07-06-03: filename-pattern assertion. The resulting tarball MUST
# match the Zed-extension-consumed pattern. Assert <arch> is one of the
# two supported values before doing any signing work.
case "$ARCH" in
  x86_64|aarch64) ;;
  *) echo "ERROR: <arch> must be x86_64 or aarch64 (got: $ARCH)" >&2; exit 2 ;;
esac

# --- fail-fast env check ---
# All 5 secrets required; we abort BEFORE doing any work or printing any
# partial state if even one is missing. `${!var:-}` indirection avoids
# printing the value.
for var in APPLE_DEVELOPER_ID_P12_BASE64 APPLE_DEVELOPER_ID_P12_PASSWORD \
           APPLE_ID_EMAIL APPLE_ID_APP_PASSWORD APPLE_TEAM_ID; do
  if [[ -z "${!var:-}" ]]; then
    echo "ERROR: $var is required but not set" >&2
    exit 2
  fi
done

# T-07-06-04 mitigation: 1200-second primary-path cap via notarytool's
# own --timeout flag; 60 * 30s = 30-minute fallback polling ceiling.
NOTARIZE_TIMEOUT_SECONDS=1200
POLL_MAX_ITERATIONS=60
POLL_SLEEP_SECONDS=30

# `run` wrapper: in DRY_RUN mode, echo commands instead of executing.
# Used for local testing on non-macOS hosts (and by CI smoke-tests in
# per the <automated> verification block).
run() {
  if [[ "${DRY_RUN:-0}" == "1" ]]; then
    echo "[DRY] $*"
  else
    "$@"
  fi
}

# --- SHA-256 BEFORE signing (T-07-06-03 chain-of-custody) ---
# `shasum -a 256` is present on macOS by default; on Linux hosts used
# for DRY_RUN smoke-tests, fall back to `sha256sum` if shasum is absent.
if command -v shasum >/dev/null 2>&1; then
  SHA_CMD="shasum -a 256"
else
  SHA_CMD="sha256sum"
fi

if [[ "${DRY_RUN:-0}" == "1" ]] && [[ ! -f "$IRONLS_BINARY" ]]; then
  # In DRY_RUN mode, allow a non-existent binary path (the point of the
  # smoke test is to exercise control flow, not to actually compute a SHA).
  PRE_SIGN_SHA="dry-run-no-binary"
else
  PRE_SIGN_SHA=$($SHA_CMD "$IRONLS_BINARY" | cut -d' ' -f1)
fi
echo "INFO: pre-sign SHA-256: $PRE_SIGN_SHA ($IRONLS_BINARY)"

# --- 1. Per-run keychain setup (T-07-06-02 + T-07-06-06) ---
# 256-bit random password; per-run keychain isolation; 1-hour auto-lock
# timeout; default-keychain swap so codesign picks up our identity.
KEYCHAIN_PASSWORD="$(openssl rand -hex 32)"
KEYCHAIN="${KEYCHAIN_NAME_OVERRIDE:-build-$$.keychain}"
run security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
run security default-keychain -s "$KEYCHAIN"
run security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
run security set-keychain-settings -t 3600 "$KEYCHAIN"

# --- 2. Import P12 (T-07-06-07) ---
# Decode to /tmp/cert-$$.p12; import into per-run keychain; remove
# immediately after import. `-T /usr/bin/codesign` whitelists codesign
# to access the private key without a GUI prompt.
P12_TMP="/tmp/cert-$$.p12"
if [[ "${DRY_RUN:-0}" == "1" ]]; then
  echo "[DRY] echo \$APPLE_DEVELOPER_ID_P12_BASE64 | base64 --decode > $P12_TMP"
else
  echo "$APPLE_DEVELOPER_ID_P12_BASE64" | base64 --decode > "$P12_TMP"
fi
run security import "$P12_TMP" -k "$KEYCHAIN" \
  -P "$APPLE_DEVELOPER_ID_P12_PASSWORD" -T /usr/bin/codesign
if [[ "${DRY_RUN:-0}" != "1" ]]; then
  rm -f "$P12_TMP"
fi
# Allow codesign to use the imported key without an interactive allow/deny
# prompt — required on headless CI runners.
run security set-key-partition-list -S apple-tool:,apple: -s \
  -k "$KEYCHAIN_PASSWORD" "$KEYCHAIN"

# --- 3. Codesign with Hardened Runtime (--options runtime) ---
# Hardened Runtime is MANDATORY per Apple policy for notarization on
# macOS 10.14+. `--timestamp` attaches an RFC-3161 secure timestamp so
# the signature remains trusted after the cert's own expiration.
if [[ "${DRY_RUN:-0}" == "1" ]]; then
  SIGN_IDENTITY="Developer ID Application: DRY-RUN (${APPLE_TEAM_ID})"
else
  # Find the Developer ID Application identity in our keychain.
  SIGN_IDENTITY=$(security find-identity -v -p codesigning "$KEYCHAIN" \
    | grep -m1 "Developer ID Application" \
    | awk -F\" '{print $2}')
  if [[ -z "$SIGN_IDENTITY" ]]; then
    echo "ERROR: no Developer ID Application identity found in keychain $KEYCHAIN" >&2
    echo "       run: security find-identity -v -p codesigning $KEYCHAIN" >&2
    exit 3
  fi
fi
echo "INFO: signing identity: $SIGN_IDENTITY"
run codesign --force --options runtime --timestamp \
  --sign "$SIGN_IDENTITY" "$IRONLS_BINARY"
run codesign --verify --verbose "$IRONLS_BINARY"

# --- 4. Zip for notarization ---
# `ditto -c -k --keepParent` is the Apple-canonical zip format for
# notarization submissions; preserves Unix permissions + extended
# attributes that plain `zip(1)` strips.
ZIP_PATH="/tmp/ironls-notarize-$$.zip"
run ditto -c -k --keepParent "$IRONLS_BINARY" "$ZIP_PATH"

# --- 5. Notarize with Apple (T-07-06-04) ---
# Primary path: `notarytool submit --wait --timeout 1200`. The 1200s cap
# covers the known 2026 hang mode (RESEARCH §Pattern Pitfall 5). On
# primary-path failure, fall back to asynchronous submit + 30-minute
# info-polling (60 iterations * 30s). Overall job-level timeout is
# 30 minutes (set in release.yml `timeout-minutes: 30`).
echo "INFO: submitting to Apple notary (primary path, --wait, ${NOTARIZE_TIMEOUT_SECONDS}s cap)"
NOTARIZE_PRIMARY_OK=1
if ! run xcrun notarytool submit "$ZIP_PATH" \
    --apple-id "$APPLE_ID_EMAIL" \
    --password "$APPLE_ID_APP_PASSWORD" \
    --team-id "$APPLE_TEAM_ID" \
    --wait \
    --timeout "$NOTARIZE_TIMEOUT_SECONDS"; then
  NOTARIZE_PRIMARY_OK=0
fi

if [[ "$NOTARIZE_PRIMARY_OK" == "0" ]]; then
  echo "WARN: notarytool --wait timed out or failed; falling back to polling" >&2
  if [[ "${DRY_RUN:-0}" == "1" ]]; then
    echo "[DRY] (fallback polling path; DRY_RUN skips re-submit + info)"
  else
    SUBMIT_OUT=$(xcrun notarytool submit "$ZIP_PATH" \
      --apple-id "$APPLE_ID_EMAIL" --password "$APPLE_ID_APP_PASSWORD" \
      --team-id "$APPLE_TEAM_ID" 2>&1)
    SUBMISSION_ID=$(echo "$SUBMIT_OUT" \
      | grep -oE 'id: [a-f0-9-]+' | head -1 | cut -d' ' -f2)
    if [[ -z "$SUBMISSION_ID" ]]; then
      echo "ERROR: fallback submit did not return a submission id" >&2
      echo "$SUBMIT_OUT" >&2
      exit 4
    fi
    echo "INFO: fallback submission id: $SUBMISSION_ID"
    FINAL_STATUS=""
    for i in $(seq 1 "$POLL_MAX_ITERATIONS"); do
      sleep "$POLL_SLEEP_SECONDS"
      INFO_OUT=$(xcrun notarytool info "$SUBMISSION_ID" \
        --apple-id "$APPLE_ID_EMAIL" --password "$APPLE_ID_APP_PASSWORD" \
        --team-id "$APPLE_TEAM_ID" 2>&1 || true)
      FINAL_STATUS=$(echo "$INFO_OUT" | awk -F': *' '/^ *status:/ {print $2; exit}')
      echo "INFO: poll iteration $i/${POLL_MAX_ITERATIONS}, status=$FINAL_STATUS"
      if [[ "$FINAL_STATUS" == "Accepted" ]]; then
        break
      fi
      if [[ "$FINAL_STATUS" == "Invalid" || "$FINAL_STATUS" == "Rejected" ]]; then
        echo "ERROR: notarization final verdict: $FINAL_STATUS" >&2
        echo "$INFO_OUT" >&2
        exit 4
      fi
    done
    if [[ "$FINAL_STATUS" != "Accepted" ]]; then
      echo "ERROR: notarization did not reach Accepted within 30 minutes" >&2
      exit 4
    fi
  fi
fi

# --- 6. Staple the notarization ticket to the binary ---
# Stapling attaches the ticket so offline Gatekeeper (no-internet users)
# can verify without re-contacting Apple. `stapler validate` confirms
# the ticket is attached.
run xcrun stapler staple "$IRONLS_BINARY"
if ! run xcrun stapler validate "$IRONLS_BINARY"; then
  echo "ERROR: stapler validate failed" >&2
  exit 5
fi

# --- 7. Re-tar + recompute SHA-256 (Zed extension consumes sidecar) ---
# The Zed extension (editors/zed/src/lib.rs) verifies the downloaded
# tarball against its `.sha256` sidecar (hand-rolled SHA per Phase 6
# 06-05 RESEARCH). Signing changes the binary's bytes, which changes
# the tarball's bytes, which changes the SHA — so we MUST rebuild both.
OUT_DIR="$(cd "$(dirname "$IRONLS_BINARY")" && pwd)"
BIN_BASENAME="$(basename "$IRONLS_BINARY")"
TAR_NAME="ironls-${VERSION_TAG}-macos-${ARCH}.tar.gz"
run tar -czf "$OUT_DIR/$TAR_NAME" -C "$OUT_DIR" "$BIN_BASENAME"
if [[ "${DRY_RUN:-0}" == "1" ]] && [[ ! -f "$OUT_DIR/$TAR_NAME" ]]; then
  POST_SIGN_SHA="dry-run-no-tarball"
else
  POST_SIGN_SHA=$($SHA_CMD "$OUT_DIR/$TAR_NAME" | cut -d' ' -f1)
fi
if [[ "${DRY_RUN:-0}" == "1" ]]; then
  echo "[DRY] echo \"$POST_SIGN_SHA  $TAR_NAME\" > $OUT_DIR/$TAR_NAME.sha256"
else
  echo "$POST_SIGN_SHA  $TAR_NAME" > "$OUT_DIR/$TAR_NAME.sha256"
fi
echo "INFO: post-sign SHA-256: $POST_SIGN_SHA ($TAR_NAME)"

# --- 8. Cleanup ---
run security delete-keychain "$KEYCHAIN"
if [[ "${DRY_RUN:-0}" != "1" ]]; then
  rm -f "$ZIP_PATH"
fi

echo "OK: signed + notarized + stapled: $OUT_DIR/$TAR_NAME"
echo "OK: sidecar: $OUT_DIR/$TAR_NAME.sha256"
