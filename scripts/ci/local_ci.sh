#!/usr/bin/env bash
# Reproduce the required CI checks locally, much faster than hosted runners.
#
#   scripts/ci/local_ci.sh [all|linux|mac]   (default: all)
#
# Legs, with the flags of .github/workflows/ci.yml:
#   linux-asan     Debug + IRON_ENABLE_SANITIZERS: full ctest (no benchmarks)
#                  and tests/integration/run_integration.sh   build-and-test (ubuntu)
#   linux-release  Release: full ctest (no benchmarks)       build-and-test-release
#   linux-leaks    tests/oracles/leak_check.sh under valgrind over the v4 corpus
#   mac-asan       Debug + IRON_ENABLE_SANITIZERS: full ctest (no benchmarks)
#                                                              build-and-test (macos)
#   mac-release    Release build of every target               build (macos-*)
#
# The Linux legs run on CI_HOST (default silvaserver.local) inside the
# iron-dev podman image (clang, cmake, ninja, OpenSSL, valgrind, X11/GL
# headers, pytest-lsp) with a memory cap, the tree rsynced to
# CI_REMOTE_DIR. The mac legs run on this machine. The checked out commit
# is tested as it is on disk (uncommitted changes included); the summary
# names the commit. Logs go to CI_LOG_DIR (default .local-ci/).
#
# Exit status 0 when every leg passed.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
cd "$here"
what=${1:-all}
CI_HOST=${CI_HOST:-silvaserver.local}
CI_REMOTE_DIR=${CI_REMOTE_DIR:-/home/victor/code/iron-local-ci}
CI_IMAGE=${CI_IMAGE:-localhost/iron-dev:latest}
CI_MEMORY=${CI_MEMORY:-24g}
CI_LOG_DIR=${CI_LOG_DIR:-$here/.local-ci}
JOBS_LOCAL=${JOBS_LOCAL:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
mkdir -p "$CI_LOG_DIR"
sha=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain | grep -v '^??' | head -1)
[ -n "$dirty" ] && sha="$sha+dirty"

# ── Linux legs, run inside the container ───────────────────────────────
linux_leg() {   # $1 = asan | release | leaks
    local leg=$1 dir="build-ci-$1"
    export IRON_CURRENT_PHASE=37
    case $leg in
    asan)
        export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_stack_use_after_return=1
        export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
        cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
            -DOPENSSL_ROOT_DIR=/usr -DIRON_ENABLE_SANITIZERS=ON >/dev/null &&
        cmake --build "$dir" -j8 >"$dir.build.log" 2>&1 &&
        ctest --test-dir "$dir" --output-on-failure -j8 -E benchmark &&
        tests/integration/run_integration.sh "$dir/ironc" ;;
    release)
        cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
            -DOPENSSL_ROOT_DIR=/usr >/dev/null &&
        cmake --build "$dir" -j8 >"$dir.build.log" 2>&1 &&
        ctest --test-dir "$dir" --output-on-failure -j8 -E benchmark ;;
    leaks)
        cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
            -DOPENSSL_ROOT_DIR=/usr >/dev/null &&
        cmake --build "$dir" -j8 --target iron ironc >"$dir.build.log" 2>&1 &&
        LEAK_CHECK_JOBS=8 tests/oracles/leak_check.sh "$dir/iron" \
            $(find tests/integration/v4 -mindepth 1 -maxdepth 2 -type d) ;;
    esac
}
if [ "$what" = "--in-container" ]; then linux_leg "$2"; exit $?; fi

run_linux() {
    rsync -az --delete --exclude 'build*/' --exclude '.iron-build/' --exclude '.local-ci/' \
        --exclude 'node_modules/' --exclude '.vscode-test/' --exclude 'target/' \
        "$here/" "$CI_HOST:$CI_REMOTE_DIR/" || { echo "linux: rsync failed"; return 1; }
    local pids=() leg
    for leg in asan release leaks; do
        ssh "$CI_HOST" "podman run --rm --memory=$CI_MEMORY \
            -v $CI_REMOTE_DIR:/work:Z -v \$HOME/.iron:/root/.iron:Z -w /work $CI_IMAGE \
            bash scripts/ci/local_ci.sh --in-container $leg" >"$CI_LOG_DIR/linux-$leg.log" 2>&1 &
        pids+=($!)
    done
    local rc=0 i=0
    for leg in asan release leaks; do
        if wait "${pids[$i]}"; then echo "PASS linux-$leg"; else echo "FAIL linux-$leg ($CI_LOG_DIR/linux-$leg.log)"; rc=1; fi
        i=$((i + 1))
    done
    return $rc
}

run_mac() {
    local rc=0
    ( export IRON_CURRENT_PHASE=37 ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_stack_use_after_return=1 \
             UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
      cmake -S . -B build-ci-mac-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
          -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3 2>/dev/null)" -DIRON_ENABLE_SANITIZERS=ON >/dev/null &&
      cmake --build build-ci-mac-asan -j"$JOBS_LOCAL" &&
      ctest --test-dir build-ci-mac-asan --output-on-failure -j"$JOBS_LOCAL" -E benchmark
    ) >"$CI_LOG_DIR/mac-asan.log" 2>&1 && echo "PASS mac-asan" || { echo "FAIL mac-asan ($CI_LOG_DIR/mac-asan.log)"; rc=1; }
    ( cmake -S . -B build-ci-mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
          -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3 2>/dev/null)" >/dev/null &&
      cmake --build build-ci-mac-release -j"$JOBS_LOCAL"
    ) >"$CI_LOG_DIR/mac-release.log" 2>&1 && echo "PASS mac-release" || { echo "FAIL mac-release ($CI_LOG_DIR/mac-release.log)"; rc=1; }
    return $rc
}

start=$(date +%s)
echo "local CI for $sha"
rc=0
case $what in
all)
    run_linux >"$CI_LOG_DIR/linux.summary" 2>&1 & lp=$!
    run_mac || rc=1
    wait $lp || rc=1
    cat "$CI_LOG_DIR/linux.summary" ;;
linux) run_linux || rc=1 ;;
mac)   run_mac || rc=1 ;;
*) echo "usage: $0 [all|linux|mac]" >&2; exit 2 ;;
esac
echo "local CI for $sha: $([ $rc -eq 0 ] && echo PASSED || echo FAILED) in $(( $(date +%s) - start )) s"
echo "$sha $([ $rc -eq 0 ] && echo pass || echo fail) $(date -u +%FT%TZ)" >> "$CI_LOG_DIR/history"
exit $rc
