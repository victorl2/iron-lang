#!/bin/bash
# soa_storage.sh: layout: soa is honored in the generated storage (#679).
#
# Output tests cannot tell a list stored field by field from one stored
# element by element. This reads the generated C of a small program:
#   - a list written with layout: soa is created with columns, one written
#     without is not, and a slice keeps the layout of its source;
#   - reading or writing one field goes to that field's column, not through
#     a gather of the whole element;
#   - a method call on an element lends a gathered copy and writes back the
#     fields the method can change, and only those.
#
# Usage: soa_storage.sh path/to/ironc
set -u
ironc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_soa_storage_XXXXXX")
trap 'rm -rf "${work}"' EXIT
cat > "${work}/prog.iron" <<'IRON'
object P {
    var x: Int
    var y: Float
    init(x: Int) {
        self.x = x
        self.y = 0.5
    }
    func bump() { self.x += 1 }
}
func main() {
    var cols: [P, layout: soa] = [P(1), P(2)]
    var rows: [P] = [P(3)]
    rows.push(P(4))
    var i = 0
    while i < cols.len() {
        cols[i].x = cols[i].x + 1
        i += 1
    }
    cols[0].bump()
    rows[0].bump()
    val part = cols[0..1]
    println("{cols[1].x} {rows[0].x} {part.len()}")
}
IRON
(cd "${work}" && "${ironc}" build prog.iron --emit-c > build.log 2>&1) || { cat "${work}/build.log"; exit 1; }
c="${work}/.iron-build/prog.c"
fail=0
need() { grep -qE -- "$2" "${c}" || { echo "[FAIL] $1: no match for /$2/"; fail=1; }; }
count() { [ "$(grep -cE -- "$2" "${c}")" = "$3" ] || { echo "[FAIL] $1: expected $3 matches of /$2/, got $(grep -cE -- "$2" "${c}")"; fail=1; }; }
need  "list header carries the layout"      '^ +bool soa;'
count "one list created with columns"       '_v[0-9]+ = Iron_List_Iron_P_create_soa\(\);' 1
count "two lists created element by element"   '_v[0-9]+ = Iron_List_Iron_P_create\(\);' 2
need  "field write goes to its column"      '\(\*Iron_List_Iron_P_field_x\(.*\) = '
need  "field read goes to its column"       '= \(\(\*Iron_List_Iron_P_field_x\(.*\)\) \+ '
need  "receiver is a gathered copy"         '= Iron_List_Iron_P_read\(\*_sb[0-9]+_l, _sb[0-9]+_i\), &_sb[0-9]+\)'
need  "only the field bump writes goes back" '\{ Iron_List_Iron_P_put_x\(_sb[0-9]+_l, _sb[0-9]+_i, &_sb[0-9]+\); \}'
need  "a slice takes its source's layout"   '\.soa = \(.*\)\.soa;'
[ "${fail}" -eq 0 ] && echo "soa storage: ok"
exit "${fail}"
