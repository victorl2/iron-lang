/* tests/integration/web/test_cli_parse.c
 *
 * Phase 7 integration test — validates the Phase 2 CLI parse path for
 * --target=web, --target=native, unknown target values, and default target.
 *
 * This test is invoked via ctest. It exits 0 on success, non-zero on failure.
 * Unlike tests/unit/test_toml_web.c, this test does NOT use Unity — it uses
 * raw exit codes and stderr messages, matching integration-test style.
 *
 * NOTE: This test does NOT run a full web build. It only proves the CLI
 * parse path from Phase 2 still works end-to-end, as required by Phase 7
 * success criterion 7. The full web build (Phase 7 SC1) is exercised by
 * .github/workflows/web.yml's end-to-end smoke job.
 *
 * Phase 7 scope: Linux/macOS only. The #ifdef _WIN32 guard below mirrors
 * the build_web.c guard and fails the Windows build loudly.
 */

#ifdef _WIN32
#error "test_cli_parse.c is Linux/macOS only (Phase 7 Windows deferral)"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* Required environment: IRONC_BINARY env var pointing at the ironc executable.
 * Set by the ctest invocation so the test is portable across build directories.
 * Falls back to ./build/ironc if unset. */
static const char *ironc_binary(void) {
    const char *env = getenv("IRONC_BINARY");
    return (env && *env) ? env : "./build/ironc";
}

/* Run a shell command and return its exit code. */
static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return 127;
    return WEXITSTATUS(rc);
}

static int test_target_web_parses(void) {
    char cmd[2048];
    /* Accept three outcomes as proof that --target=web reached build_web.c:
     *   1. "using emcc" banner — emcc found, preflight succeeded
     *   2. "emcc not found in PATH" — preflight found no emcc (dev machines)
     *   3. "dist/web/index.html" — full link succeeded (CI with emcc)
     * All three prove the dispatch reached iron_build_web. */
    snprintf(cmd, sizeof(cmd),
             "echo 'func main() {}' > /tmp/iron_cli_parse_web.iron && "
             "%s build --target=web /tmp/iron_cli_parse_web.iron 2>&1 | "
             "grep -qE 'using emcc|emcc not found in PATH|dist/web/index.html'",
             ironc_binary());
    int rc = run_cmd(cmd);
    if (rc != 0) {
        fprintf(stderr, "FAIL: --target=web did not reach build_web.c (rc=%d)\n", rc);
        return 1;
    }
    fprintf(stderr, "OK: --target=web parse -> build_web.c dispatch\n");
    return 0;
}

static int test_target_native_parses(void) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "echo 'func main() { println(\"ok\") }' > /tmp/iron_cli_parse_native.iron && "
             "%s build --target=native /tmp/iron_cli_parse_native.iron -o /tmp/iron_cli_parse_native 2>&1",
             ironc_binary());
    int rc = run_cmd(cmd);
    if (rc != 0) {
        fprintf(stderr, "FAIL: --target=native build failed (rc=%d)\n", rc);
        return 1;
    }
    fprintf(stderr, "OK: --target=native parse -> native build\n");
    return 0;
}

static int test_target_unknown_errors(void) {
    char cmd[2048];
    /* --target=bogus must exit non-zero and mention valid targets */
    snprintf(cmd, sizeof(cmd),
             "echo 'func main() {}' > /tmp/iron_cli_parse_unk.iron && "
             "OUT=$(%s build --target=bogus /tmp/iron_cli_parse_unk.iron 2>&1); "
             "RC=$?; "
             "if [ $RC -eq 0 ]; then exit 1; fi; "
             "echo \"$OUT\" | grep -q 'valid targets: web, native'",
             ironc_binary());
    int rc = run_cmd(cmd);
    if (rc != 0) {
        fprintf(stderr, "FAIL: --target=bogus did not produce expected error (rc=%d)\n", rc);
        return 1;
    }
    fprintf(stderr, "OK: --target=bogus error format\n");
    return 0;
}

static int test_target_default_is_native(void) {
    char cmd[2048];
    /* No --target flag should default to native (same behavior as --target=native) */
    snprintf(cmd, sizeof(cmd),
             "echo 'func main() { println(\"ok\") }' > /tmp/iron_cli_parse_default.iron && "
             "%s build /tmp/iron_cli_parse_default.iron -o /tmp/iron_cli_parse_default 2>&1",
             ironc_binary());
    int rc = run_cmd(cmd);
    if (rc != 0) {
        fprintf(stderr, "FAIL: default target build failed (rc=%d)\n", rc);
        return 1;
    }
    fprintf(stderr, "OK: no --target flag -> native default\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_target_web_parses();
    failures += test_target_native_parses();
    failures += test_target_unknown_errors();
    failures += test_target_default_is_native();

    if (failures > 0) {
        fprintf(stderr, "test_cli_parse: %d test(s) FAILED\n", failures);
        return 1;
    }
    fprintf(stderr, "test_cli_parse: all 4 tests PASSED\n");
    return 0;
}
