#!/usr/bin/env bash
# Packages and the iron project tool, regressions found by
# tests/fuzz/mod_fuzz.py:
#   1. .iron files in subdirectories of src/ are compiled, and `import a.b`
#      finds src/a/b.iron (#534); a file without declarations can be
#      imported too.
#   2. comptime read_file resolves against the calling file's directory,
#      from any working directory (#535).
#   3. Globals are initialized after the globals they read through
#      functions of other files (#549).
#   4. Builds and checks of one package running at the same time do not
#      read each other's half-written target/combined.iron.
#   5. A source file name the file marker cannot quote is refused.
#
# Usage: package_modules_smoke.sh IRON_BIN
set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
export PATH="$(dirname "${IRON}"):${PATH}"

WORK="$(mktemp -d -t iron-pkg-smoke-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

P="${WORK}/my pkg é"
mkdir -p "${P}/src/geom/shapes" "${P}/src/data"
cd "${P}"
printf '[package]\nname = "app"\nversion = "0.1.0"\n' > iron.toml
cat > src/geom/shapes/square.iron <<'EOF'
pub func square(x: Int) -> Int {
    return x * x
}
EOF
printf -- '-- nothing here yet\n' > src/geom/empty.iron
printf hello > src/data/greeting.txt
cat > src/data/load.iron <<'EOF'
pub func greeting() -> String {
    return read_file("greeting.txt")
}
EOF
cat > src/b_derived.iron <<'EOF'
val DERIVED = base_value() * 2 + 1

pub func derived() -> Int {
    return DERIVED
}
EOF
cat > src/z_base.iron <<'EOF'
val BASE = 20

pub func base_value() -> Int {
    return BASE
}
EOF
cat > src/main.iron <<'EOF'
import geom.shapes.square
import geom.empty
import data.load

val GREETING = comptime greeting()

func main() {
    println("{square(7)} {derived()} {GREETING}")
}
EOF
out="$("${IRON}" run 2>build.log)" || { cat build.log; fail "iron run"; }
[ "${out}" = "49 41 hello" ] || fail "unexpected output: ${out}"
(cd src/geom && "${IRON}" build >/dev/null 2>&1) || fail "iron build from src/geom"
(cd src/geom && "${IRON}" check >/dev/null 2>&1) || fail "iron check from src/geom"

# concurrent builds and checks of the same package
python3 - <<'EOF'
for k in range(3):
    with open(f'src/big{k}.iron', 'w') as f:
        for i in range(1500):
            f.write(f'pub func big{k}_{i}(x: Int) -> Int {{\n    return x + {i}\n}}\n\n')
EOF
for round in 1 2 3; do
    pids=()
    for i in 1 2 3 4 5 6 7 8; do
        if [ $((i % 4)) = 0 ]; then cmd=build; else cmd=check; fi
        "${IRON}" "${cmd}" > "${WORK}/c${i}.log" 2>&1 &
        pids+=($!)
    done
    for i in "${!pids[@]}"; do
        wait "${pids[$i]}" || { cat "${WORK}/c$((i + 1)).log"; fail "concurrent build $((i + 1))"; }
    done
done
ls target | grep -q '\.[0-9]*\.iron$' && fail "temporary combined file left in target/"

# a file name the marker cannot quote
printf 'pub func q() -> Int {\n    return 1\n}\n' > 'src/a"b.iron'
if "${IRON}" build > q.log 2>&1; then fail "a file name with a double quote built"; fi
grep -q 'may not contain a double quote' q.log || { cat q.log; fail "no message for the quoted file name"; }

# a single-file package: import does not match directories above it (#603)
mkdir -p "${WORK}/solo/src" && cd "${WORK}/solo"
printf '[package]\nname = "solo"\nversion = "0.1.0"\n' > iron.toml
printf 'import solo\n\nfunc main() {\n    println("x")\n}\n' > src/main.iron
if "${IRON}" build > solo.log 2>&1; then fail "import solo matched the package directory"; fi
grep -q 'E0209' solo.log || { cat solo.log; fail "import solo: no E0209"; }

echo "package_modules_smoke OK"
