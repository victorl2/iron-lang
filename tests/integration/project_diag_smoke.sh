#!/usr/bin/env bash
# Multi-file package diagnostics: errors point at the real source file and
# its own line numbers (not target/combined.iron), and declarations
# (functions, extern functions, objects, interfaces, fields, methods, inits)
# without pub are private to their file.
#
# Usage: project_diag_smoke.sh [IRON_BIN]

set -euo pipefail

# Output is matched with `grep ... <<< "${out}"`, not `echo | grep -q`: with
# pipefail, grep -q leaving at its first match can kill echo with SIGPIPE
# and fail a check that matched.

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON="$(dirname "${IRON_BIN_ABS}")/iron"

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

new_pkg() {
    rm -rf "${SANDBOX}/pkg"
    mkdir -p "${SANDBOX}/pkg/src"
    printf '[package]\nname = "pkg"\nversion = "0.1.0"\ntype = "bin"\n' > "${SANDBOX}/pkg/iron.toml"
}

# 1. Location of an error in the second file.
new_pkg
printf 'import lib\nfunc main() {\n    println("{helper()}")\n}\n' > "${SANDBOX}/pkg/src/main.iron"
printf 'pub func helper() -> Int {\n    return "not an int"\n}\n' > "${SANDBOX}/pkg/src/lib.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
grep -qF -- "--> src/lib.iron:2:5" <<< "${out}" || fail "wrong location: ${out}"
grep -q "combined.iron" <<< "${out}" && fail "combined.iron in output: ${out}"

# 2. A private function used from another file.
new_pkg
printf 'import lib\nfunc main() {\n    println("{helper()}")\n}\n' > "${SANDBOX}/pkg/src/main.iron"
printf 'func helper() -> Int {\n    return 42\n}\n' > "${SANDBOX}/pkg/src/lib.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
grep -q "E0320" <<< "${out}" || fail "private helper visible across files: ${out}"

# 3. The pub version builds and runs.
printf 'pub func helper() -> Int {\n    return 42\n}\n' > "${SANDBOX}/pkg/src/lib.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" run 2>&1 | tail -1)
[ "${out}" = "42" ] || fail "pub helper: ${out}"

# 4. Private fields, methods and inits of a pub object (#159).
new_pkg
cat > "${SANDBOX}/pkg/src/lib.iron" <<'IRON'
pub object Player {
    var health: Int
    pub val name: String
    pub init(health: Int) {
        self.health = health
        self.name = "p"
    }
    init hidden() {
        self.health = 1
        self.name = "h"
    }
    func recalc() {
        self.health = 0
    }
    pub func hp() -> Int {
        return self.health
    }
}
pub object Point {
    pub val x: Int
    val y: Int
}
IRON
cat > "${SANDBOX}/pkg/src/main.iron" <<'IRON'
import lib
func main() {
    var p = Player(10)
    p.health = 5
    p.recalc()
    var q = Player.hidden()
    val pt = Point(1, 2)
    println("{p.hp()} {p.name} {pt.x} {q.hp()}")
}
IRON
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
for what in "field 'health' of 'Player'" "method 'Player.recalc'" "init 'Player.hidden'" \
            "field 'y' of 'Point'"; do
    grep -qF "${what} is private to src/lib.iron" <<< "${out}" \
        || grep -qF "${what} (set by its field-wise constructor) is private" <<< "${out}" \
        || fail "no visibility error for ${what}: ${out}"
done
# pub members stay usable: no error on the println line (line 8).
grep -q "src/main.iron:8:" <<< "${out}" && fail "pub members rejected: ${out}"

