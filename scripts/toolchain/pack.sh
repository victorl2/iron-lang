#!/bin/bash
# pack.sh - assemble an Iron toolchain bundle from an LLVM install prefix.
#
# The bundle is what `ironc` looks for (src/cli/toolchain.c): a directory
# with bin/ (clang, lld and the archivers), lib/clang/<major>/ (the
# resource headers and compiler-rt runtimes) and a toolchain.txt manifest.
# The toolchain workflow runs this after building LLVM; developers can run
# it on a local LLVM to get a bundle for IRON_TOOLCHAIN.
#
# usage: scripts/toolchain/pack.sh <llvm-prefix> <llvm-version> <bundle> <host> <out-dir>
#   host is one of macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64
#
# Writes <out-dir>/iron-toolchain-<llvm>-<bundle>-<host>/ and, next to it,
# iron-toolchain-<llvm>-<bundle>-<host>.tar.gz and its .sha256 line.
set -euo pipefail

if [ $# -ne 5 ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
prefix=$1 llvm=$2 bundle=$3 host=$4 out=$5
name="iron-toolchain-$llvm-$bundle-$host"
dest="$out/$name"
exe=""
case "$host" in windows-*) exe=".exe";; esac

rm -rf "$dest"
mkdir -p "$dest/bin" "$dest/lib"

# Tools. clang and lld are usually symlinks to clang-<major> and lld; copy
# the real files once and recreate the links (copies on Windows).
missing=""
copy_tool() {   # copy_tool <name> [required]
    local n=$1 required=${2:-yes}
    local src="$prefix/bin/$n$exe"
    if [ ! -e "$src" ]; then
        [ "$required" = yes ] && { echo "pack.sh: $src is missing" >&2; exit 1; }
        missing="$missing $n"
        return 0
    fi
    if [ -L "$src" ] && [ "$exe" = "" ]; then
        local target
        target=$(readlink "$src")
        target=$(basename "$target")
        [ -e "$dest/bin/$target" ] || cp -f "$prefix/bin/$target" "$dest/bin/$target"
        ln -sf "$target" "$dest/bin/$n"
    else
        cp -f "$src" "$dest/bin/$n$exe"
    fi
}
copy_tool clang
copy_tool clang-cl no
copy_tool lld no
copy_tool ld.lld no
copy_tool ld64.lld no
copy_tool lld-link no
copy_tool wasm-ld no
copy_tool llvm-ar
copy_tool llvm-ranlib no
copy_tool llvm-dlltool no
copy_tool llvm-lib no
copy_tool llvm-nm no
copy_tool llvm-objcopy no
copy_tool llvm-strip no

# Resource directory: headers and runtimes, lib/clang/<major>/ (one entry).
resdir=""
for d in "$prefix"/lib/clang/*/; do
    [ -d "$d" ] || continue
    [ -z "$resdir" ] || { echo "pack.sh: more than one resource directory under $prefix/lib/clang" >&2; exit 1; }
    resdir=${d%/}
done
[ -n "$resdir" ] || { echo "pack.sh: no resource directory under $prefix/lib/clang" >&2; exit 1; }
mkdir -p "$dest/lib/clang"
cp -R "$resdir" "$dest/lib/clang/$(basename "$resdir")"

# Shared LLVM libraries, when the install links its tools against them
# (Homebrew does; our CI builds are static and have none).
for lib in "$prefix"/lib/libLLVM*.dylib "$prefix"/lib/libLLVM*.so* "$prefix"/lib/libclang-cpp*.dylib "$prefix"/lib/libclang-cpp*.so*; do
    [ -e "$lib" ] && cp -R "$lib" "$dest/lib/"
done

cat > "$dest/toolchain.txt" <<EOF
iron-toolchain 1
llvm $llvm
bundle $bundle
host $host
EOF

( cd "$out" && tar -czf "$name.tar.gz" "$name" )
if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
else
    ( cd "$out" && shasum -a 256 "$name.tar.gz" > "$name.tar.gz.sha256" )
fi
du -sh "$dest" "$out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"
# A release bundle must have every tool; the workflow checks this line is absent.
[ -z "$missing" ] || echo "pack.sh: optional tools not in $prefix:$missing"
