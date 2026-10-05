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
#
# Writes <out-dir>/iron-rt-<version>-<target>/ with lib/ and a rt.txt
# manifest, plus the .tar.gz and its .sha256 line next to it.
set -euo pipefail

if [ $# -ne 3 ]; then
    sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
target=$1 version=$2 out=$3
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

# 1. The Iron runtime and stdlib. The same list as the Linux bundle and as
#    build.c compiles into a native program (raylib and TLS are not part of
#    a bundle yet). _DLL selects the UCRT DLL (ucrtbase) declarations.
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
    src/stdlib/iron_hint.c src/stdlib/iron_net.c src/stdlib/iron_http.c src/stdlib/iron_tls.c src/stdlib/iron_websocket.c)
for s in $sources; do
    o="$work/objs/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.obj/')"
    "$clang" "${cflags[@]}" -c "$here/$s" -o "$o"
done
rm -f "$dest/lib/iron_rt.lib"
"$tc/bin/llvm-lib.exe" -nologo "-out:$(w "$dest/lib/iron_rt.lib")" "$work"/objs/*.obj

# The entry object is compiled without the SDK: it declares what it uses.
"$clang" --target="$triple" -O2 -ffreestanding -nostdinc \
    -isystem "$("$clang" -print-resource-dir)/include" \
    -c "$here/src/runtime/iron_win_crt0.c" -o "$dest/lib/iron_crt0.obj"

# 2. Import libraries. Every symbol the runtime and the entry point leave
#    undefined, minus what the bundle itself defines, must be exported by
#    one of the system DLLs; it goes into that DLL's .def file.
echo "== import libraries"
"$nm" --defined-only -j "$work"/objs/*.obj "$dest/lib/iron_crt0.obj" | tr -d '\r' | sort -u > "$work/defined.txt"
"$nm" --undefined-only -j "$work"/objs/*.obj "$dest/lib/iron_crt0.obj" | tr -d '\r' \
    | sed 's/^__imp_//' | sort -u > "$work/undefined.txt"
builtins_src="$("$clang" -print-resource-dir)/lib/windows/clang_rt.builtins-$arch.lib"
"$nm" --defined-only -j "$builtins_src" 2>/dev/null | tr -d '\r' | sort -u > "$work/builtins.txt"
# main is the program's own.
comm -23 "$work/undefined.txt" "$work/defined.txt" | comm -23 - "$work/builtins.txt" \
    | grep -vx main > "$work/needed.txt"

dlls="ucrtbase kernel32 ws2_32 bcrypt"
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

# 3. Smoke test: link a program that calls into the runtime with only the
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
llvm $llvm
MANIFEST

# Neutral ownership: the archive is unpacked as root in containers and CI,
# where tar would otherwise try to restore Windows' uid and gid and fail.
( cd "$out" && tar --owner=0 --group=0 --numeric-owner -czf "$name.tar.gz" "$name" && \
  (sha256sum "$name.tar.gz" 2>/dev/null || shasum -a 256 "$name.tar.gz") > "$name.tar.gz.sha256" )
echo "== wrote $out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"
