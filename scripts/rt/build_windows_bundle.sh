#!/bin/bash
# build_windows_bundle.sh - build the Iron runtime bundle for windows-x86_64.
#
# A runtime bundle is what `ironc build --target=<target>` links a program
# against (src/cli/rtbundle.c). For Windows it holds:
#
#   lib/iron_rt.lib          the Iron runtime and stdlib, compiled for
#                            x86_64-pc-windows-msvc against the UCRT
#   lib/iron_crt0.obj        the entry point and the few symbols vcruntime
#                            would supply (src/runtime/iron_win_crt0.c)
#   lib/<dll>.lib            import libraries for the system DLLs the runtime
#                            calls (ucrtbase, kernel32, ws2_32, bcrypt), made
#                            with llvm-dlltool from the .def files beside them
#   lib/clang_rt.builtins.lib  the compiler builtins
#   lib/iron_tls.lib, libssl.lib, libcrypto.lib
#                            the TLS module against a static OpenSSL, for
#                            programs that import http or websocket
#   lib/iron_tls_none.lib    the TLS module without OpenSSL, for the rest
#   lib/raylib.lib           raylib (GLFW on Win32, OpenGL) and Iron's raylib
#                            shim, for programs that import raylib
#
# Every DLL named there ships with Windows 10 and later, so a program links
# with lld-link and runs without the Visual Studio Build Tools or the
# Visual C++ redistributable.
#
# This script needs a Windows host with the Windows SDK and MSVC headers in
# INCLUDE (a Developer prompt, or ilammy/msvc-dev-cmd in CI) and dumpbin on
# PATH: the runtime is compiled against the SDK here, once, so programs
# never are. The .def files list only the symbols the runtime and the entry
# point import, each checked against the DLL's export table.
#
# usage: scripts/rt/build_windows_bundle.sh <target> <iron-version> <out-dir>
#   target is windows-x86_64
#
# environment:
#   IRON_TOOLCHAIN   the pinned toolchain bundle to compile with (required)
#   IRON_COMMIT      the short commit hash recorded in the manifest
#                    (default: git rev-parse --short HEAD)
#   OPENSSL_VERSION  OpenSSL release for TLS (default 3.5.9)
#   PERL             a native Windows perl for OpenSSL's Configure
#                    (default: Strawberry Perl in C:\Strawberry)
#
# Writes <out-dir>/iron-rt-<version>-<target>/ with lib/ and a rt.txt
# manifest, plus the .tar.gz and its .sha256 line next to it.
set -euo pipefail

if [ $# -ne 3 ]; then
    sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
target=$1 version=$2
out=$(mkdir -p "$3" && cd "$3" && pwd)
here=$(cd "$(dirname "$0")/../.." && pwd)
tc=${IRON_TOOLCHAIN:?set IRON_TOOLCHAIN to the pinned toolchain bundle}
tc=$(cd "$tc" && pwd)

case "$target" in
    windows-x86_64) triple=x86_64-pc-windows-msvc; arch=x86_64;;
    *) echo "build_windows_bundle.sh: unknown target $target" >&2; exit 2;;
esac
# MS-style tools take -flag:value here: Git Bash rewrites a leading / into a
# path. Paths inside those options are given in Windows form.
w() { cygpath -w "$1"; }
command -v dumpbin >/dev/null || { echo "build_windows_bundle.sh: dumpbin not on PATH (run from a Developer prompt)" >&2; exit 1; }

llvm=$(sed -n 's/^llvm //p' "$tc/toolchain.txt")
clang="$tc/bin/clang.exe"
nm="$tc/bin/llvm-nm.exe"
name="iron-rt-$version-$target"
dest="$out/$name"
work="$out/work-$target"
rm -rf "$dest" "$work"
mkdir -p "$dest/lib" "$work/objs"

