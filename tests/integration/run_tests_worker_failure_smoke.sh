#!/usr/bin/env bash
# A worker that dies under set -e must not hide the ordered logs and summary.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d -t iron-runner-worker-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p "${WORK}/tests/integration/v4" "${WORK}/bin"
cp "${ROOT}/tests/run_tests.sh" "${WORK}/tests/run_tests.sh"
printf 'func main() {\n}\n' > "${WORK}/tests/integration/v4/case.iron"
printf 'ok\n' > "${WORK}/tests/integration/v4/case.expected"

cat > "${WORK}/bin/iron" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
src="${!#}"
rm -f "${src%.iron}.expected"
name="$(basename "${src}" .iron)"
printf '#!/bin/sh\nprintf ok\\n\n' > "${name}"
chmod +x "${name}"
MOCK
chmod +x "${WORK}/bin/iron"

set +e
IRON_TEST_JOBS=2 "${WORK}/tests/run_tests.sh" v4 "${WORK}/bin/iron" \
    > "${WORK}/out" 2>&1
rc=$?
set -e

[ "${rc}" -ne 0 ] || { cat "${WORK}/out"; echo "runner unexpectedly passed"; exit 1; }
grep -q '^=== Summary ===$' "${WORK}/out" || { cat "${WORK}/out"; echo "summary missing"; exit 1; }
grep -q '^PASS=0 FAIL=1 XFAIL=0 TOTAL=1$' "${WORK}/out" || {
    cat "${WORK}/out"
    echo "failure count missing"
    exit 1
}
echo "run_tests worker failure summary OK"
