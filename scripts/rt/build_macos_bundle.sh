#!/bin/bash
# build_macos_bundle.sh - build the Iron runtime bundle for a macOS target.
#
# A runtime bundle is what `ironc build --target=<target>` links a program
# against (src/cli/rtbundle.c). For macOS it holds:
#
#   lib/libiron_rt.a             the Iron runtime and stdlib, compiled for
#                                the target against the macOS SDK
#   lib/libSystem.tbd            a text stub for /usr/lib/libSystem.B.dylib
#                                listing only the symbols the runtime uses
#   lib/libclang_rt.builtins.a   the compiler builtins (libclang_rt.osx.a)
#
# ld64.lld links a program against the stub instead of the SDK, so any host
# (Linux, Windows) builds macOS executables; dyld resolves the symbols from
# the real libSystem at run time. arm64 executables are ad-hoc signed by
# the linker.
#
# This script needs a macOS host with the SDK (xcrun --show-sdk-path): the
# runtime is compiled against it here, once, so programs never are. The
# stub lists every symbol the runtime leaves undefined, and a link of the
# whole runtime against the real SDK checks that libSystem exports each.
#
# usage: scripts/rt/build_macos_bundle.sh <target> <iron-version> <out-dir>
#   target is macos-arm64 or macos-x86_64
#
# environment:
#   IRON_TOOLCHAIN   the pinned toolchain bundle to compile with (required)
#   IRON_COMMIT      the short commit hash recorded in the manifest
#                    (default: git rev-parse --short HEAD)
#
# Writes <out-dir>/iron-rt-<version>-<target>/ with lib/ and a rt.txt
# manifest, plus the .tar.gz and its .sha256 line next to it.
set -euo pipefail

if [ $# -ne 3 ]; then
    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
target=$1 version=$2
out=$(mkdir -p "$3" && cd "$3" && pwd)
here=$(cd "$(dirname "$0")/../.." && pwd)
tc=${IRON_TOOLCHAIN:?set IRON_TOOLCHAIN to the pinned toolchain bundle}

# The minimum macOS version matches the target triples in src/cli/target.c.
case "$target" in
    macos-arm64)  triple=arm64-apple-macosx11.0;  arch=arm64;  tbd_target=arm64-macos;;
    macos-x86_64) triple=x86_64-apple-macosx11.0; arch=x86_64; tbd_target=x86_64-macos;;
    *) echo "build_macos_bundle.sh: unknown target $target" >&2; exit 2;;
esac
sdk=$(xcrun --show-sdk-path)
llvm=$(sed -n 's/^llvm //p' "$tc/toolchain.txt")
clang="$tc/bin/clang"
nm="$tc/bin/llvm-nm"
resource=$("$clang" -print-resource-dir)
builtins_src="$resource/lib/darwin/libclang_rt.osx.a"
name="iron-rt-$version-$target"
dest="$out/$name"
work="$out/work-$target"
rm -rf "$dest" "$work"
mkdir -p "$dest/lib" "$work/objs"

# 1. The Iron runtime and stdlib. The same list as the other bundles and as
#    build.c compiles into a native program (raylib and TLS are not part of
#    a bundle yet).
echo "== iron runtime for $triple"
sources=$(cd "$here" && ls src/util/stb_ds_impl.c src/util/arena.c src/util/strbuf.c \
    src/runtime/iron_string.c src/runtime/iron_rc.c src/runtime/iron_builtins.c \
    src/runtime/iron_threads.c src/runtime/iron_collections.c src/runtime/iron_net_init.c \
    src/runtime/iron_oom.c src/runtime/iron_fmt.c src/runtime/iron_heap_track.c \
    src/runtime/iron_panic.c src/runtime/iron_leakcheck.c src/runtime/iron_arena_rt.c \
    src/runtime/iron_os.c \
    src/stdlib/iron_math.c src/stdlib/iron_io.c src/stdlib/iron_time.c src/stdlib/iron_log.c \
    src/stdlib/iron_hint.c src/stdlib/iron_net.c src/stdlib/iron_http.c src/stdlib/iron_tls.c src/stdlib/iron_websocket.c)
