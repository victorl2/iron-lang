#!/bin/bash
# install.sh - fetch the pinned toolchain bundle into ~/.iron/toolchain, the
# way ironc does on first use, without needing ironc built first. CI runs
# this (through .github/actions/toolchain) before building Iron programs;
# users normally let ironc do it or run `iron toolchain install`.
#
# usage: scripts/toolchain/install.sh [dest-dir]
#   The version and checksums come from src/cli/toolchain_pins.h.
#   IRON_TOOLCHAIN_URL overrides the download base URL.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
pins=$root/src/cli/toolchain_pins.h
llvm=$(sed -n 's/^#define IRON_TOOLCHAIN_LLVM *"\(.*\)".*/\1/p' "$pins")
bundle=$(sed -n 's/^#define IRON_TOOLCHAIN_BUNDLE *"\(.*\)".*/\1/p' "$pins")
version="$llvm-$bundle"

case "$(uname -s):$(uname -m)" in
    Darwin:arm64)                      host=macos-arm64;;
    Darwin:x86_64)                     host=macos-x86_64;;
    Linux:x86_64)                      host=linux-x86_64;;
    Linux:aarch64|Linux:arm64)         host=linux-arm64;;
    MINGW*|MSYS*|CYGWIN*)              host=windows-x86_64;;
    *) echo "install.sh: unsupported host $(uname -s) $(uname -m)" >&2; exit 1;;
esac
sha=$(sed -n "s/.*{ *\"$host\", *\"\([0-9a-f]*\)\" *}.*/\1/p" "$pins")
if [ -z "$sha" ]; then
    echo "install.sh: no bundle is published for $host (llvm $llvm bundle $bundle)" >&2
    exit 1
fi

home=${HOME:-${USERPROFILE:-}}
dest=${1:-$home/.iron/toolchain/$version}
if [ -f "$dest/toolchain.txt" ] && grep -q "^llvm $llvm\$" "$dest/toolchain.txt" && grep -q "^bundle $bundle\$" "$dest/toolchain.txt"; then
    echo "toolchain llvm $llvm bundle $bundle already at $dest"
    exit 0
fi

name="iron-toolchain-$version-$host.tar.gz"
base=${IRON_TOOLCHAIN_URL:-https://github.com/victorl2/iron-lang/releases/download/toolchain-$version/}
parent=$(dirname "$dest")
mkdir -p "$parent"
archive="$parent/$name"
echo "downloading $base$name"
curl -fL --retry 3 -o "$archive" "$base$name"
if command -v sha256sum >/dev/null 2>&1; then got=$(sha256sum "$archive" | cut -d' ' -f1)
else got=$(shasum -a 256 "$archive" | cut -d' ' -f1); fi
if [ "$got" != "$sha" ]; then
    echo "install.sh: checksum mismatch for $name: expected $sha, got $got" >&2
    rm -f "$archive"
    exit 1
fi
staging="$parent/.staging-$version"
rm -rf "$staging"; mkdir -p "$staging"
tar -xzf "$archive" -C "$staging" --strip-components=1
rm -f "$archive"
rm -rf "$dest"
mv "$staging" "$dest"
echo "toolchain llvm $llvm bundle $bundle installed at $dest"