# 1. OpenSSL, static, built once into a cache next to the output. VC-WIN64A
#    static libraries are compiled /MT /Zl (no default CRT named); -GS-
#    drops the stack cookie helpers vcruntime would provide. Its Configure
#    needs a native Windows perl and nmake (from the Developer prompt).
openssl=${OPENSSL_VERSION:-3.5.9}
perl=${PERL:-/c/Strawberry/perl/bin/perl.exe}
cache="$out/cache-$target"
ossl_prefix="$cache/openssl-$openssl-install"
if [ ! -f "$ossl_prefix/lib/libssl.lib" ]; then
    echo "== openssl $openssl"
    src="$cache/openssl-$openssl"
    if [ ! -f "$src/libssl.lib" ]; then
        mkdir -p "$cache"
        rm -rf "$src"
        curl -fsSL "https://github.com/openssl/openssl/releases/download/openssl-$openssl/openssl-$openssl.tar.gz" \
            | tar -xz -C "$cache"
        ( cd "$src" &&
          MSYS2_ARG_CONV_EXCL='*' "$perl" Configure VC-WIN64A no-shared no-asm no-tests no-docs no-apps \
              no-dso no-engine no-module -GS- "CC=$(w "$tc/bin/clang-cl.exe")" \
              "--prefix=$(w "$ossl_prefix")" "--openssldir=$(w "$ossl_prefix/ssl")" > "$cache/configure.log" &&
          MSYS2_ARG_CONV_EXCL='*' nmake -nologo build_libs > "$cache/build.log" ) \
            || { tail -30 "$cache/configure.log" "$cache/build.log" 2>/dev/null; exit 1; }
    fi
    # clang-cl keeps the debug info in the objects, so the ossl_static.pdb
    # that install_dev copies is never written; an empty one satisfies it.
    [ -f "$src/ossl_static.pdb" ] || : > "$src/ossl_static.pdb"
    ( cd "$src" && MSYS2_ARG_CONV_EXCL='*' nmake -nologo install_dev > "$cache/install.log" ) \
        || { tail -30 "$cache/install.log"; exit 1; }
fi

# 2. The Iron runtime and stdlib. The same list as the Linux bundle and as
#    build.c compiles into a native program (raylib is built below). _DLL
#    selects the UCRT DLL (ucrtbase) declarations.
echo "== iron runtime for $triple"
cflags=(--target="$triple" -std=gnu17 -fwrapv -fno-strict-aliasing -O2
        -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_DLL -D_MT
        -I "$here/src" -I "$here/src/stdlib" -I "$here/src/vendor")
sources=$(cd "$here" && ls src/util/stb_ds_impl.c src/util/arena.c src/util/strbuf.c \
    src/runtime/iron_string.c src/runtime/iron_rc.c src/runtime/iron_builtins.c \
    src/runtime/iron_threads.c src/runtime/iron_collections.c src/runtime/iron_net_init.c \
    src/runtime/iron_oom.c src/runtime/iron_fmt.c src/runtime/iron_heap_track.c \
    src/runtime/iron_panic.c src/runtime/iron_leakcheck.c src/runtime/iron_arena_rt.c \
    src/runtime/iron_os.c \
    src/stdlib/iron_math.c src/stdlib/iron_io.c src/stdlib/iron_time.c src/stdlib/iron_log.c \
    src/stdlib/iron_hint.c src/stdlib/iron_net.c src/stdlib/iron_http.c src/stdlib/iron_websocket.c)
for s in $sources; do
    o="$work/objs/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.obj/')"
    "$clang" "${cflags[@]}" -c "$here/$s" -o "$o"
