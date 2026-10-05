#!/bin/bash
# build_linux_bundle.sh - build the Iron runtime bundle for a Linux target.
#
# A runtime bundle is what `ironc build --target=<target>` links a program
# against (src/cli/rtbundle.c): the Iron runtime and stdlib compiled for the
# target, a static libc and its start files, and the compiler builtins. The
# generated C itself is freestanding, so a program for the target is built
# on any host from the pinned clang plus this bundle; no SDK is involved.
#
# Linux targets use musl, linked statically, so the binary runs on any
# distribution.
#
# usage: scripts/rt/build_linux_bundle.sh <target> <iron-version> <out-dir>
#   target is linux-x86_64 or linux-arm64
#
# environment:
#   IRON_TOOLCHAIN   the pinned toolchain bundle to compile with (required)
#   MUSL_VERSION     musl release (default 1.2.5)
#   OPENSSL_VERSION  OpenSSL release for TLS (default 3.5.9)
#   LLVM_SRC         llvm-project checkout for compiler-rt (cloned when unset)
#   JOBS             parallel jobs (default: nproc)
#   IRON_COMMIT      the short commit hash recorded in the manifest (default: git rev-parse --short HEAD)
#
# The TLS module is built twice: libiron_tls.a against a static OpenSSL
# (libssl.a, libcrypto.a, shipped too) for programs that import http or
# websocket, and libiron_tls_none.a for the rest.
#
# Writes <out-dir>/iron-rt-<version>-<target>/ with lib/ (libiron_rt.a,
# libc.a, crt1.o, crti.o, crtn.o, libclang_rt.builtins.a) and a rt.txt
# manifest, plus the .tar.gz and its .sha256 line next to it.
set -euo pipefail

if [ $# -ne 3 ]; then
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
target=$1 version=$2
# Absolute: the paths below are passed to tools that run in other
# directories (compiler-rt builds from its own build tree).
out=$(mkdir -p "$3" && cd "$3" && pwd)
musl=${MUSL_VERSION:-1.2.5}
jobs=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
here=$(cd "$(dirname "$0")/../.." && pwd)
tc=${IRON_TOOLCHAIN:?set IRON_TOOLCHAIN to the pinned toolchain bundle}

case "$target" in
    linux-x86_64) arch=x86_64; triple=x86_64-linux-musl;;
    linux-arm64)  arch=aarch64; triple=aarch64-linux-musl;;
    *) echo "build_linux_bundle.sh: unknown target $target" >&2; exit 2;;
esac
llvm=$(sed -n 's/^llvm //p' "$tc/toolchain.txt")
clang="$tc/bin/clang"
resource=$("$clang" -print-resource-dir)
name="iron-rt-$version-$target"
dest="$out/$name"
work="$out/work-$target"
mkdir -p "$dest/lib" "$work"

# 1. musl: headers for compiling the runtime, static libc and start files.
musl_prefix="$work/musl"
if [ ! -f "$musl_prefix/lib/libc.a" ]; then
    echo "== musl $musl"
    rm -rf "$work/musl-$musl"
    curl -fsSL "https://musl.libc.org/releases/musl-$musl.tar.gz" | tar -xz -C "$work"
    ( cd "$work/musl-$musl" &&
      CC="$clang --target=$triple" AR="$tc/bin/llvm-ar" RANLIB="$tc/bin/llvm-ranlib" \
      LIBCC="" ./configure --prefix="$musl_prefix" --target="$triple" --disable-shared >/dev/null &&
      make -j "$jobs" >/dev/null && make install >/dev/null )
fi

# 2. compiler-rt builtins for the target (the toolchain bundle only carries
#    the host's). Only the builtins library: no sanitizers, no profile.
if [ ! -f "$work/builtins/lib/linux/libclang_rt.builtins-$arch.a" ] && \
   [ ! -f "$work/builtins/lib/$triple/libclang_rt.builtins.a" ]; then
    echo "== compiler-rt builtins $llvm for $triple"
    src=${LLVM_SRC:-$work/llvm-project}
    if [ ! -d "$src/compiler-rt" ]; then
        git clone --depth 1 --branch "llvmorg-$llvm" --filter=blob:none --sparse \
            https://github.com/llvm/llvm-project.git "$src" >/dev/null 2>&1
        ( cd "$src" && git sparse-checkout set compiler-rt cmake third-party >/dev/null 2>&1 )
    fi
    cmake -S "$src/compiler-rt/lib/builtins" -B "$work/builtins-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$clang" -DCMAKE_ASM_COMPILER="$clang" -DCMAKE_CXX_COMPILER="$clang" \
        -DCMAKE_C_COMPILER_TARGET="$triple" -DCMAKE_ASM_COMPILER_TARGET="$triple" \
        -DCMAKE_CXX_COMPILER_TARGET="$triple" \
        -DCMAKE_SYSROOT="$musl_prefix" -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR="$arch" \
        -DCMAKE_AR="$tc/bin/llvm-ar" -DCMAKE_RANLIB="$tc/bin/llvm-ranlib" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCOMPILER_RT_BAREMETAL_BUILD=OFF -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
        -DCMAKE_INSTALL_PREFIX="$work/builtins" > "$work/builtins-configure.log" 2>&1 \
        || { cat "$work/builtins-configure.log"; exit 1; }
    cmake --build "$work/builtins-build" -j "$jobs" > "$work/builtins-build.log" 2>&1 \
        || { tail -40 "$work/builtins-build.log"; exit 1; }
    cmake --install "$work/builtins-build" >/dev/null
