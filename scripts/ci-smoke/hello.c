/* Minimal smoke-test payload for the Phase 1 web CI workflow.
 *
 * Compiled by `.github/workflows/web.yml` via:
 *     emcc scripts/ci-smoke/hello.c -o /tmp/hello.js
 * to validate that the pinned emsdk (see .emsdk-version) installs
 * and produces both hello.js and hello.wasm artifacts on
 * ubuntu-latest and macos-latest runners.
 *
 * This is a PLACEHOLDER. Phase 7 (build_web.c emcc orchestration)
 * replaces the CI payload with a real `iron build --target=web`
 * invocation against examples/hello/hello.iron.
 */
int main(void) {
    return 0;
}