done
rm -f "$dest/lib/iron_rt.lib"
"$tc/bin/llvm-lib.exe" -nologo "-out:$(w "$dest/lib/iron_rt.lib")" "$work"/objs/*.obj
mkdir -p "$work/tls"
"$clang" "${cflags[@]}" -c "$here/src/stdlib/iron_tls.c" -o "$work/tls/iron_tls_none.obj"
"$clang" "${cflags[@]}" -DIRON_HAVE_OPENSSL=1 -I "$ossl_prefix/include" \
    -c "$here/src/stdlib/iron_tls.c" -o "$work/tls/iron_tls.obj"
"$tc/bin/llvm-lib.exe" -nologo "-out:$(w "$dest/lib/iron_tls_none.lib")" "$work/tls/iron_tls_none.obj"
"$tc/bin/llvm-lib.exe" -nologo "-out:$(w "$dest/lib/iron_tls.lib")" "$work/tls/iron_tls.obj"
cp "$ossl_prefix/lib/libssl.lib" "$ossl_prefix/lib/libcrypto.lib" "$dest/lib/"

# raylib: each source is its own translation unit (rlgl.h and glad.h have
# no guards around their implementation sections), as build.c compiles
# them; GLFW picks its Win32 backend.
echo "== raylib"
mkdir -p "$work/raylib"
rl_flags=(--target="$triple" -std=gnu17 -O2 -w -D_DLL -D_MT -D_CRT_SECURE_NO_WARNINGS -DPLATFORM_DESKTOP
          -I "$here/src" -I "$here/src/stdlib" -I "$here/src/vendor"
          -I "$here/src/vendor/raylib" -I "$here/src/vendor/raylib/external/glfw/include")
for s in src/vendor/raylib/rcore.c src/vendor/raylib/rshapes.c src/vendor/raylib/rtextures.c \
         src/vendor/raylib/rtext.c src/vendor/raylib/rmodels.c src/vendor/raylib/raudio.c \
         src/vendor/raylib/rglfw.c src/stdlib/iron_raylib.c src/stdlib/iron_raylib_layout.c; do
    "$clang" "${rl_flags[@]}" -c "$here/$s" -o "$work/raylib/$(basename "$s" .c).obj"
done
"$tc/bin/llvm-lib.exe" -nologo "-out:$(w "$dest/lib/raylib.lib")" "$work"/raylib/*.obj

# The entry object is compiled without the SDK: it declares what it uses.
"$clang" --target="$triple" -O2 -ffreestanding -nostdinc \
    -isystem "$("$clang" -print-resource-dir)/include" \
    -c "$here/src/runtime/iron_win_crt0.c" -o "$dest/lib/iron_crt0.obj"

# 3. Import libraries. Every symbol the runtime, OpenSSL and the entry point leave
#    undefined, minus what the bundle itself defines, must be exported by
#    one of the system DLLs; it goes into that DLL's .def file.
echo "== import libraries"
inputs=("$work"/objs/*.obj "$work"/tls/*.obj "$work"/raylib/*.obj "$dest/lib/iron_crt0.obj"
        "$dest/lib/libssl.lib" "$dest/lib/libcrypto.lib")
"$nm" --defined-only -j "${inputs[@]}" 2>/dev/null | tr -d '\r' | sort -u > "$work/defined.txt"
"$nm" --undefined-only -j "${inputs[@]}" 2>/dev/null | tr -d '\r' \
    | sed 's/^__imp_//' | sort -u > "$work/undefined.txt"
builtins_src="$("$clang" -print-resource-dir)/lib/windows/clang_rt.builtins-$arch.lib"
"$nm" --defined-only -j "$builtins_src" 2>/dev/null | tr -d '\r' | sort -u > "$work/builtins.txt"
# main is the program's own.
comm -23 "$work/undefined.txt" "$work/defined.txt" | comm -23 - "$work/builtins.txt" \
    | grep -vx main > "$work/needed.txt"

dlls="ucrtbase kernel32 ws2_32 bcrypt crypt32 advapi32 user32 opengl32 gdi32 winmm shell32"
sysdir=$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32
for d in $dlls; do
    # Exported names, forwarded ones included ("name (forwarded to ...)").
    dumpbin -nologo -exports "$(w "$sysdir/$d.dll")" | tr -d '\r' | awk '
        $1 ~ /^[0-9]+$/ && NF >= 3 {
            if ($4 ~ /^\(forwarded/ || NF == 3) print $3; else print $4
        }' | sort -u > "$work/$d.exports"
done
: > "$work/unresolved.txt"
for d in $dlls; do : > "$work/$d.syms"; done
while read -r sym; do
    found=
    for d in $dlls; do
        if grep -qxF "$sym" "$work/$d.exports"; then echo "$sym" >> "$work/$d.syms"; found=1; break; fi
    done
    [ -n "$found" ] || echo "$sym" >> "$work/unresolved.txt"
done < "$work/needed.txt"
if [ -s "$work/unresolved.txt" ]; then
    echo "build_windows_bundle.sh: no system DLL exports these symbols:" >&2
    sed 's/^/  /' "$work/unresolved.txt" >&2
    exit 1
fi
for d in $dlls; do
    [ -s "$work/$d.syms" ] || continue
    { echo "LIBRARY $d.dll"; echo "EXPORTS"; sed 's/^/    /' "$work/$d.syms"; } > "$dest/lib/$d.def"
    "$tc/bin/llvm-dlltool.exe" -m i386:x86-64 -d "$dest/lib/$d.def" -l "$dest/lib/$d.lib"
    echo "   $d.dll: $(wc -l < "$work/$d.syms") symbols"
done

cp "$builtins_src" "$dest/lib/clang_rt.builtins.lib"

# 4. Smoke test: link a program that calls into the runtime with only the
#    bundle (no SDK library path, no default libraries) and run it.
echo "== smoke test"
cat > "$work/smoke.c" <<'SMOKE'
#include "runtime/iron_runtime.h"
int main(int argc, char **argv) {
    /* A _Thread_local with an initializer: the TLS template from the entry
     * object's _tls_used must reach every thread. */
    if (iron_stack_gen != 1) return 3;
    iron_runtime_init(argc, argv);
    Iron_println(iron_string_from_literal("bundle ok", 9));
    return 0;
}
SMOKE
"$clang" --target="$triple" -O2 -std=gnu17 -nostdinc -isystem "$("$clang" -print-resource-dir)/include" \
    -I "$here/src" -I "$here/src/stdlib" -c "$work/smoke.c" -o "$work/smoke.obj"