# 5. A comment in a package file that looks like a file marker is a comment:
#    the code after it stays in its own file, with its own line numbers.
new_pkg
printf 'import lib\nfunc main() {\n    println("{helper()}")\n}\n' > "${SANDBOX}/pkg/src/main.iron"
cat > "${SANDBOX}/pkg/src/lib.iron" <<'IRON'
-- @file: "src/main.iron" @line: 100 @lines: 9
// @file: src/main.iron
pub func helper() -> Int {
    return "not an int"
}
IRON
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
grep -qF -- "--> src/lib.iron:4:5" <<< "${out}" || fail "marker comment moved the error: ${out}"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" build 2>&1 || true)
grep -qF -- "--> src/lib.iron:4:5" <<< "${out}" || fail "marker comment moved the build error: ${out}"
#    It does not move a private function into the file that uses it either.
cat > "${SANDBOX}/pkg/src/lib.iron" <<'IRON'
// @file: "src/main.iron" @line: 100 @lines: 9
func helper() -> Int {
    return 42
}
IRON
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
grep -q "E0320" <<< "${out}" || fail "marker comment made a private helper visible: ${out}"
#    And the files after it are still found: main.iron sorts after lib.iron.
printf 'pub func helper() -> Int {\n    // @file: "src/zzz.iron" @line: 1\n    return 42\n}\n' > "${SANDBOX}/pkg/src/lib.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" run 2>&1 | tail -1)
[ "${out}" = "42" ] || fail "package with a marker comment: ${out}"
mkdir -p "${SANDBOX}/pkg/tests"
printf 'import lib\n// @file: "src/lib.iron" @line: 1 @lines: 2\ntest "helper" {\n    assert(helper() == 42)\n}\n' > "${SANDBOX}/pkg/tests/lib_test.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" test 2>&1 || true)
grep -q "1 passed" <<< "${out}" || fail "iron test with a marker comment: ${out}"

# 6. Interfaces and extern functions follow the same rule (#842): private to
#    their file unless marked pub.
new_pkg
cat > "${SANDBOX}/pkg/src/lib.iron" <<'IRON'
interface Hidden {
    readonly func value() -> Int
}

extern func labs(x: Int) -> Int
IRON
#    A private interface named in a type annotation and in an impl list.
cat > "${SANDBOX}/pkg/src/main.iron" <<'IRON'
object P impl Hidden {
    readonly func value() -> Int { return 1 }
}

func main() {
    val h: Hidden = P()
    println("{h.value()}")
}
IRON
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
grep -q "E0320" <<< "${out}" || fail "private interface visible across files: ${out}"
grep -qF "interface 'Hidden' is private to src/lib.iron" <<< "${out}" || fail "private interface message: ${out}"
grep -qF -- "--> src/main.iron:1:1" <<< "${out}" || fail "private interface in an impl list: ${out}"
grep -qF -- "--> src/main.iron:6:12" <<< "${out}" || fail "private interface in an annotation: ${out}"
#    ... and in the bound of a generic parameter.
cat > "${SANDBOX}/pkg/src/main.iron" <<'IRON'
func show[T: Hidden](x: T) -> Int { return x.value() }

func main() {
    println("1")
}
IRON
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
grep -qF -- "--> src/main.iron:1:11" <<< "${out}" || fail "private interface in a generic bound: ${out}"
#    A private extern function.
printf 'func main() {\n    println("{labs(-9)}")\n}\n' > "${SANDBOX}/pkg/src/main.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
grep -q "E0320" <<< "${out}" || fail "private extern func visible across files: ${out}"
#    In its own file a private interface is used freely.
cat >> "${SANDBOX}/pkg/src/lib.iron" <<'IRON'

object Q impl Hidden {
    readonly func value() -> Int { return 7 }
}

pub func seven() -> Int {
    val h: Hidden = Q()
    return h.value() + labs(-0)
}
IRON
printf 'func main() {\n    println("{seven()}")\n}\n' > "${SANDBOX}/pkg/src/main.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" run 2>&1 | tail -1)
[ "${out}" = "7" ] || fail "private interface used in its own file: ${out}"
#    The pub versions are visible from the other file.
cat > "${SANDBOX}/pkg/src/lib.iron" <<'IRON'
pub interface Hidden {
    readonly func value() -> Int
}

pub extern func labs(x: Int) -> Int
IRON
cat > "${SANDBOX}/pkg/src/main.iron" <<'IRON'
object P impl Hidden {
    readonly func value() -> Int { return 1 }
}

func show[T: Hidden](x: T) -> Int { return x.value() }

func main() {
    val h: Hidden = P()
    println("{h.value()} {show(P())} {labs(-9)}")
}
IRON
out=$(cd "${SANDBOX}/pkg" && "${IRON}" run 2>&1 | tail -1)
[ "${out}" = "1 1 9" ] || fail "pub interface and pub extern func: ${out}"

echo "project_diag_smoke OK"