for s in $sources; do
    o="$work/objs/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    "$clang" --target="$triple" -isysroot "$sdk" -std=gnu17 -fwrapv -fno-strict-aliasing -O2 \
        -I "$here/src" -I "$here/src/stdlib" -I "$here/src/vendor" -c "$here/$s" -o "$o"
done
rm -f "$dest/lib/libiron_rt.a"
"$tc/bin/llvm-ar" rcs "$dest/lib/libiron_rt.a" "$work"/objs/*.o
cp "$builtins_src" "$dest/lib/libclang_rt.builtins.a"

# 2. The libSystem stub: every symbol the runtime leaves undefined, minus
#    what it and the builtins define. dyld_stub_binder is what ld64 binds
#    lazy symbols through; libSystem always exports it.
echo "== libSystem stub"
"$nm" --defined-only -j "$work"/objs/*.o | sort -u > "$work/defined.txt"
"$nm" --undefined-only -j "$work"/objs/*.o | sort -u > "$work/undefined.txt"
"$nm" --defined-only -j "$builtins_src" 2>/dev/null | sort -u > "$work/builtins.txt"
{ comm -23 "$work/undefined.txt" "$work/defined.txt" | comm -23 - "$work/builtins.txt"
  echo dyld_stub_binder; } | sort -u > "$work/needed.txt"
{
    echo '--- !tapi-tbd'
    echo 'tbd-version: 4'
    echo "targets: [ $tbd_target ]"
    echo "install-name: '/usr/lib/libSystem.B.dylib'"
    echo 'current-version: 1351'
    echo 'exports:'
    echo "  - targets: [ $tbd_target ]"
    echo '    symbols: ['
    sed 's/^/        /; s/$/,/' "$work/needed.txt" | sed '$ s/,$//'
    echo '    ]'
    echo '...'
} > "$dest/lib/libSystem.tbd"
echo "   libSystem.B.dylib: $(wc -l < "$work/needed.txt" | tr -d ' ') symbols"

# Every stub symbol must really be in libSystem: link the whole runtime
# against the SDK (not the stub) and let the linker report what is missing.
cat > "$work/main.c" <<'MAIN'
int main(void) { return 0; }
MAIN
"$clang" --target="$triple" -isysroot "$sdk" -o "$work/sdk-check" "$work/main.c" \
    -Wl,-force_load,"$dest/lib/libiron_rt.a"

# 3. Smoke test: link a program that calls into the runtime with only the
#    bundle (no SDK) and run it when the host can.
echo "== smoke test"
cat > "$work/smoke.c" <<'SMOKE'
#include "runtime/iron_runtime.h"
int main(int argc, char **argv) {
    /* A _Thread_local with an initializer (dyld's thread-local variables). */
    if (iron_stack_gen != 1) return 3;
    iron_runtime_init(argc, argv);
    Iron_println(iron_string_from_literal("bundle ok", 9));
    return 0;
}
SMOKE
"$clang" --target="$triple" -O2 -std=gnu17 -nostdinc -isystem "$resource/include" \
    -I "$here/src" -I "$here/src/stdlib" -c "$work/smoke.c" -o "$work/smoke.o"
"$tc/bin/ld64.lld" -arch "$arch" -platform_version macos 11.0 11.0 -o "$work/smoke" \
    "$work/smoke.o" "$dest/lib/libiron_rt.a" "$dest/lib/libSystem.tbd" "$dest/lib/libclang_rt.builtins.a"
if [ "$(uname -m)" = "$arch" ] || { [ "$arch" = x86_64 ] && arch -x86_64 true 2>/dev/null; }; then
    got=$("$work/smoke")
    [ "$got" = "bundle ok" ] || { echo "build_macos_bundle.sh: smoke test printed '$got'" >&2; exit 1; }
fi

commit=${IRON_COMMIT:-$(cd "$here" && git rev-parse --short HEAD 2>/dev/null || echo unknown)}
cat > "$dest/rt.txt" <<MANIFEST
iron-rt 1
version $version
commit $commit
target $target
triple $triple
libc libSystem
llvm $llvm
MANIFEST

( cd "$out" && tar --uid 0 --gid 0 --no-xattrs --no-mac-metadata -czf "$name.tar.gz" "$name" && \
  (shasum -a 256 "$name.tar.gz" 2>/dev/null || sha256sum "$name.tar.gz") > "$name.tar.gz.sha256" )
echo "== wrote $out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"
