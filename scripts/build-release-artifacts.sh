#!/usr/bin/env bash
# scripts/build-release-artifacts.sh
# Phase 36 Plan 04 (REL-11) — assemble the v4.0.0-alpha release
# artifact bundle into dist/.
#
# Produces (depending on host platform):
#   Linux x86_64:  dist/ironls-v<VERSION>-linux-x86_64.tar.gz   + .sha256
#   macOS arm64:   dist/ironls-v<VERSION>-macos-arm64.tar.gz    + .sha256
#                  (after invoking scripts/sign-and-notarize-macos.sh)
#   macOS x86_64:  dist/ironls-v<VERSION>-macos-x86_64.tar.gz   + .sha256
#                  (after invoking scripts/sign-and-notarize-macos.sh)
#   Any platform with Node + npm:
#                  dist/iron-lsp-<VERSION>.vsix                 + .sha256
#   If grammars/tree-sitter/iron/tree-sitter-iron.wasm exists:
#                  dist/iron.wasm                               + .sha256
#
# Always emits:
#   dist/MANIFEST.txt                  — sorted list of every file in dist/.
#
# Usage:
#   bash scripts/build-release-artifacts.sh             # auto-detect host
#   bash scripts/build-release-artifacts.sh --linux-only  # skip macOS sign+notarize path
#   bash scripts/build-release-artifacts.sh --help
#
# Exit codes:
#   0  All artifacts for the host platform produced successfully.
#   1  A required step failed.
#   2  Wrong host platform for the requested mode.

set -euo pipefail

# ---------------------------------------------------------------------------
# Args / help.
# ---------------------------------------------------------------------------
LINUX_ONLY=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --linux-only)
      LINUX_ONLY=1
      shift
      ;;
    -h|--help)
      cat <<'EOF'
Usage: bash scripts/build-release-artifacts.sh [--linux-only]

Builds the v4.0.0-alpha release artifact bundle into dist/.

Default: auto-detects host platform via `uname -s`/`uname -m` and runs the
corresponding pipeline (Linux x86_64, macOS arm64, or macOS x86_64).

--linux-only  Force the Linux x86_64 path even on macOS (skips sign+notarize).
              Useful when iterating on the script from a Mac without Apple
              secrets in the shell.

Prints the suggested `gh release upload` command on success.
EOF
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      echo "Run with --help for usage." >&2
      exit 2
      ;;
  esac
done

# ---------------------------------------------------------------------------
# Locate repo root.
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

# ---------------------------------------------------------------------------
# Read IRON_VERSION_FULL from CMakeLists.txt.
# Expected: 4.0.0-alpha
# ---------------------------------------------------------------------------
VERSION=$(grep -oE 'IRON_VERSION_FULL "[^"]+"' CMakeLists.txt \
            | head -1 | sed 's/.*"\([^"]*\)"/\1/')
if [ -z "${VERSION}" ]; then
  echo "ERROR: could not parse IRON_VERSION_FULL from CMakeLists.txt." >&2
  exit 1
fi
echo "Iron version: ${VERSION}"

# ---------------------------------------------------------------------------
# Detect host platform (overridable by --linux-only).
# ---------------------------------------------------------------------------
HOST_OS="$(uname -s)"
HOST_ARCH="$(uname -m)"

if [ "${LINUX_ONLY}" = "1" ]; then
  TARGET_OS="Linux"
  TARGET_ARCH="x86_64"
else
  TARGET_OS="${HOST_OS}"
  TARGET_ARCH="${HOST_ARCH}"
fi

echo "Host:   ${HOST_OS} ${HOST_ARCH}"
echo "Target: ${TARGET_OS} ${TARGET_ARCH}"

# Choose a portable sha256 binary.
if command -v shasum >/dev/null 2>&1; then
  SHA256="shasum -a 256"
elif command -v sha256sum >/dev/null 2>&1; then
  SHA256="sha256sum"
else
  echo "ERROR: neither shasum nor sha256sum found on PATH." >&2
  exit 1
fi

mkdir -p dist

# ---------------------------------------------------------------------------
# Helper: write a .sha256 sidecar in the conventional "<hash>  <basename>" form.
# ---------------------------------------------------------------------------
write_sha256_sidecar() {
  local path="$1"
  local dir
  dir="$(dirname "${path}")"
  local base
  base="$(basename "${path}")"
  (cd "${dir}" && ${SHA256} "${base}" > "${base}.sha256")
}

# ---------------------------------------------------------------------------
# Build ironls (cmake + ninja or make).
# ---------------------------------------------------------------------------
build_ironls() {
  echo ""
  echo "============================================================"
  echo "Building ironls (Release, IRON_BUILD_LSP=ON)..."
  echo "============================================================"
  local generator=""
  if command -v ninja >/dev/null 2>&1; then
    generator="-G Ninja"
  fi

  cmake -S . -B build ${generator} -DCMAKE_BUILD_TYPE=Release -DIRON_BUILD_LSP=ON
  local jobs
  if command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
  else
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  fi
  cmake --build build -j"${jobs}" --target ironls
}

