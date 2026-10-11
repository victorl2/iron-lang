#!/usr/bin/env bash
# Reproduce the required CI checks locally, much faster than hosted runners.
#
#   scripts/ci/local_ci.sh [all|linux|mac]   (default: all)
#
# Legs, with the flags of .github/workflows/ci.yml:
#   linux-asan     Debug + IRON_ENABLE_SANITIZERS: full ctest (no benchmarks)
#                  and tests/integration/run_integration.sh   build-and-test (ubuntu)
#                  with a MySQL server for test_mysql_integration
#   linux-release  Release: full ctest (no benchmarks)       build-and-test-release
#   linux-leaks    tests/oracles/leak_check.sh under valgrind over the v4 corpus,
#                  and the MySQL integration tests and the apps/inventory
#                  service suite under valgrind
#   mac-asan       Debug + IRON_ENABLE_SANITIZERS: full ctest (no benchmarks)
#                                                              build-and-test (macos)
#   mac-release    Release build of every target               build (macos-*)
#
# The Linux legs run on CI_HOST (default silvaserver.local) inside the
# iron-dev podman image (clang, cmake, ninja, OpenSSL, valgrind, X11/GL
# headers, pytest-lsp) with a memory cap, the tree rsynced to
# CI_REMOTE_DIR. The asan and leaks legs each get their own MySQL server
# (CI_MYSQL_IMAGE, default docker.io/library/mysql:8.4) on a podman network
# created for the run and removed after it; IRON_MYSQL_URL and
# IRON_MYSQL_CA point the tests at it. The mac legs run on this machine. The checked out commit
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
CI_MYSQL_IMAGE=${CI_MYSQL_IMAGE:-docker.io/library/mysql:8.4}
CI_LOG_DIR=${CI_LOG_DIR:-$here/.local-ci}
JOBS_LOCAL=${JOBS_LOCAL:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
mkdir -p "$CI_LOG_DIR"
sha=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain | grep -v '^??' | head -1)
[ -n "$dirty" ] && sha="$sha+dirty"

# ── Linux legs, run inside the container ───────────────────────────────
# TLS and crypto are part of what the legs test: fail when CMake did not
# find OpenSSL instead of testing the "unavailable" stubs.
need_openssl() {
    if grep -q '^OPENSSL_CRYPTO_LIBRARY:FILEPATH=.*NOTFOUND' "$1/CMakeCache.txt"; then
        echo "local_ci: CMake did not find OpenSSL in $1" >&2
        return 1
    fi
}

linux_leg() {   # $1 = asan | release | leaks
    local leg=$1 dir="build-ci-$1"
    export IRON_CURRENT_PHASE=37
    # No OPENSSL_ROOT_DIR here: with it, CMake 3.28's FindOpenSSL looks in
    # /usr/lib and /usr/lib64 only and misses Ubuntu's multiarch
    # /usr/lib/x86_64-linux-gnu, so TLS and crypto built as unavailable.
    # -U drops OpenSSL results cached by such a run in a reused build dir.
    case $leg in
    asan)
        export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_stack_use_after_return=1
        export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
        cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
            -U 'OPENSSL_*' -DIRON_ENABLE_SANITIZERS=ON >/dev/null && need_openssl "$dir" &&
        cmake --build "$dir" -j8 >"$dir.build.log" 2>&1 &&
        ctest --test-dir "$dir" --output-on-failure -j8 -E benchmark &&
        tests/integration/run_integration.sh "$dir/ironc" ;;
    release)
        cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
            -U 'OPENSSL_*' >/dev/null && need_openssl "$dir" &&
        cmake --build "$dir" -j8 >"$dir.build.log" 2>&1 &&
        ctest --test-dir "$dir" --output-on-failure -j8 -E benchmark ;;
    leaks)
        cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
            -U 'OPENSSL_*' >/dev/null && need_openssl "$dir" &&
        cmake --build "$dir" -j8 --target iron ironc >"$dir.build.log" 2>&1 &&
        LEAK_CHECK_JOBS=8 tests/oracles/leak_check.sh "$dir/iron" \
            $(find tests/integration/v4 -mindepth 1 -maxdepth 2 -type d) &&
        { [ -z "${IRON_MYSQL_URL:-}" ] || IRON_MYSQL_LEAKS=1 tests/mysql/run.sh "$dir/ironc"; } &&
        { [ -z "${IRON_MYSQL_URL:-}" ] || INVENTORY_VALGRIND=1 apps/inventory/tests/run.sh "$dir/iron"; } ;;
    esac
}
if [ "$what" = "--in-container" ]; then linux_leg "$2"; exit $?; fi

run_linux() {
    rsync -az --delete --exclude 'build*/' --exclude '.iron-build/' --exclude '.local-ci/' \
        --exclude 'node_modules/' --exclude '.vscode-test/' --exclude 'target/' \
        "$here/" "$CI_HOST:$CI_REMOTE_DIR/" || { echo "linux: rsync failed"; return 1; }
    # MySQL servers for the asan and leaks legs, on a network of their own.
    local tag="iron-ci-$$" leg
    local net="$tag-net"
    ssh "$CI_HOST" "podman network create $net >/dev/null" || { echo "linux: podman network failed"; return 1; }
    for leg in asan leaks; do
        ssh "$CI_HOST" "podman run -d --name $tag-mysql-$leg --network $net --memory=2g \
            -e MYSQL_ROOT_PASSWORD=ironci -e MYSQL_DATABASE=iron_test $CI_MYSQL_IMAGE \
            --max-allowed-packet=67108864 --mysql-native-password=ON >/dev/null" ||
            echo "linux: could not start the MySQL server for $leg (its tests will be skipped)"
    done
    for leg in asan leaks; do
        ssh "$CI_HOST" "for i in \$(seq 1 90); do
                podman exec $tag-mysql-$leg mysqladmin -uroot -pironci ping >/dev/null 2>&1 &&
                podman exec $tag-mysql-$leg mysql --protocol=TCP -h127.0.0.1 -uroot -pironci -e 'SELECT 1' >/dev/null 2>&1 && break
                sleep 2
            done
            podman cp $tag-mysql-$leg:/var/lib/mysql/ca.pem $CI_REMOTE_DIR/.mysql-ca-$leg.pem" >/dev/null 2>&1
    done
    local pids=()
    for leg in asan release leaks; do
        local mysql_env=""
        if [ "$leg" != release ]; then
            mysql_env="-e IRON_MYSQL_URL=mysql://root:ironci@$tag-mysql-$leg:3306/iron_test \
                -e IRON_MYSQL_CA=/work/.mysql-ca-$leg.pem"
        fi
        ssh "$CI_HOST" "podman run --rm --memory=$CI_MEMORY --network $net $mysql_env \
            -v $CI_REMOTE_DIR:/work:Z -v \$HOME/.iron:/root/.iron:Z -w /work $CI_IMAGE \
            bash scripts/ci/local_ci.sh --in-container $leg" >"$CI_LOG_DIR/linux-$leg.log" 2>&1 &
        pids+=($!)
    done
    local rc=0 i=0
    for leg in asan release leaks; do
        if wait "${pids[$i]}"; then echo "PASS linux-$leg"; else echo "FAIL linux-$leg ($CI_LOG_DIR/linux-$leg.log)"; rc=1; fi
        i=$((i + 1))
    done
    ssh "$CI_HOST" "podman rm -f $tag-mysql-asan $tag-mysql-leaks >/dev/null 2>&1;
        podman network rm $net >/dev/null 2>&1; rm -f $CI_REMOTE_DIR/.mysql-ca-*.pem"
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
