#!/usr/bin/env bash
# Reproduce the required CI checks locally, with recoverable detached Linux
# legs on a shared podman host.
#
#   scripts/ci/local_ci.sh [all|linux|mac|status|wait]   (default: all)
#
# Linux legs are detached named containers. Their logs, JUnit documents and
# result files live under CI_REMOTE_DIR/.local-ci/runs/<run-id>/, so `status`
# and `wait` can reconnect after the initiating SSH session disappears.
set -uo pipefail

here=$(cd "$(dirname "$0")/../.." && pwd)
cd "$here"
what=${1:-all}
CI_HOST=${CI_HOST:-silvaserver.local}
CI_REMOTE_DIR=${CI_REMOTE_DIR:-/home/victor/code/iron-local-ci}
CI_IMAGE=${CI_IMAGE:-localhost/iron-dev:latest}
CI_MEMORY=${CI_MEMORY:-24g}
CI_MYSQL_IMAGE=${CI_MYSQL_IMAGE:-docker.io/library/mysql:8.4}
CI_MYSQL_MEMORY=${CI_MYSQL_MEMORY:-3g}
CI_LOG_DIR=${CI_LOG_DIR:-$here/.local-ci}
CI_POLL_SECONDS=${CI_POLL_SECONDS:-15}
JOBS_LOCAL=${JOBS_LOCAL:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
current_file="$CI_LOG_DIR/current-run"
mkdir -p "$CI_LOG_DIR"
sha=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain | grep -v '^??' | head -1)
[ -n "$dirty" ] && sha="$sha+dirty"
active_run=""

remote_ci() {
    local action=$1
    shift
    ssh "$CI_HOST" env \
        CI_IMAGE="$CI_IMAGE" CI_MEMORY="$CI_MEMORY" \
        CI_MYSQL_IMAGE="$CI_MYSQL_IMAGE" CI_MYSQL_MEMORY="$CI_MYSQL_MEMORY" \
        bash "$CI_REMOTE_DIR/scripts/ci/remote_linux_ci.sh" "$action" "$@"
}

current_run() {
    [ -s "$current_file" ] || {
        echo "local_ci: no detached Linux run is recorded" >&2
        return 1
    }
    head -1 "$current_file"
}

cleanup_active() {
    [ -n "$active_run" ] || return 0
    echo "local_ci: cleaning resources for $active_run"
    remote_ci cleanup "$active_run" || true
}

interrupted() {
    trap - INT TERM
    cleanup_active
    exit 130
}
trap interrupted INT TERM

choose_capacity() {
    local capacity load cores loaded reserve free share computed_scale
    capacity=$(ssh "$CI_HOST" "awk '{print \$1}' /proc/loadavg; nproc") || return 1
    load=$(printf '%s\n' "$capacity" | sed -n '1p')
    cores=$(printf '%s\n' "$capacity" | sed -n '2p')
    loaded=$(awk -v n="$load" 'BEGIN { print int(n) + (n > int(n)) }')
    reserve=$((cores / 6))
    [ "$reserve" -lt 2 ] && reserve=2
    [ "$reserve" -ge "$cores" ] && reserve=$((cores - 1))
    [ "$reserve" -lt 0 ] && reserve=0
    free=$((cores - loaded))
    [ "$free" -lt 1 ] && free=1
    share=$(((free - reserve) / 3))
    [ "$share" -lt 1 ] && share=1
    [ "$share" -gt 8 ] && share=8
    computed_scale=$(((cores + free - 1) / free))
    [ "$computed_scale" -gt 8 ] && computed_scale=8

    jobs_asan=${CI_JOBS_ASAN:-${CI_JOBS:-$share}}
    jobs_release=${CI_JOBS_RELEASE:-${CI_JOBS:-$share}}
    jobs_leaks=${CI_JOBS_LEAKS:-${CI_JOBS:-$share}}
    timeout_scale=${IRON_TEST_TIMEOUT_SCALE:-$computed_scale}
    echo "linux capacity: $cores CPUs, load $load, reserving $reserve CPUs"
    echo "linux jobs: asan=$jobs_asan release=$jobs_release leaks=$jobs_leaks; timeout scale=$timeout_scale"
}

sync_tree() {
    rsync -az --delete --exclude 'build*/' --exclude '.iron-build/' \
        --exclude '.local-ci/' --exclude 'node_modules/' \
        --exclude '.vscode-test/' --exclude 'target/' \
        "$here/" "$CI_HOST:$CI_REMOTE_DIR/"
}

