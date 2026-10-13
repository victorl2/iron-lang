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

# A stray positional after the source file is an error, not silently
# dropped (issue 454); program arguments of run go after --.
for bin in "${IRON}" "${IRONC}"; do
    if out=$("${bin}" run hello.iron extra 2>&1); then fail "'${bin} run hello.iron extra' exited 0"; fi
    echo "${out}" | grep -qF "unexpected argument 'extra'" \
        || fail "'${bin} run hello.iron extra' printed: ${out}"
    echo "${out}" | grep -qF "after --" \
        || fail "'${bin} run hello.iron extra' did not point at --: ${out}"
    if out=$("${bin}" check hello.iron extra 2>&1); then fail "'${bin} check hello.iron extra' exited 0"; fi
    echo "${out}" | grep -qF "unexpected argument 'extra'" || fail "check printed: ${out}"
done
if out=$("${IRONC}" build hello.iron extra 2>&1); then fail "'ironc build hello.iron extra' exited 0"; fi
echo "${out}" | grep -qF "unexpected argument 'extra'" || fail "build printed: ${out}"
"${IRONC}" run --help 2>&1 | grep -qF -- "-- program arguments" || fail "run --help does not mention --"

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

# A freshly scaffolded project is already formatted (issue 241: the
# formatter's default indentation is the repository's four spaces).
FMT_DIR="${SANDBOX}/fmt_default"
mkdir -p "${FMT_DIR}"
(cd "${FMT_DIR}" && "${IRON}" init >/dev/null 2>&1)
if ! (cd "${FMT_DIR}" && "${IRON}" fmt --check src/main.iron >/dev/null 2>&1); then
    echo "FAIL: iron fmt --check reformats the scaffolded src/main.iron" >&2
    exit 1
fi

# IRON_WORKERS sizes the task pool (#547): 8 tasks that each sleep 200 ms
# overlap with 8 workers and run one after another with 1.
cat > "${SANDBOX}/workers.iron" <<'IRONEOF'
import atomic
import time

func nap(done: AtomicInt) -> Int {
    Time.sleep(200)
    done.add(1)
    return 0
}

func main() {
    val done = AtomicInt.new(0)
    val t0 = Time.now_ms()
    for i in range(8) {
        spawn("nap") {
            return nap(done)
        }
    }
    while done.get() < 8 {
        Time.sleep(5)
    }
    println("{Time.now_ms() - t0 < 700}")
}
IRONEOF
"${IRONC}" build "${SANDBOX}/workers.iron" -o "${SANDBOX}/workers" >/dev/null 2>&1 || fail "build workers.iron"
[ "$(IRON_WORKERS=8 "${SANDBOX}/workers")" = "true" ] || fail "IRON_WORKERS=8 did not run 8 tasks at once"
[ "$(IRON_WORKERS=1 "${SANDBOX}/workers")" = "false" ] || fail "IRON_WORKERS=1 did not serialize the tasks"

echo "cli_args_smoke OK"