# ---------------------------------------------------------------------------
# Linux x86_64 path.
# ---------------------------------------------------------------------------
build_linux_x86_64() {
  build_ironls
  local stage="dist/ironls-v${VERSION}-linux-x86_64"
  rm -rf "${stage}"
  mkdir -p "${stage}"
  cp build/ironls "${stage}/ironls"
  chmod +x "${stage}/ironls"

  local tarball="dist/ironls-v${VERSION}-linux-x86_64.tar.gz"
  tar -czf "${tarball}" -C dist "ironls-v${VERSION}-linux-x86_64"
  write_sha256_sidecar "${tarball}"
  echo "Produced: ${tarball}"
  echo "Produced: ${tarball}.sha256"
}

# ---------------------------------------------------------------------------
# macOS path (arm64 or x86_64).
# ---------------------------------------------------------------------------
build_macos() {
  local arch_label="$1"   # arm64 or x86_64
  build_ironls

  local stage="dist/ironls-v${VERSION}-macos-${arch_label}"
  rm -rf "${stage}"
  mkdir -p "${stage}"
  cp build/ironls "${stage}/ironls"
  chmod +x "${stage}/ironls"

  echo ""
  echo "Invoking scripts/sign-and-notarize-macos.sh ${stage}/ironls..."
  # The sign script fails fast if Apple env vars are unset.
  bash scripts/sign-and-notarize-macos.sh "${stage}/ironls"

  local tarball="dist/ironls-v${VERSION}-macos-${arch_label}.tar.gz"
  tar -czf "${tarball}" -C dist "ironls-v${VERSION}-macos-${arch_label}"
  write_sha256_sidecar "${tarball}"
  echo "Produced: ${tarball}"
  echo "Produced: ${tarball}.sha256"
}

# ---------------------------------------------------------------------------
# VSIX bundling (any platform with Node + npm).
# ---------------------------------------------------------------------------
build_vsix() {
  echo ""
  echo "============================================================"
  echo "Building VSCode .vsix (npm + vsce)..."
  echo "============================================================"
  if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
    echo "SKIP: node/npm not found on PATH; .vsix will not be produced."
    echo "      Install Node >= 18 to bundle the VSIX, or run vsce package"
    echo "      manually per docs/dev/PHASE-36-VSCE-PUBLISH-PLAYBOOK.md."
    return 0
  fi

  (
    cd editors/vscode
    npm install
    npm run prepackage
    npx --yes @vscode/vsce package --pre-release --no-dependencies
  )

  local vsix_src="editors/vscode/iron-lsp-${VERSION}.vsix"
  local vsix_dst="dist/iron-lsp-${VERSION}.vsix"
  if [ ! -f "${vsix_src}" ]; then
    echo "ERROR: expected ${vsix_src} after vsce package, not found." >&2
    return 1
  fi
  mv "${vsix_src}" "${vsix_dst}"
  write_sha256_sidecar "${vsix_dst}"
  echo "Produced: ${vsix_dst}"
  echo "Produced: ${vsix_dst}.sha256"
}

# ---------------------------------------------------------------------------
# Tree-sitter wasm passthrough (if present).
# ---------------------------------------------------------------------------
copy_treesitter_wasm() {
  local src="grammars/tree-sitter/iron/tree-sitter-iron.wasm"
  if [ ! -f "${src}" ]; then
    echo "SKIP: ${src} not present; iron.wasm will not be in the bundle."
    return 0
  fi
  local dst="dist/iron.wasm"
  cp "${src}" "${dst}"
  write_sha256_sidecar "${dst}"
  echo "Produced: ${dst}"
  echo "Produced: ${dst}.sha256"
}

# ---------------------------------------------------------------------------
# Pipeline dispatch.
# ---------------------------------------------------------------------------
case "${TARGET_OS}-${TARGET_ARCH}" in
  Linux-x86_64)
    build_linux_x86_64
    ;;
  Darwin-arm64)
    build_macos arm64
    ;;
  Darwin-x86_64)
    build_macos x86_64
    ;;
  *)
    echo "ERROR: unsupported host: ${TARGET_OS} ${TARGET_ARCH}" >&2
    echo "       Supported: Linux x86_64, Darwin arm64, Darwin x86_64." >&2
    exit 2
    ;;
esac

build_vsix
copy_treesitter_wasm

# ---------------------------------------------------------------------------
# Manifest + upload hint.
# ---------------------------------------------------------------------------
find dist -type f | sort > dist/MANIFEST.txt
echo ""
echo "============================================================"
echo "dist/MANIFEST.txt:"
echo "============================================================"
cat dist/MANIFEST.txt

echo ""
echo "============================================================"
echo "Suggested upload (run after gh release create v${VERSION}):"
echo "============================================================"
echo "  gh release upload v${VERSION} \\"
# Print every artifact path on its own continued line.
while read -r f; do
  echo "    ${f} \\"
done < <(grep -vE '/MANIFEST\.txt$' dist/MANIFEST.txt)
echo "    --clobber"
echo ""
echo "Done."
