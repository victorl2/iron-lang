#!/usr/bin/env bash
# Run CTest once at normal parallelism, then retry only failures with fewer
# workers. The first and retry passes each produce JUnit, and the exact retry
# list is retained beside them.
set -uo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: $0 BUILD_DIR JOBS RESULT_PREFIX [CTEST_ARG ...]" >&2
    exit 2
fi

build_dir=$1
jobs=$2
prefix=$3
shift 3
retry_jobs=${CI_RETRY_JOBS:-2}
[ "$retry_jobs" -gt "$jobs" ] && retry_jobs=$jobs
[ "$retry_jobs" -lt 1 ] && retry_jobs=1
mkdir -p "$(dirname "$prefix")"
: > "$prefix-retried.txt"

ctest --test-dir "$build_dir" --output-on-failure -j"$jobs" \
    --output-junit "$prefix-first.xml" "$@"
first_rc=$?
retry_rc=0

failed_log="$build_dir/Testing/Temporary/LastTestsFailed.log"
if [ "$first_rc" -ne 0 ] && [ -s "$failed_log" ]; then
    sed 's/^[0-9][0-9]*://' "$failed_log" > "$prefix-retried.txt"
    echo "ctest: retrying these tests with -j$retry_jobs:"
    sed 's/^/  /' "$prefix-retried.txt"
    ctest --test-dir "$build_dir" --output-on-failure -j"$retry_jobs" \
        --rerun-failed --output-junit "$prefix-retry.xml"
    retry_rc=$?
elif [ "$first_rc" -ne 0 ]; then
    retry_rc=$first_rc
    echo "ctest: first pass failed without a CTest failed-test list" >&2
fi

if [ "$first_rc" -eq 0 ] || [ "$retry_rc" -eq 0 ]; then
    status=pass
    rc=0
else
    status=fail
    rc=$retry_rc
fi
{
    echo "status=$status"
    echo "first_exit_code=$first_rc"
    echo "retry_exit_code=$retry_rc"
    echo "retry_jobs=$retry_jobs"
} > "$prefix-ctest.result"

if [ -s "$prefix-retried.txt" ]; then
    echo "ctest: second pass required by $(wc -l < "$prefix-retried.txt") test(s):"
    sed 's/^/  /' "$prefix-retried.txt"
else
    echo "ctest: no second pass was needed"
fi
exit "$rc"
