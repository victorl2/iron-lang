#!/usr/bin/env bash
# Vendoring smoke: Iron has no package manager. Third-party code is copied
# into a project's vendor/ directory and `iron build` compiles it together
# with the project.
#
# Usage: vendor_smoke.sh [IRON_BIN]
#   IRON_BIN: path to the iron binary (default: ./build/iron)
#
# Asserts:
#   1. iron init writes no [dependencies] table.
#   2. A library project copied into vendor/<name>/ builds and runs; only its
#      src/ is compiled (its tests/ directory is ignored).
#   3. Loose .iron files under vendor/, including nested directories, are
#      compiled.
#   4. iron check sees vendored declarations.
#   5. A legacy empty [dependencies] table warns but still builds.
#   6. A legacy [dependencies] entry fails with a hint that points at vendor/.
#   7. No iron.lock is ever written.

set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON_BIN_DIR="$(dirname "${IRON_BIN_ABS}")"

# Ensure iron finds the build-tree ironc, not a stale system install on PATH.
export PATH="${IRON_BIN_DIR}:${PATH}"

WORK="$(mktemp -d -t iron-vendor-smoke-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

fail() { echo "FAIL: $*"; exit 1; }

# ── A library, scaffolded the normal way ────────────────────────────────────
mkdir "${WORK}/greeter"
cd "${WORK}/greeter"
"${IRON_BIN_ABS}" init --lib > /dev/null 2>&1
cat > src/lib.iron <<'EOF'
pub func greet(name: String) -> String {
    return "Hello, {name}!"
}
EOF
mkdir tests
echo 'this is not valid iron' > tests/ignored.iron

# ── The application that vendors it ─────────────────────────────────────────
mkdir "${WORK}/app"
cd "${WORK}/app"
"${IRON_BIN_ABS}" init > /dev/null 2>&1
grep -q '^\[dependencies\]' iron.toml && fail "iron init still writes [dependencies]"

mkdir -p vendor/strutil/nested
cp -R "${WORK}/greeter" vendor/greeter
cat > vendor/strutil/shout.iron <<'EOF'
pub func shout(s: String) -> String {
    return "{s}!"
}
EOF
cat > vendor/strutil/nested/twice.iron <<'EOF'
pub func twice(s: String) -> String {
    return "{s}{s}"
}
EOF
cat > src/main.iron <<'EOF'
import greeter

func main() {
    println(greet("vendor"))
    println(shout(twice("ab")))
}
EOF

"${IRON_BIN_ABS}" build > build.log 2>&1 || { cat build.log; fail "iron build with vendor/ failed"; }
out="$(./target/app)"
[ "${out}" = "$(printf 'Hello, vendor!\nabab!')" ] || fail "unexpected output: ${out}"

"${IRON_BIN_ABS}" check > check.log 2>&1 || { cat check.log; fail "iron check with vendor/ failed"; }

# ── Legacy manifests ────────────────────────────────────────────────────────
printf '\n[dependencies]\nraylib = true\n' >> iron.toml
"${IRON_BIN_ABS}" build > legacy_empty.log 2>&1 \
    || { cat legacy_empty.log; fail "empty legacy [dependencies] should still build"; }
grep -q 'no package manager' legacy_empty.log \
    || { cat legacy_empty.log; fail "empty legacy [dependencies] should warn"; }

printf 'iron-ecs = { git = "owner/iron-ecs", version = "0.2.0" }\n' >> iron.toml
set +e
"${IRON_BIN_ABS}" build > legacy_entry.log 2>&1
rc=$?
set -e
[ "${rc}" = "1" ] || { cat legacy_entry.log; fail "legacy dependency entry should exit 1, got ${rc}"; }
grep -q "declares dependency 'iron-ecs'" legacy_entry.log \
    || { cat legacy_entry.log; fail "missing legacy dependency error"; }
grep -q 'vendor/iron-ecs/' legacy_entry.log \
    || { cat legacy_entry.log; fail "legacy dependency error should point at vendor/"; }

[ ! -e iron.lock ] || fail "iron.lock must never be written"

echo "vendor_smoke OK"