fi
builtins=$(find "$work/builtins/lib" -name 'libclang_rt.builtins*.a' | head -1)
[ -n "$builtins" ] || { echo "build_linux_bundle.sh: builtins library missing" >&2; exit 1; }

# 3. OpenSSL, static, against the same musl. OPENSSLDIR only seeds the
#    default trust path; iron_tls.c also probes the distributions' CA
#    bundles. no-async: it needs ucontext, which musl lacks.
openssl=${OPENSSL_VERSION:-3.5.9}
ossl_prefix="$work/openssl"
if [ ! -f "$ossl_prefix/lib/libssl.a" ] && [ ! -f "$ossl_prefix/lib64/libssl.a" ]; then
    echo "== openssl $openssl"
    rm -rf "$work/openssl-$openssl"
    curl -fsSL "https://github.com/openssl/openssl/releases/download/openssl-$openssl/openssl-$openssl.tar.gz" | tar -xz -C "$work"
    case "$arch" in x86_64) ossl_target=linux-x86_64;; aarch64) ossl_target=linux-aarch64;; esac
    ( cd "$work/openssl-$openssl" &&
      CC="$clang --target=$triple --sysroot=$musl_prefix" AR="$tc/bin/llvm-ar" RANLIB="$tc/bin/llvm-ranlib" \
      ./Configure "$ossl_target" no-shared no-tests no-docs no-apps no-dso no-engine no-async \
          no-afalgeng no-ktls no-module no-secure-memory -fPIC --prefix="$ossl_prefix" --openssldir=/etc/ssl >/dev/null &&
      make -j "$jobs" build_libs >/dev/null && make install_dev >/dev/null )
fi
ossl_lib=$(dirname "$(find "$ossl_prefix" -name libssl.a | head -1)")

# 4. The Iron runtime and stdlib, compiled against musl for the target.
#    Mirrors the source list build.c compiles into every program (raylib,
#    which needs X11, is not part of a Linux bundle).
echo "== iron runtime for $triple"
objs="$work/rt-objs"
rm -rf "$objs" && mkdir -p "$objs"
sources=$(cd "$here" && ls src/util/stb_ds_impl.c src/util/arena.c src/util/strbuf.c \
    src/runtime/iron_string.c src/runtime/iron_rc.c src/runtime/iron_builtins.c \
    src/runtime/iron_threads.c src/runtime/iron_collections.c src/runtime/iron_net_init.c \
    src/runtime/iron_oom.c src/runtime/iron_fmt.c src/runtime/iron_heap_track.c \
    src/runtime/iron_panic.c src/runtime/iron_leakcheck.c src/runtime/iron_arena_rt.c \
    src/runtime/iron_os.c \
    src/stdlib/iron_math.c src/stdlib/iron_io.c src/stdlib/iron_time.c src/stdlib/iron_log.c \
    src/stdlib/iron_hint.c src/stdlib/iron_net.c src/stdlib/iron_http.c src/stdlib/iron_websocket.c)
cflags=(--target="$triple" --sysroot="$musl_prefix" -std=gnu17 -fwrapv -fno-strict-aliasing
        -O2 -fPIC -I "$here/src" -I "$here/src/stdlib" -I "$here/src/vendor" -D_GNU_SOURCE)
for s in $sources; do
    o="$objs/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    "$clang" "${cflags[@]}" -c "$here/$s" -o "$o"
done
rm -f "$dest/lib/libiron_rt.a" "$dest/lib/libiron_tls.a" "$dest/lib/libiron_tls_none.a"
"$tc/bin/llvm-ar" rcs "$dest/lib/libiron_rt.a" "$objs"/*.o
"$clang" "${cflags[@]}" -c "$here/src/stdlib/iron_tls.c" -o "$work/iron_tls_none.o"
"$clang" "${cflags[@]}" -DIRON_HAVE_OPENSSL=1 -I "$ossl_prefix/include" \
    -c "$here/src/stdlib/iron_tls.c" -o "$work/iron_tls.o"
"$tc/bin/llvm-ar" rcs "$dest/lib/libiron_tls_none.a" "$work/iron_tls_none.o"
"$tc/bin/llvm-ar" rcs "$dest/lib/libiron_tls.a" "$work/iron_tls.o"
cp "$ossl_lib/libssl.a" "$ossl_lib/libcrypto.a" "$dest/lib/"

cp "$musl_prefix/lib/libc.a" "$musl_prefix/lib/crt1.o" "$musl_prefix/lib/crti.o" "$musl_prefix/lib/crtn.o" "$dest/lib/"
cp "$builtins" "$dest/lib/libclang_rt.builtins.a"

# The commit the runtime was compiled from: a program's generated C and the
# runtime must agree on every type and symbol, so ironc refuses a bundle
# from another commit (IRON_GIT_HASH, as CMakeLists.txt computes it).
commit=${IRON_COMMIT:-$(cd "$here" && git rev-parse --short HEAD 2>/dev/null || echo unknown)}
cat > "$dest/rt.txt" <<MANIFEST
iron-rt 1
version $version
commit $commit
target $target
triple $triple
libc musl $musl
tls openssl $openssl
llvm $llvm
MANIFEST

( cd "$out" && tar -czf "$name.tar.gz" "$name" && \
  (shasum -a 256 "$name.tar.gz" 2>/dev/null || sha256sum "$name.tar.gz") > "$name.tar.gz.sha256" )
echo "== wrote $out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"
