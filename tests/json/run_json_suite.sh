#!/bin/bash
# run_json_suite.sh - JSONTestSuite conformance for the json module.
#
# Builds json_suite.iron and runs it over tests/json/JSONTestSuite/test_parsing
# (nst/JSONTestSuite, MIT). y_ files must be accepted (and round-trip), n_
# files must be rejected, i_ files may go either way but must not crash; the
# i_ verdicts are compared with i_expected.txt so a behaviour change shows.
#
# usage: tests/json/run_json_suite.sh <ironc>
set -u
here=$(cd "$(dirname "$0")" && pwd)
ironc=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_json_suite_XXXXXX")
trap 'rm -rf "$work"' EXIT

(cd "$work" && "$ironc" build "$here/json_suite.iron" -o "$work/json_suite" >/dev/null 2>&1) || {
    echo "json suite: build failed"; exit 1; }

out="$work/results.txt"
"$work/json_suite" $(LC_ALL=C ls "$here"/JSONTestSuite/test_parsing/*.json) > "$out" 2>"$work/stderr.txt"
status=$?
if [ $status -ne 0 ]; then
    echo "json suite: driver exited with $status"; cat "$work/stderr.txt"; tail -3 "$out"; exit 1
fi

fail=0; y=0; n=0; i=0
while read -r name verdict rest; do
    case "$name" in
        y_*) y=$((y + 1))
             if [ "$verdict" != accept ]; then echo "FAIL $name: expected accept, got $verdict $rest"; fail=1; fi ;;
        n_*) n=$((n + 1))
             if [ "$verdict" != reject ]; then echo "FAIL $name: expected reject, got $verdict $rest"; fail=1; fi ;;
        i_*) i=$((i + 1))
             if [ "$verdict" = roundtrip-fail ] || [ "$verdict" = unreadable ]; then
                 echo "FAIL $name: $verdict $rest"; fail=1; fi ;;
    esac
done < "$out"

total=$(ls "$here"/JSONTestSuite/test_parsing/*.json | wc -l | tr -d ' ')
seen=$(wc -l < "$out" | tr -d ' ')
if [ "$seen" != "$total" ]; then echo "FAIL: $seen results for $total files"; fail=1; fi

grep '^i_' "$out" | awk '{print $1, $2}' | LC_ALL=C sort > "$work/i_actual.txt"
if ! diff -u "$here/i_expected.txt" "$work/i_actual.txt"; then
    echo "FAIL: implementation-defined verdicts changed (update i_expected.txt if intended)"; fail=1
fi

echo "json suite: y=$y n=$n i=$i files, $( [ $fail -eq 0 ] && echo PASS || echo FAIL )"
exit $fail
