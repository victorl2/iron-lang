#!/bin/bash
# run.sh - build apps/inventory and run its integration suite against a live
# MySQL server.
#
#   apps/inventory/tests/run.sh <iron> [integration.py args...]
#
# <iron> is the project driver (build/iron); ironc is taken from the same
# directory. The suite creates and drops its own databases on the server.
#
# environment:
#   IRON_MYSQL_URL      mysql://user:password@host:port/database for an account
#                       that may create and drop databases. Unset: the suite is
#                       skipped (exit 77, which ctest reports as skipped).
#   INVENTORY_SANITIZE  1: build the service with AddressSanitizer and
#                       UndefinedBehaviorSanitizer (IRON_CFLAGS) and fail on
#                       any report
#   INVENTORY_VALGRIND  1: run the service under valgrind (Linux) and fail on
#                       a leak or memory error
set -u
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
if [ $# -lt 1 ]; then sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; fi
IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
IRONC=$(dirname "$IRON")/ironc
if [ -z "${IRON_MYSQL_URL:-}" ]; then
    echo "SKIP: IRON_MYSQL_URL is not set; inventory integration suite not run"
    exit 77
fi
command -v python3 >/dev/null || { echo "run.sh: python3 is required" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/inventory-it.XXXXXX")
trap 'rm -rf "$work"' EXIT
# Build copies of the sources so parallel runs do not share target/.
cp -R "$app/src" "$app/iron.toml" "$work/" && mkdir -p "$work/stub" &&
    cp -R "$app/stub/src" "$app/stub/iron.toml" "$work/stub/" || exit 1

build() {   # $1 = project dir, $2 = log name
    (cd "$1" && "$IRON" build "${@:3}") >"$work/$2.build" 2>&1 ||
        { echo "[FAIL] build $2"; sed 's/^/    /' "$work/$2.build"; exit 1; }
}
wrap=""
if [ "${INVENTORY_SANITIZE:-0}" = 1 ]; then
    export IRON_CFLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined"
    export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}
    case "$(uname -s)" in Darwin) ASAN_OPTIONS=${ASAN_OPTIONS/detect_leaks=1/detect_leaks=0} ;; esac
    export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}
    if [ -z "${ASAN_SYMBOLIZER_PATH:-}" ]; then
        for s in llvm-symbolizer llvm-symbolizer-20 llvm-symbolizer-19 llvm-symbolizer-18; do
            command -v $s >/dev/null && { export ASAN_SYMBOLIZER_PATH=$(command -v $s); break; }
        done
    fi
fi
build "$work" service
build "$work/stub" stub
unset IRON_CFLAGS
"$IRONC" build "$here/dbtool.iron" -o "$work/dbtool" >"$work/dbtool.build" 2>&1 ||
    { echo "[FAIL] build dbtool"; sed 's/^/    /' "$work/dbtool.build"; exit 1; }
if [ "${INVENTORY_VALGRIND:-0}" = 1 ]; then
    command -v valgrind >/dev/null || { echo "run.sh: INVENTORY_VALGRIND=1 but valgrind is not installed" >&2; exit 2; }
    wrap="valgrind --quiet --leak-check=full --errors-for-leak-kinds=definite,indirect --show-leak-kinds=definite,indirect --error-exitcode=99"
fi
python3 "$here/integration.py" --service "$work/target/inventory" --stub "$work/stub/target/fxstub" \
    --dbtool "$work/dbtool" --mysql-url "$IRON_MYSQL_URL" --wrap "$wrap" "$@"
