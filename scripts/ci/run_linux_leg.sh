#!/usr/bin/env bash
# One Linux local-CI leg. This runs unchanged in a remote podman container or
# directly from a checkout, which keeps local reproduction identical.
set -uo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 asan|release|leaks" >&2
    exit 2
fi
leg=$1
case "$leg" in asan|release|leaks) ;; *) echo "unknown Linux leg: $leg" >&2; exit 2 ;; esac

repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
jobs=${CI_JOBS:-$(nproc)}
scale=${IRON_TEST_TIMEOUT_SCALE:-1}
retry_jobs=${CI_RETRY_JOBS:-2}
result_dir=${CI_RESULT_DIR:-$repo/.local-ci/local-$leg}
build_dir=${CI_BUILD_DIR:-$repo/build-ci-$leg}
build_lock=${CI_BUILD_LOCK:-$repo/.local-ci/linux-build.lock}
mkdir -p "$result_dir" "$(dirname "$build_lock")"
prefix="$result_dir/$leg"
started=$(date +%s)
ctest_rc=0
extra_rc=0

write_result() {
    local rc=$1 status=pass
    [ "$rc" -eq 0 ] || status=fail
    {
        echo "status=$status"
        echo "exit_code=$rc"
        echo "leg=$leg"
        echo "jobs=$jobs"
        echo "timeout_scale=$scale"
        echo "elapsed_seconds=$(( $(date +%s) - started ))"
        echo "finished_at=$(date -u +%FT%TZ)"
    } > "$prefix.result"
}
trap 'rc=$?; write_result "$rc"' EXIT

need_openssl() {
    if grep -q '^OPENSSL_CRYPTO_LIBRARY:FILEPATH=.*NOTFOUND' "$build_dir/CMakeCache.txt"; then
        echo "local_ci: CMake did not find OpenSSL in $build_dir" >&2
        return 1
    fi
}

configure_and_build() {
    local build_type=$1
    shift
    # All Linux leg containers share this lock through the checkout mount.
    # Configuration and compilation are serialized; test phases may overlap.
    exec 9>"$build_lock"
    echo "$leg: waiting for the shared build slot"
    flock 9
    echo "$leg: configuring and building with -j$jobs"
    cmake -S . -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" -DCMAKE_C_COMPILER=clang \
        -DIRON_TEST_TIMEOUT_SCALE="$scale" -DIRON_TEST_JOBS="$jobs" \
        -U 'OPENSSL_*' "$@" &&
        need_openssl &&
        cmake --build "$build_dir" -j"$jobs" ${CI_BUILD_TARGETS:+--target $CI_BUILD_TARGETS}
    local rc=$?
    flock -u 9
    exec 9>&-
    return "$rc"
}

export IRON_CURRENT_PHASE=37
export PARITY_JOBS="$jobs" IRON_TEST_JOBS="$jobs" LEAK_CHECK_JOBS="$jobs"
export CI_RETRY_JOBS="$retry_jobs"

case "$leg" in
asan)
    export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_stack_use_after_return=1
    export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    configure_and_build Debug -DIRON_ENABLE_SANITIZERS=ON || exit $?
    scripts/ci/ctest_with_retry.sh "$build_dir" "$jobs" "$prefix" -E benchmark
    ctest_rc=$?
    tests/integration/run_integration.sh "$build_dir/ironc"
    extra_rc=$?
    ;;
release)
    configure_and_build Release || exit $?
    scripts/ci/ctest_with_retry.sh "$build_dir" "$jobs" "$prefix" -E benchmark
    ctest_rc=$?
    ;;
leaks)
    CI_BUILD_TARGETS="iron ironc" configure_and_build Release || exit $?
    tests/oracles/leak_check.sh "$build_dir/iron" \
        $(find tests/integration/v4 -mindepth 1 -maxdepth 2 -type d)
    extra_rc=$?
    if [ -n "${IRON_MYSQL_URL:-}" ]; then
        if ! IRON_MYSQL_LEAKS=1 tests/mysql/run.sh "$build_dir/ironc"; then extra_rc=1; fi
        if ! INVENTORY_VALGRIND=1 apps/inventory/tests/run.sh "$build_dir/iron"; then extra_rc=1; fi
    fi
    ;;
esac

[ "$ctest_rc" -eq 0 ] && [ "$extra_rc" -eq 0 ]