start_linux() {
    if [ -s "$current_file" ]; then
        echo "local_ci: a run is already recorded; use '$0 status' or '$0 wait'" >&2
        return 1
    fi
    sync_tree || { echo "linux: rsync failed" >&2; return 1; }
    choose_capacity || { echo "linux: capacity query failed" >&2; return 1; }
    active_run="$(git rev-parse --short HEAD)-$(date -u +%Y%m%dT%H%M%SZ)-$$"
    printf '%s\n' "$active_run" > "$current_file"
    if ! remote_ci start "$active_run" "$jobs_asan" "$jobs_release" "$jobs_leaks" "$timeout_scale"; then
        echo "linux: remote startup failed" >&2
        cleanup_active
        rm -f "$current_file"
        active_run=""
        return 1
    fi
    echo "linux: detached run $active_run started"
}

show_status() {
    local run
    run=$(current_run) || return 1
    echo "linux run $run on $CI_HOST"
    remote_ci status "$run"
}

fetch_results() {
    local run=$1 local_dir="$CI_LOG_DIR/runs/$run"
    mkdir -p "$local_dir"
    rsync -az "$CI_HOST:$CI_REMOTE_DIR/.local-ci/runs/$run/" "$local_dir/"
}

finish_linux() {
    local run=$1 rc=0 leg status retried local_dir="$CI_LOG_DIR/runs/$run"
    fetch_results "$run" || return 1
    for leg in asan release leaks; do
        status=$(sed -n 's/^status=//p' "$local_dir/$leg.result" | head -1)
        if [ "$status" = pass ]; then
            echo "PASS linux-$leg"
        else
            echo "FAIL linux-$leg ($local_dir/$leg.log)"
            rc=1
        fi
        retried="$local_dir/$leg-retried.txt"
        if [ -s "$retried" ]; then
            echo "  second pass:"
            sed 's/^/    /' "$retried"
        fi
    done
    remote_ci cleanup "$run" || rc=1
    rm -f "$current_file"
    active_run=""
    return "$rc"
}

wait_linux() {
    local run state finished
    run=$(current_run) || return 1
    active_run=$run
    while :; do
        if ! state=$(remote_ci status "$run"); then
            echo "linux: SSH unavailable; run '$0 wait' to reattach to $run" >&2
            active_run=""
            return 75
        fi
        echo "$state"
        finished=$(printf '%s\n' "$state" | awk '$2 == "pass" || $2 == "fail" { n++ } END { print n+0 }')
        [ "$finished" -eq 3 ] && break
        sleep "$CI_POLL_SECONDS"
    done
    finish_linux "$run"
}

run_mac() {
    local rc=0 scale=${IRON_TEST_TIMEOUT_SCALE:-1}
    ( export IRON_CURRENT_PHASE=37
      export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_stack_use_after_return=1
      export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
      cmake -S . -B build-ci-mac-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_C_COMPILER=clang -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3 2>/dev/null)" \
          -DIRON_ENABLE_SANITIZERS=ON -DIRON_TEST_TIMEOUT_SCALE="$scale" \
          -DIRON_TEST_JOBS="$JOBS_LOCAL" &&
      cmake --build build-ci-mac-asan -j"$JOBS_LOCAL" &&
      CI_RETRY_JOBS=2 PARITY_JOBS="$JOBS_LOCAL" IRON_TEST_JOBS="$JOBS_LOCAL" \
          scripts/ci/ctest_with_retry.sh build-ci-mac-asan "$JOBS_LOCAL" \
          "$CI_LOG_DIR/mac-asan" -E benchmark
    ) >"$CI_LOG_DIR/mac-asan.log" 2>&1 && echo "PASS mac-asan" || {
        echo "FAIL mac-asan ($CI_LOG_DIR/mac-asan.log)"; rc=1;
    }
    ( cmake -S . -B build-ci-mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=clang -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3 2>/dev/null)" \
          -DIRON_TEST_TIMEOUT_SCALE="$scale" -DIRON_TEST_JOBS="$JOBS_LOCAL" &&
      cmake --build build-ci-mac-release -j"$JOBS_LOCAL"
    ) >"$CI_LOG_DIR/mac-release.log" 2>&1 && echo "PASS mac-release" || {
        echo "FAIL mac-release ($CI_LOG_DIR/mac-release.log)"; rc=1;
    }
    return "$rc"
}

start=$(date +%s)
echo "local CI for $sha"
rc=0
case "$what" in
all)
    start_linux || exit $?
    run_mac || rc=1
    wait_linux || rc=1
    ;;
linux)
    start_linux && wait_linux || rc=$?
    ;;
mac)
    run_mac || rc=1
    ;;
status)
    show_status
    exit $?
    ;;
wait)
    wait_linux
    exit $?
    ;;
*)
    echo "usage: $0 [all|linux|mac|status|wait]" >&2
    exit 2
    ;;
esac
echo "local CI for $sha: $([ "$rc" -eq 0 ] && echo PASSED || echo FAILED) in $(( $(date +%s) - start )) s"
echo "$sha $([ "$rc" -eq 0 ] && echo pass || echo fail) $(date -u +%FT%TZ)" >> "$CI_LOG_DIR/history"
exit "$rc"
