#!/bin/bash
# run_conformance.sh - JSON spec conformance (RFC 8259 / ECMA-404) for the
# json module: writes the case manifests with gen_cases.py, runs them through
# conformance.iron, then runs semantics.iron. Prints one line per category
# and every failing case; exits non-zero on any failure.
#
# usage: tests/json/conformance/run_conformance.sh <ironc> [--random N]
set -u
here=$(cd "$(dirname "$0")" && pwd)
ironc=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_json_conf_XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 "$here/gen_cases.py" "$work/cases" "$@" >/dev/null || { echo "conformance: gen_cases.py failed"; exit 1; }
for prog in conformance semantics; do
    (cd "$work" && "$ironc" build "$here/$prog.iron" -o "$work/$prog" >"$work/$prog.build.log" 2>&1) || {
        echo "conformance: building $prog.iron failed"; cat "$work/$prog.build.log"; exit 1; }
done
"$work/conformance" "$work"/cases/*.tsv >"$work/results.txt" 2>&1
status=$?
"$work/semantics" >"$work/semantics.txt" 2>&1
sstatus=$?
fail=0
if [ $status -ne 0 ]; then echo "conformance driver exited with $status"; tail -5 "$work/results.txt"; fail=1; fi
if [ $sstatus -ne 0 ]; then echo "semantics driver exited with $sstatus"; tail -5 "$work/semantics.txt"; fail=1; fi
grep '^FAIL' "$work/results.txt" | head -50
grep '^FAIL' "$work/semantics.txt"
grep -q '^FAIL' "$work/results.txt" "$work/semantics.txt" && fail=1
printf '%-16s %8s %8s\n' category passed cases
while read -r _ cat passed total; do
    printf '%-16s %8s %8s\n' "$cat" "$passed" "$total"
done < <(grep '^SUMMARY' "$work/results.txt")
spass=$(grep -c '^PASS' "$work/semantics.txt")
stotal=$(grep -cE '^(PASS|FAIL)' "$work/semantics.txt")
printf '%-16s %8s %8s\n' semantics "$spass" "$stotal"
ncat=$(grep -c '^SUMMARY' "$work/results.txt")
if [ "$ncat" -ne 8 ]; then echo "conformance: expected 8 categories, got $ncat"; fail=1; fi
if [ "$stotal" -lt 40 ]; then echo "conformance: semantics ran only $stotal checks"; fail=1; fi
echo "json conformance: $( [ $fail -eq 0 ] && echo PASS || echo FAIL )"
exit $fail
