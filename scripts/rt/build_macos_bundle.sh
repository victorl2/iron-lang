#!/bin/bash
# build_macos_bundle.sh - build the Iron runtime bundle for a macOS target.
#
# A runtime bundle is what `ironc build --target=<target>` links a program
# against (src/cli/rtbundle.c). For macOS it holds:
#
#   lib/libiron_rt.a             the Iron runtime and stdlib, compiled for
#                                the target against the macOS SDK
#   lib/libSystem.tbd            a text stub for /usr/lib/libSystem.B.dylib
#                                listing only the symbols the bundle uses
#   lib/libraylib.a              raylib (GLFW on Cocoa, OpenGL) and Iron's
#                                raylib shim, for programs that import raylib
#   lib/<Framework>.tbd, lib/frameworks.txt
#                                stubs for the frameworks and libraries raylib
#                                uses (AppKit, OpenGL, IOKit, libobjc, ...),
#                                and the list ironc links for a raylib program
#   lib/libclang_rt.builtins.a   the compiler builtins (libclang_rt.osx.a)
#   lib/libiron_tls.a, libssl.a, libcrypto.a
#                                the TLS module against a static OpenSSL, for
#                                programs that import http or websocket
#   lib/libiron_tls_none.a       the TLS module without OpenSSL, for the rest
#
# ld64.lld links a program against the stub instead of the SDK, so any host
# (Linux, Windows) builds macOS executables; dyld resolves the symbols from
# the real libSystem at run time. arm64 executables are ad-hoc signed by
# the linker.
#
# This script needs a macOS host with the SDK (xcrun --show-sdk-path): the
# runtime is compiled against it here, once, so programs never are. The
# stubs list every symbol the bundle leaves undefined, each attributed to
# the SDK library that exports it (scripts/rt/macos_stubs.py), and a link of
# the runtime against the real SDK checks libSystem once more.
#
# usage: scripts/rt/build_macos_bundle.sh <target> <iron-version> <out-dir>
#   target is macos-arm64 or macos-x86_64
#
# environment:
#   IRON_TOOLCHAIN   the pinned toolchain bundle to compile with (required)
#   IRON_COMMIT      the short commit hash recorded in the manifest
#                    (default: git rev-parse --short HEAD)
#   OPENSSL_VERSION  OpenSSL release for TLS (default 3.5.9)
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

# 1. OpenSSL, static, built once into a cache next to the output.
openssl=${OPENSSL_VERSION:-3.5.9}
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
cache="$out/cache-$target"
ossl_prefix="$cache/openssl-$openssl-install"
if [ ! -f "$ossl_prefix/lib/libssl.a" ]; then
    echo "== openssl $openssl"
    mkdir -p "$cache"
    rm -rf "$cache/openssl-$openssl"
    curl -fsSL "https://github.com/openssl/openssl/releases/download/openssl-$openssl/openssl-$openssl.tar.gz" \
        | tar -xz -C "$cache"
    case "$arch" in arm64) ossl_target=darwin64-arm64-cc;; x86_64) ossl_target=darwin64-x86_64-cc;; esac
    ( cd "$cache/openssl-$openssl" &&
      CC="$clang --target=$triple -isysroot $sdk" AR="$tc/bin/llvm-ar" RANLIB="$tc/bin/llvm-ranlib" \
      ./Configure "$ossl_target" no-shared no-tests no-docs no-apps no-dso no-engine no-module no-asm \
          --prefix="$ossl_prefix" --openssldir=/etc/ssl > "$cache/configure.log" &&
      make -j "$jobs" build_libs > "$cache/build.log" && make install_dev > "$cache/install.log" ) \
        || { tail -30 "$cache/configure.log" "$cache/build.log" 2>/dev/null; exit 1; }
fi

# 2. The Iron runtime and stdlib. The same list as the other bundles and as
#    build.c compiles into a native program (raylib is not part of a bundle
#    yet).
echo "== iron runtime for $triple"
sources=$(cd "$here" && ls src/util/stb_ds_impl.c src/util/arena.c src/util/strbuf.c \
    src/runtime/iron_string.c src/runtime/iron_rc.c src/runtime/iron_builtins.c \
    src/runtime/iron_threads.c src/runtime/iron_collections.c src/runtime/iron_net_init.c \
    src/runtime/iron_oom.c src/runtime/iron_fmt.c src/runtime/iron_heap_track.c \
    src/runtime/iron_panic.c src/runtime/iron_leakcheck.c src/runtime/iron_arena_rt.c \
    src/runtime/iron_os.c \
    src/stdlib/iron_math.c src/stdlib/iron_io.c src/stdlib/iron_time.c src/stdlib/iron_log.c \
    src/stdlib/iron_hint.c src/stdlib/iron_net.c src/stdlib/iron_http.c src/stdlib/iron_websocket.c)
cflags=(--target="$triple" -isysroot "$sdk" -std=gnu17 -fwrapv -fno-strict-aliasing -O2
        -I "$here/src" -I "$here/src/stdlib" -I "$here/src/vendor")
for s in $sources; do
    o="$work/objs/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    "$clang" "${cflags[@]}" -c "$here/$s" -o "$o"
