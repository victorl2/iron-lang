#!/bin/bash
# build_llvm.sh - build the pinned LLVM release for an Iron toolchain bundle.
#
# Builds clang, lld, the archivers and compiler-rt (builtins and the host
# sanitizer runtimes) for the X86, AArch64 and WebAssembly targets only,
# statically linked, with nothing optional (no zlib, zstd, libxml2,
# terminfo, libedit, Python bindings, static analyzer). The install goes to
# <out>/prefix, which pack.sh turns into the bundle.
#
# usage: scripts/toolchain/build_llvm.sh <llvm-version> <host> <out-dir> [jobs]
#   host is one of macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64
#
# Environment:
#   LLVM_SRC          existing llvm-project checkout to use instead of cloning
#   CMAKE_C_COMPILER_LAUNCHER / CMAKE_CXX_COMPILER_LAUNCHER
#                     e.g. sccache; forwarded to cmake
#   LLVM_LINK_JOBS    parallel link jobs (default 2; links are memory hungry)
set -euo pipefail

if [ $# -lt 3 ]; then
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
llvm=$1 host=$2 out=$3 jobs=${4:-}
mkdir -p "$out"
out=$(cd "$out" && pwd)
src=${LLVM_SRC:-$out/llvm-project}
build=$out/build
prefix=$out/prefix
if [ -z "$jobs" ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)
fi

if [ ! -d "$src/llvm" ]; then
    echo "== fetching llvm-project $llvm"
    git clone --depth 1 --branch "llvmorg-$llvm" https://github.com/llvm/llvm-project.git "$src"
fi
# A pinned release only: never build whatever happens to be checked out.
actual=$(git -C "$src" describe --tags --exact-match 2>/dev/null || git -C "$src" rev-parse --short HEAD)
case "$actual" in
    "llvmorg-$llvm") ;;
    *) echo "build_llvm.sh: $src is at $actual, not llvmorg-$llvm" >&2; exit 1;;
esac

args=(
    -S "$src/llvm" -B "$build" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$prefix"
    -DLLVM_ENABLE_PROJECTS="clang;lld"
    -DLLVM_ENABLE_RUNTIMES="compiler-rt"
    -DLLVM_TARGETS_TO_BUILD="X86;AArch64;WebAssembly"
    -DLLVM_ENABLE_ASSERTIONS=OFF
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF
    -DLLVM_INCLUDE_UTILS=OFF -DLLVM_BUILD_UTILS=OFF
    -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF
    -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF
    -DCLANG_PLUGIN_SUPPORT=OFF
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF
    -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF -DLLVM_ENABLE_Z3_SOLVER=OFF
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF
    -DLLVM_ENABLE_LIBCXX=OFF
    -DLLVM_PARALLEL_LINK_JOBS="${LLVM_LINK_JOBS:-2}"
    -DLLVM_DISTRIBUTION_COMPONENTS="clang;clang-resource-headers;lld;llvm-ar;llvm-ranlib;llvm-dlltool;llvm-lib;llvm-nm;llvm-objcopy;llvm-strip;builtins;runtimes"
    # compiler-rt: builtins plus the host sanitizers Iron's CI runs with.
    -DCOMPILER_RT_BUILD_SANITIZERS=ON
    -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF
    -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF
    -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_GWP_ASAN=OFF
    -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF
    -DCOMPILER_RT_INCLUDE_TESTS=OFF
)
if [ -n "${CMAKE_C_COMPILER_LAUNCHER:-}" ]; then
    args+=(-DCMAKE_C_COMPILER_LAUNCHER="$CMAKE_C_COMPILER_LAUNCHER"
           -DCMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER:-$CMAKE_C_COMPILER_LAUNCHER}")
fi
case "$host" in
    linux-*)
        # Tools that run on any distribution with a recent enough glibc:
        # libstdc++ and libgcc linked in.
        args+=(-DLLVM_STATIC_LINK_CXX_STDLIB=ON
               -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc"
               -DLLVM_ENABLE_PIC=ON)
        ;;
    macos-*)
        args+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
               -DCOMPILER_RT_ENABLE_IOS=OFF -DCOMPILER_RT_ENABLE_WATCHOS=OFF
               -DCOMPILER_RT_ENABLE_TVOS=OFF -DCOMPILER_RT_ENABLE_XROS=OFF)
        case "$host" in
            macos-arm64)  args+=(-DCMAKE_OSX_ARCHITECTURES=arm64  -DLLVM_HOST_TRIPLE=arm64-apple-darwin);;
            macos-x86_64) args+=(-DCMAKE_OSX_ARCHITECTURES=x86_64 -DLLVM_HOST_TRIPLE=x86_64-apple-darwin);;
        esac
        ;;
    windows-*)
        # Built with clang-cl against the MSVC runtime, statically linked.
        args+=(-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
               -DLLVM_USE_CRT_RELEASE=MT
               -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
               -DLLVM_HOST_TRIPLE=x86_64-pc-windows-msvc)
        ;;
    *) echo "build_llvm.sh: unknown host $host" >&2; exit 1;;
esac

echo "== configuring ($jobs jobs)"
cmake "${args[@]}"
echo "== building"
cmake --build "$build" -j "$jobs" --target install-distribution
echo "== installed to $prefix"
ls "$prefix/bin"
