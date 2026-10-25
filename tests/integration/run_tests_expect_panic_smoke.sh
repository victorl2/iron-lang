#!/usr/bin/env bash
# Both accepted comment spellings must survive directive extraction.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d -t iron-runner-panic-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p "${WORK}/tests/integration/v4" "${WORK}/bin"
cp "${ROOT}/tests/run_tests.sh" "${WORK}/tests/run_tests.sh"
printf '%s\n' '-- @expect-panic: dash panic' 'func main() {}' \
    > "${WORK}/tests/integration/v4/dash.iron"
printf '%s\n' '// @expect-panic: slash panic' 'func main() {}' \
    > "${WORK}/tests/integration/v4/slash.iron"

cat > "${WORK}/bin/iron" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
src="${!#}"
name="$(basename "${src}" .iron)"
printf '#!/bin/sh\nprintf "%s panic\\n" >&2\nexit 1\n' "${name}" > "${name}"
chmod +x "${name}"
MOCK
chmod +x "${WORK}/bin/iron"

set +e
IRON_TEST_JOBS=2 "${WORK}/tests/run_tests.sh" v4 "${WORK}/bin/iron" \
    > "${WORK}/out" 2>&1
rc=$?
set -e
[ "${rc}" -eq 0 ] || { cat "${WORK}/out"; exit "${rc}"; }
grep -q '^PASS=2 FAIL=0 XFAIL=0 TOTAL=2$' "${WORK}/out" || {
    cat "${WORK}/out"
    echo "expect-panic directive result missing"
    exit 1
}
echo "run_tests expect-panic directives OK"