libs=()
for d in $dlls; do [ -f "$dest/lib/$d.lib" ] && libs+=("$dest/lib/$d.lib"); done
"$tc/bin/lld-link.exe" -nologo -nodefaultlib -subsystem:console -entry:mainCRTStartup \
    "-out:$(w "$work/smoke.exe")" "$dest/lib/iron_crt0.obj" "$work/smoke.obj" "$dest/lib/iron_rt.lib" \
    "${libs[@]}" "$dest/lib/clang_rt.builtins.lib"
got=$("$work/smoke.exe" | tr -d '\r')
[ "$got" = "bundle ok" ] || { echo "build_windows_bundle.sh: smoke test printed '$got'" >&2; exit 1; }

commit=${IRON_COMMIT:-$(cd "$here" && git rev-parse --short HEAD 2>/dev/null || echo unknown)}
cat > "$dest/rt.txt" <<MANIFEST
iron-rt 1
version $version
commit $commit
target $target
triple $triple
libc ucrt
tls openssl $openssl
llvm $llvm
MANIFEST

# Neutral ownership: the archive is unpacked as root in containers and CI,
# where tar would otherwise try to restore Windows' uid and gid and fail.
( cd "$out" && tar --owner=0 --group=0 --numeric-owner -czf "$name.tar.gz" "$name" && \
  (sha256sum "$name.tar.gz" 2>/dev/null || shasum -a 256 "$name.tar.gz") > "$name.tar.gz.sha256" )
echo "== wrote $out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"