done
rm -f "$dest/lib/libiron_rt.a"
"$tc/bin/llvm-ar" rcs "$dest/lib/libiron_rt.a" "$work"/objs/*.o
cp "$builtins_src" "$dest/lib/libclang_rt.builtins.a"
mkdir -p "$work/tls"
"$clang" "${cflags[@]}" -c "$here/src/stdlib/iron_tls.c" -o "$work/tls/iron_tls_none.o"
"$clang" "${cflags[@]}" -DIRON_HAVE_OPENSSL=1 -I "$ossl_prefix/include" \
    -c "$here/src/stdlib/iron_tls.c" -o "$work/tls/iron_tls.o"
"$tc/bin/llvm-ar" rcs "$dest/lib/libiron_tls_none.a" "$work/tls/iron_tls_none.o"
"$tc/bin/llvm-ar" rcs "$dest/lib/libiron_tls.a" "$work/tls/iron_tls.o"
cp "$ossl_prefix/lib/libssl.a" "$ossl_prefix/lib/libcrypto.a" "$dest/lib/"

# raylib: each source is its own translation unit, as build.c compiles
# them; GLFW's Cocoa backend is Objective-C. Selector and class stubs
# (objc_msgSend$sel) are off so the objects call objc_msgSend itself
# instead of leaving stubs for the linker to synthesize.
echo "== raylib"
mkdir -p "$work/raylib"
rl_flags=(--target="$triple" -isysroot "$sdk" -std=gnu17 -O2 -w -DPLATFORM_DESKTOP
          -fno-objc-msgsend-selector-stubs -fno-objc-msgsend-class-selector-stubs
          -I "$here/src" -I "$here/src/stdlib" -I "$here/src/vendor"
          -I "$here/src/vendor/raylib" -I "$here/src/vendor/raylib/external/glfw/include")
for s in src/vendor/raylib/rcore.c src/vendor/raylib/rshapes.c src/vendor/raylib/rtextures.c \
         src/vendor/raylib/rtext.c src/vendor/raylib/rmodels.c src/vendor/raylib/raudio.c \
         src/vendor/raylib/rglfw.c src/stdlib/iron_raylib.c src/stdlib/iron_raylib_layout.c; do
    lang=()
    case "$s" in *rglfw.c) lang=(-xobjective-c);; esac
    "$clang" "${rl_flags[@]}" ${lang[@]+"${lang[@]}"} -c "$here/$s" -o "$work/raylib/$(basename "$s" .c).o"
done
"$tc/bin/llvm-ar" rcs "$dest/lib/libraylib.a" "$work"/raylib/*.o

# 3. Link stubs: every symbol the bundle leaves undefined, minus what it
#    and the builtins define, attributed to the SDK library that exports
#    it. libSystem.tbd serves every program; the others are raylib's.
echo "== link stubs"
inputs=("$work"/objs/*.o "$work"/tls/*.o "$work"/raylib/*.o "$dest/lib/libssl.a" "$dest/lib/libcrypto.a")
"$nm" --defined-only -j "${inputs[@]}" 2>/dev/null | sort -u > "$work/defined.txt"
"$nm" --undefined-only -j "${inputs[@]}" 2>/dev/null | sort -u > "$work/undefined.txt"
"$nm" --defined-only -j "$builtins_src" 2>/dev/null | sort -u > "$work/builtins.txt"
{ comm -23 "$work/undefined.txt" "$work/defined.txt" | comm -23 - "$work/builtins.txt"
  echo dyld_stub_binder; } | sort -u > "$work/needed.txt"
python3 "$here/scripts/rt/macos_stubs.py" "$sdk" "$arch" "$work/needed.txt" "$work/stubs" > "$work/stubs.txt"
sed 's/^/   /' "$work/stubs.txt"
cp "$work"/stubs/*.tbd "$dest/lib/"
[ -f "$dest/lib/libSystem.tbd" ] || { echo "build_macos_bundle.sh: no libSystem stub" >&2; exit 1; }
awk '$1 != "libSystem.tbd" {print $1}' "$work/stubs.txt" > "$dest/lib/frameworks.txt"

# Every stub symbol must really be in libSystem: link the whole runtime
# against the SDK (not the stub) and let the linker report what is missing.
cat > "$work/main.c" <<'MAIN'
int main(void) { return 0; }
MAIN
"$clang" --target="$triple" -isysroot "$sdk" -o "$work/sdk-check" "$work/main.c" \
    -Wl,-force_load,"$dest/lib/libiron_rt.a" -Wl,-force_load,"$dest/lib/libiron_tls.a" \
    "$dest/lib/libssl.a" "$dest/lib/libcrypto.a"

# 4. Smoke test: link a program that calls into the runtime with only the
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
    "$work/smoke.o" "$dest/lib/libiron_rt.a" "$dest/lib/libiron_tls_none.a" "$dest/lib/libSystem.tbd" "$dest/lib/libclang_rt.builtins.a"
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
tls openssl $openssl
llvm $llvm
MANIFEST

( cd "$out" && tar --uid 0 --gid 0 --no-xattrs --no-mac-metadata -czf "$name.tar.gz" "$name" && \
  (shasum -a 256 "$name.tar.gz" 2>/dev/null || sha256sum "$name.tar.gz") > "$name.tar.gz.sha256" )
echo "== wrote $out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"
