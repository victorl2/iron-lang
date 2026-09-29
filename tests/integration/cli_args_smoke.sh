#!/usr/bin/env bash
# CLI argument handling: unknown flags are errors in any position (they
# used to be taken as the source path, or ignored after it), ironc's help
# does not list the iron-only `init`, `iron run -o` keeps the binary, and
# `iron init <name>` scaffolds <name>/ with a package of that name whose
# version pin the running compiler satisfies.
#
# Usage: cli_args_smoke.sh [IRON_BIN]

set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON_BIN_DIR="$(dirname "${IRON_BIN_ABS}")"
IRON="${IRON_BIN_DIR}/iron"
IRONC="${IRON_BIN_DIR}/ironc"

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT
cd "${SANDBOX}"
printf 'func main() {\n    println("hi")\n}\n' > hello.iron

fail() { echo "FAIL: $*"; exit 1; }

expect_unknown_flag() {
    local flag="$1"; shift
    local out
    if out=$("$@" 2>&1); then fail "'$*' exited 0"; fi
    echo "${out}" | grep -qF "unknown flag '${flag}'" || fail "'$*' printed: ${out}"
}

expect_unknown_flag --bogus-flag "${IRON}" run --bogus-flag hello.iron
expect_unknown_flag --bogus-flag "${IRON}" run hello.iron --bogus-flag
expect_unknown_flag --keep-binary "${IRON}" run --keep-binary hello.iron
expect_unknown_flag --frobnicate "${IRON}" check --frobnicate hello.iron
expect_unknown_flag --bogus "${IRONC}" build hello.iron --bogus

# Program arguments after -- are not flags.
[ "$("${IRON}" run hello.iron -- --not-a-flag)" = "hi" ] || fail "run with -- args"
# fmt --check is still accepted.
fmt_out=$("${IRON}" fmt --check hello.iron 2>&1 || true)
echo "${fmt_out}" | grep -q "unknown flag" && fail "fmt --check rejected: ${fmt_out}"

"${IRONC}" --help | grep -q "^  init " && fail "ironc help lists init"

"${IRON}" run -o kept hello.iron > /dev/null || fail "run -o failed"
[ -x kept ] || fail "run -o did not keep the binary"

# run --debug-build builds into a temp directory; the debug allocator's
# leak report follows the program's own output.
printf 'object W { val n: Int }\nfunc main() {\n    val w = heap W(3)\n    println("n {w.n}")\n}\n' > leaky.iron
dbg_out=$("${IRON}" run --debug-build leaky.iron 2>&1) || fail "run --debug-build: ${dbg_out}"
echo "${dbg_out}" | grep -A1 '^n 3$' | grep -q 'leaked at exit' \
    || fail "run --debug-build leak report: ${dbg_out}"

mkdir work && cd work
"${IRON}" init myapp > /dev/null || fail "iron init myapp failed"
grep -q '^name = "myapp"' myapp/iron.toml || fail "package not named myapp"
[ -f myapp/src/main.iron ] || fail "myapp/src/main.iron missing"
[ ! -f iron.toml ] || fail "iron init myapp scaffolded into the current directory"
if "${IRON}" init a b > /dev/null 2>&1; then fail "iron init a b accepted"; fi
if "${IRON}" init myapp > /dev/null 2>&1; then fail "iron init over an existing directory accepted"; fi
(cd myapp && [ "$("${IRON}" run 2>/dev/null | tail -1)" = "Hello, Iron!" ]) \
    || fail "scaffolded package does not build with its own version pin"
expect_unknown_flag --bogus bash -c "cd myapp && '${IRON}' run --bogus"

echo "cli_args_smoke OK"
