/* test_shell_subst.c — WEB-TEST-06
 *
 * Verifies IRON_WEB_DEFAULT_SHELL embedded string contains all four shell
 * patches + the {{{ SCRIPT }}} substitution slot that Phase 9 promised.
 * This is a structural test — the actual emcc substitution is verified by
 * the Phase 9 CI smoke step in .github/workflows/web.yml.
 */

#include "unity.h"
#include "cli/web_shell_template.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_shell_has_script_substitution_slot(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(IRON_WEB_DEFAULT_SHELL, "{{{ SCRIPT }}}"),
        "shell must preserve emcc's {{{ SCRIPT }}} substitution slot");
}

static void test_shell_has_canvas_element(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(IRON_WEB_DEFAULT_SHELL, "id=canvas"),
        "shell must contain <canvas id=canvas> for rcore_web.c");
}

static void test_shell_has_coop_coep_preflight(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(IRON_WEB_DEFAULT_SHELL, "crossOriginIsolated"),
        "shell must check self.crossOriginIsolated for SharedArrayBuffer support");
}

static void test_shell_has_audio_unlock(void) {
    /* Either the unlockAudio function or the _audio_resume Module hook must be present */
    int has_unlock = strstr(IRON_WEB_DEFAULT_SHELL, "unlockAudio") != NULL
                  || strstr(IRON_WEB_DEFAULT_SHELL, "_audio_resume") != NULL;
    TEST_ASSERT_TRUE_MESSAGE(has_unlock,
        "shell must contain audio-unlock listener for suspended AudioContext");
}

static void test_shell_has_webgl_context_lost_handler(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(IRON_WEB_DEFAULT_SHELL, "webglcontextlost"),
        "shell must handle webglcontextlost with a reload prompt");
}

static void test_shell_is_non_empty(void) {
    TEST_ASSERT_GREATER_THAN_MESSAGE(500, strlen(IRON_WEB_DEFAULT_SHELL),
        "shell HTML should be at least 500 chars (real template, not a stub)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_shell_has_script_substitution_slot);
    RUN_TEST(test_shell_has_canvas_element);
    RUN_TEST(test_shell_has_coop_coep_preflight);
    RUN_TEST(test_shell_has_audio_unlock);
    RUN_TEST(test_shell_has_webgl_context_lost_handler);
    RUN_TEST(test_shell_is_non_empty);
    return UNITY_END();
}
