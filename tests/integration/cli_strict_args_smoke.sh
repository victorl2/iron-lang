#!/usr/bin/env bash
# Command line and manifest rules found by tests/fuzz/cli_fuzz.py
# (#564, #565, #566).
# Usage: cli_strict_args_smoke.sh IRON_BIN
set -uo pipefail
IRON="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
IRONC="$(dirname "${IRON}")/ironc"
export PATH="$(dirname "${IRON}"):${PATH}"
WORK="$(mktemp -d -t iron-cli-strict-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
fail() { echo "FAIL: $*"; exit 1; }
# expect_fail <regex> cmd...: the command fails and its output matches
expect_fail() {
    local re=$1; shift
    local out
    if out="$("$@" 2>&1)"; then fail "succeeded: $*"; fi
    grep -qE -- "${re}" <<< "${out}" || { echo "${out}"; fail "no '${re}' from: $*"; }
}
cd "${WORK}"
printf 'func main() {\n    println("ok")\n}\n' > ok.iron
printf 'test "a" {\n    assert(1 == 2)\n}\ntest "-dash" {\n    assert(true)\n}\n' > t.iron
mkdir adir

expect_fail "unexpected argument 'b.iron'" "${IRONC}" build ok.iron b.iron
expect_fail "unexpected argument 'x'" "${IRONC}" check ok.iron x
expect_fail "after --" "${IRONC}" run ok.iron x
expect_fail "is a directory" "${IRONC}" build adir
expect_fail "is a directory" "${IRONC}" check adir
expect_fail "standard input" "${IRONC}" build -
expect_fail "unknown flag '-o' for command 'fmt'" "${IRONC}" fmt -o x ok.iron
"${IRONC}" fmt --check ok.iron >/dev/null 2>&1 || fail "ironc fmt --check ok.iron"
"${IRONC}" fmt ok.iron --check >/dev/null 2>&1 || fail "ironc fmt ok.iron --check"
out="$("${IRONC}" bogus 2>&1)"; grep -q "Subcommands" <<< "${out}" && fail "ironc bogus printed the whole help"
out="$("${IRONC}" test t.iron 2>&1)"; grep -q "assertion failed: t.iron:2" <<< "${out}" || { echo "${out}"; fail "assert location"; }
out="$("${IRONC}" test t.iron -- -dash 2>&1)"; grep -q "1 passed, 0 failed, 1 filtered out" <<< "${out}" || { echo "${out}"; fail "ironc test -- filter"; }

mkdir pkg && cd pkg
printf '[package]\nname = "pkg"\nversion = "0.1.0"\n' > iron.toml
mkdir src && cp ../ok.iron src/main.iron && cp ../t.iron src/t.iron
expect_fail "unexpected argument 'foo'" "${IRON}" build foo
expect_fail "after --" "${IRON}" run foo
expect_fail "unexpected argument 'foo'" "${IRON}" check foo
expect_fail "unknown target" "${IRON}" build --target=../escape
[ -e ../escape ] && fail "--target=../escape made a directory"
"${IRON}" build -o "${WORK}/custom bin" --no-optimize --debug-build --force-comptime >/dev/null 2>&1 || fail "documented flags"
[ "$("${WORK}/custom bin")" = ok ] || fail "-o in a package"
"${IRON}" build --target native >/dev/null 2>&1 || fail "--target with a separate value"
out="$("${IRON}" build --report-bounds-checks 2>&1)" || { echo "${out}"; fail "build --report-bounds-checks"; }
grep -q "bounds check" <<< "${out}" || { echo "${out}"; fail "build did not forward --report-bounds-checks"; }
out="$("${IRON}" run --report-bounds-checks 2>&1)" || { echo "${out}"; fail "run --report-bounds-checks"; }
grep -q "bounds check" <<< "${out}" || { echo "${out}"; fail "run did not forward --report-bounds-checks"; }
out="$("${IRON}" test -- -dash 2>&1)"; grep -q "1 passed, 0 failed, 1 filtered out" <<< "${out}" || { echo "${out}"; fail "iron test -- filter"; }

# manifests
printf '[package]\nname = "a"b"\nversion = "0.1.0"\n' > iron.toml
expect_fail "unexpected text after the closing quote" "${IRON}" build
printf '[package]\nname = "abc\nversion = "0.1.0"\n' > iron.toml
expect_fail "unterminated string" "${IRON}" check
printf '[package]\nname = "../../evil"\nversion = "0.1.0"\n' > iron.toml
expect_fail "invalid package name" "${IRON}" build
printf '[package]\nname = "pkg"\nversion = "0.1.0"\ntype = "binary"\n' > iron.toml
expect_fail "invalid package type" "${IRON}" build
printf '[package]\nname = "q\\"x # not a comment"\nversion = "0.1.0"\n' > iron.toml
expect_fail "invalid package name" "${IRON}" build
mkdir -p '../qu"ote' && cd '../qu"ote' && "${IRON}" init >/dev/null 2>&1 || fail "iron init in qu\"ote"
grep -q '^name = "qu_ote"' iron.toml || { cat iron.toml; fail "iron init name from a quoted directory"; }
"${IRON}" build >/dev/null 2>&1 || fail "build after iron init in qu\"ote"
echo "cli_strict_args_smoke OK"
