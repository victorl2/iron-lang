#!/usr/bin/env bash
# Golden tests for `ironc fmt --migrate-comments` (src/fmt/migrate.c).
# For every <case>.iron here:
#   - the migrated copy must equal <case>.expected byte for byte,
#   - a second migration must change nothing (idempotence),
#   - `--check` must report the original and pass on the migrated copy,
#   - when the case has a `func main`, the original and the migrated copy
#     must print the same.
# <case>.refuse files must be left alone with a nonzero exit.
# Usage: run.sh <ironc>
set -u
ironc=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf "$work"' EXIT; [ -n "${KEEP:-}" ] && echo "work: $work"
fail=0
for src in "$here"/*.iron; do
    name=$(basename "$src" .iron)
    bad=$fail
    cp "$src" "$work/$name.iron"
    if ! cmp -s "$src" "$here/$name.expected" &&
       "$ironc" fmt --migrate-comments --check "$work/$name.iron" >/dev/null 2>&1; then
        echo "FAIL $name: --check did not report the original"; fail=1
    fi
    if ! "$ironc" fmt --migrate-comments "$work/$name.iron" >/dev/null 2>"$work/$name.err"; then
        echo "FAIL $name: migration failed: $(cat "$work/$name.err")"; fail=1; continue
    fi
    if ! cmp -s "$work/$name.iron" "$here/$name.expected"; then
        echo "FAIL $name: differs from $name.expected"
        diff "$here/$name.expected" "$work/$name.iron" | head -20
        fail=1; continue
    fi
    cp "$work/$name.iron" "$work/$name.again.iron"
    "$ironc" fmt --migrate-comments "$work/$name.again.iron" >/dev/null 2>&1
    if ! cmp -s "$work/$name.iron" "$work/$name.again.iron" ||
       ! "$ironc" fmt --migrate-comments --check "$work/$name.again.iron" >/dev/null 2>&1; then
        echo "FAIL $name: a second migration changed the file"; fail=1
    fi
    if grep -q '^func main' "$src"; then
        (cd "$work" && cp "$src" orig_$name.iron &&
         "$ironc" run "orig_$name.iron" >"$name.before" 2>&1;
         "$ironc" run "$name.iron" >"$name.after" 2>&1)
        if ! cmp -s "$work/$name.before" "$work/$name.after"; then
            echo "FAIL $name: the migrated program prints something else"; fail=1
        fi
    fi
    [ "$fail" = "$bad" ] && echo "ok   $name"
done
for src in "$here"/*.refuse; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .refuse)
    cp "$src" "$work/$name.iron"
    if "$ironc" fmt --migrate-comments "$work/$name.iron" >/dev/null 2>&1 ||
       ! cmp -s "$src" "$work/$name.iron"; then
        echo "FAIL $name: should have been refused and left alone"; fail=1
    else
        echo "ok   $name (refused)"
    fi
done

# An explicit path is subject to the same .iron filter as files discovered
# below a directory. In particular, migration must not rewrite arbitrary text.
printf '%s\n' '-- keep this text' > "$work/README.txt"
cp "$work/README.txt" "$work/README.before"
if "$ironc" fmt --migrate-comments "$work/README.txt" >/dev/null 2>&1 ||
   ! cmp -s "$work/README.before" "$work/README.txt"; then
    echo "FAIL non_iron: explicit non-.iron file should be refused and left alone"
    fail=1
else
    echo "ok   non_iron (refused)"
fi
exit $fail
