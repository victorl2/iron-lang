#!/bin/bash
# Run one benchmark process and append its validated RESULT line. Intentional
# SKIP output is accepted; process failures and missing or malformed output
# fail the sweep instead of silently leaving a table cell empty.
set -u
if [ $# -lt 7 ]; then
    echo "usage: capture.sh <out> <variant> <lib> <mode> <suffix> <command> [args...]" >&2
    exit 2
fi
out=$1 variant=$2 lib=$3 mode=$4 suffix=$5
shift 5
raw=$(
    "$@" 2>&1
)
status=$?
if [ $status -ne 0 ]; then
    echo "run.sh: $variant $lib $mode failed with status $status" >&2
    printf '%s\n' "$raw" >&2
    exit $status
fi
lines=$(printf '%s\n' "$raw" | grep -E '^(RESULT|SKIP) ' || true)
count=$(printf '%s\n' "$lines" | grep -c . || true)
if [ "$count" -ne 1 ]; then
    echo "run.sh: $variant $lib $mode produced $count RESULT/SKIP lines" >&2
    printf '%s\n' "$raw" >&2
    exit 1
fi
set -- $lines
case $1 in
RESULT)
    if [ $# -ne 6 ] || [ "$3" != "$mode" ]; then
        echo "run.sh: malformed result for $variant $lib $mode: $lines" >&2
        exit 1
    fi
    echo "RESULT $variant $lib ${3}${suffix} $4 $5 $6" >> "$out"
    ;;
SKIP)
    if [ $# -ne 3 ] || [ "$3" != "$mode" ]; then
        echo "run.sh: malformed skip for $variant $lib $mode: $lines" >&2
        exit 1
    fi
    ;;
*)
    echo "run.sh: unexpected result for $variant $lib $mode: $lines" >&2
    exit 1
    ;;
esac
