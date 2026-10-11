#!/bin/bash
# run.sh - MySQL client integration tests against a live server.
#
#   tests/mysql/run.sh <ironc> [test ...]     (default: every tests/mysql/*.iron)
#
# environment:
#   IRON_MYSQL_URL    mysql://user:password@host:port/database for an account
#                     that may create users, tables and procedures in that
#                     database. Unset: every test is skipped (exit 77, which
#                     ctest reports as skipped).
#   IRON_MYSQL_CA     the server's CA certificate (enables the verify_ca and
#                     verify_identity checks in auth.iron)
#   IRON_MYSQL_LEAKS  1: run each test under valgrind (Linux) or leaks
#                     (macOS) and fail on a leak or memory error
#
# A test passes when it exits 0 and its last line starts with PASS (or SKIP).
set -u
here=$(cd "$(dirname "$0")" && pwd)
if [ $# -lt 1 ]; then sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; fi
IRONC=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
if [ -z "${IRON_MYSQL_URL:-}" ]; then
    echo "SKIP: IRON_MYSQL_URL is not set; MySQL integration tests not run"
    exit 77
fi
tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
    for f in "$here"/*.iron; do tests+=("$(basename "$f" .iron)"); done
fi

detector=""
if [ "${IRON_MYSQL_LEAKS:-0}" = 1 ]; then
    if [ "$(uname -s)" = Darwin ] && command -v leaks >/dev/null 2>&1; then detector=leaks
    elif command -v valgrind >/dev/null 2>&1; then detector=valgrind
    else echo "run.sh: IRON_MYSQL_LEAKS=1 but neither leaks nor valgrind is installed" >&2; exit 2; fi
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/iron-mysql.XXXXXX")
trap 'rm -rf "$work"' EXIT
failed=0
for t in "${tests[@]}"; do
    src="$here/$t.iron"
    [ -f "$src" ] || { echo "[FAIL] $t (no such test)"; failed=1; continue; }
    bin="$work/$t"
    start=$(date +%s)
    if ! "$IRONC" build "$src" -o "$bin" >"$work/$t.build" 2>&1; then
        echo "[FAIL] $t (build failed)"; sed 's/^/    /' "$work/$t.build"; failed=1; continue
    fi
    case $detector in
        valgrind)
            valgrind --quiet --leak-check=full --errors-for-leak-kinds=definite,indirect \
                --error-exitcode=99 "$bin" >"$work/$t.out" 2>"$work/$t.err"; rc=$? ;;
        leaks)
            MallocStackLogging=1 leaks --atExit -- "$bin" >"$work/$t.out" 2>"$work/$t.err"; rc=$?
            if ! grep -qE ': 0 leaks for 0 total leaked bytes' "$work/$t.out"; then rc=99; fi ;;
        *)
            "$bin" >"$work/$t.out" 2>"$work/$t.err"; rc=$? ;;
    esac
    secs=$(( $(date +%s) - start ))
    last=$(grep -E '^(PASS|SKIP|FAILED)' "$work/$t.out" | tail -1)
    if [ $rc -eq 0 ] && { [[ $last == PASS* ]] || [[ $last == SKIP* ]]; }; then
        echo "[${last%% *}] $t (${secs}s)"
    else
        echo "[FAIL] $t (exit $rc, ${secs}s)"
        grep -E '^FAIL' "$work/$t.out" | sed 's/^/    /'
        tail -40 "$work/$t.err" | sed 's/^/    /'
        failed=1
    fi
done
exit $failed
