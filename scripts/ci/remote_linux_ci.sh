#!/usr/bin/env bash
# Host-side podman orchestration for scripts/ci/local_ci.sh. Every resource is
# derived from one validated run id so cleanup cannot affect another run.
set -uo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 start|status|cleanup RUN_ID [ASAN_JOBS RELEASE_JOBS LEAK_JOBS SCALE]" >&2
    exit 2
fi
action=$1
run_id=$2
case "$run_id" in *[!a-zA-Z0-9_.-]*|'') echo "invalid local-CI run id" >&2; exit 2 ;; esac

repo=$(cd "$(dirname "$0")/../.." && pwd)
run_dir="$repo/.local-ci/runs/$run_id"
prefix="iron-ci-$run_id"
network="$prefix-net"
image=${CI_IMAGE:-localhost/iron-dev:latest}
memory=${CI_MEMORY:-24g}
mysql_image=${CI_MYSQL_IMAGE:-docker.io/library/mysql:8.4}
mysql_memory=${CI_MYSQL_MEMORY:-3g}
legs=(asan release leaks)

cleanup() {
    local leg
    for leg in "${legs[@]}"; do
        podman rm -f "$prefix-$leg" "$prefix-mysql-$leg" >/dev/null 2>&1 || true
    done
    podman network rm "$network" >/dev/null 2>&1 || true
}

status() {
    local leg state outcome exit_code
    for leg in "${legs[@]}"; do
        if [ -f "$run_dir/$leg.result" ]; then
            outcome=$(sed -n 's/^status=//p' "$run_dir/$leg.result" | head -1)
            echo "$leg ${outcome:-invalid-result}"
        else
            state=$(podman inspect -f '{{.State.Status}}' "$prefix-$leg" 2>/dev/null || true)
            case "$state" in
                exited|dead)
                    exit_code=$(podman inspect -f '{{.State.ExitCode}}' "$prefix-$leg" 2>/dev/null || echo 125)
                    {
                        echo "status=fail"
                        echo "exit_code=$exit_code"
                        echo "leg=$leg"
                        echo "failure=container exited without a result file"
                    } > "$run_dir/$leg.result"
                    echo "$leg fail"
                    ;;
                "")
                    if [ -f "$run_dir/launched" ]; then
                        {
                            echo "status=fail"
                            echo "exit_code=125"
                            echo "leg=$leg"
                            echo "failure=container disappeared without a result file"
                        } > "$run_dir/$leg.result"
                        echo "$leg fail"
                    else
                        echo "$leg not-started"
                    fi
                    ;;
                *) echo "$leg $state" ;;
            esac
        fi
    done
}

case "$action" in
cleanup)
    cleanup
    exit 0
    ;;
status)
    status
    exit 0
    ;;
start)
    if [ "$#" -ne 6 ]; then
        echo "start requires ASAN_JOBS RELEASE_JOBS LEAK_JOBS SCALE" >&2
        exit 2
    fi
    asan_jobs=$3
    release_jobs=$4
    leak_jobs=$5
    scale=$6
    mkdir -p "$run_dir" "$HOME/.iron"
    {
        echo "run_id=$run_id"
        echo "network=$network"
        echo "asan_jobs=$asan_jobs"
        echo "release_jobs=$release_jobs"
        echo "leak_jobs=$leak_jobs"
        echo "timeout_scale=$scale"
        echo "started_at=$(date -u +%FT%TZ)"
    } > "$run_dir/manifest"

    cleanup
    failed=1
    trap 'rc=$?; [ "$failed" -eq 0 ] || cleanup; exit "$rc"' EXIT
    podman network create "$network" >/dev/null

    for leg in "${legs[@]}"; do
        podman run -d --name "$prefix-mysql-$leg" --network "$network" \
            --memory="$mysql_memory" \
            -e MYSQL_ROOT_PASSWORD=ironci -e MYSQL_DATABASE=iron_test \
            "$mysql_image" --max-allowed-packet=67108864 \
            --mysql-native-password=ON >/dev/null
    done

    # A successful ping can still precede usable TLS and table operations.
    # Exercise the same TCP/TLS/query path the tests need before any leg starts.
    for leg in "${legs[@]}"; do
        ready=0
        for _ in $(seq 1 120); do
            if podman exec "$prefix-mysql-$leg" mysql --protocol=TCP \
                -h127.0.0.1 -uroot -pironci iron_test -e \
                'DROP TABLE IF EXISTS iron_ci_ready; CREATE TABLE iron_ci_ready (n INT); INSERT INTO iron_ci_ready VALUES (1); SELECT n FROM iron_ci_ready; DROP TABLE iron_ci_ready;' \
                >/dev/null 2>&1; then
                ready=1
                break
            fi
            sleep 2
        done
        if [ "$ready" -ne 1 ]; then
            echo "mysql for $leg did not become query-ready" >&2
            exit 1
        fi
        podman cp "$prefix-mysql-$leg:/var/lib/mysql/ca.pem" \
            "$run_dir/mysql-$leg-ca.pem"
    done

    index=0
    for leg in "${legs[@]}"; do
        case "$leg" in
            asan) jobs=$asan_jobs ;;
            release) jobs=$release_jobs ;;
            leaks) jobs=$leak_jobs ;;
        esac
        podman run -d --name "$prefix-$leg" --network "$network" \
            --memory="$memory" \
            -e CI_JOBS="$jobs" -e CI_RETRY_JOBS="${CI_RETRY_JOBS:-2}" \
            -e IRON_TEST_TIMEOUT_SCALE="$scale" \
            -e CI_RESULT_DIR="/work/.local-ci/runs/$run_id" \
            -e CI_BUILD_LOCK="/work/.local-ci/runs/$run_id/build.lock" \
            -e IRON_MYSQL_URL="mysql://root:ironci@$prefix-mysql-$leg:3306/iron_test" \
            -e IRON_MYSQL_CA="/work/.local-ci/runs/$run_id/mysql-$leg-ca.pem" \
            -v "$repo:/work:Z" -v "$HOME/.iron:/root/.iron:Z" -w /work \
            "$image" bash -c \
            "exec bash scripts/ci/run_linux_leg.sh $leg >.local-ci/runs/$run_id/$leg.log 2>&1" \
            >/dev/null
        # Avoid three simultaneous container startup bursts. Compilation is
        # additionally serialized by CI_BUILD_LOCK inside each container.
        index=$((index + 1))
        [ "$index" -eq 3 ] || sleep 3
    done
    : > "$run_dir/launched"
    failed=0
    status
    ;;
*)
    echo "unknown remote action: $action" >&2
    exit 2
    ;;
esac
