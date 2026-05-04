/* test_lsp_build_smoke -- Phase 2 Plan 01.
 * Proves the LSP umbrella headers and the vendored yyjson drop-in are
 * includable from a unit TU and that their canonical types/macros are
 * visible. Every later Phase 2 unit test inherits this CMake wiring. */
#include "unity.h"
#include "lsp/lsp.h"
#include "lsp/facade/types.h"
#include "lsp/server/dispatch.h"
#include "lsp/store/document.h"
#include "lsp/transport/types.h"
#include "vendor/yyjson/yyjson.h"

void setUp(void) {}
void tearDown(void) {}

static void test_yyjson_version_available(void) {
    /* yyjson 0.12.0 exports YYJSON_VERSION_MAJOR/MINOR/PATCH/HEX.
     * The "intent" assertion is "header parses and defines a version"; the
     * exact values below will need bumping when yyjson is re-vendored. */
    TEST_ASSERT_EQUAL_INT(0, YYJSON_VERSION_MAJOR);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(12, YYJSON_VERSION_MINOR);
}

static void test_lsp_facade_types_sized(void) {
    IronLsp_Position p = (IronLsp_Position){.line = 1u, .character = 2u};
    TEST_ASSERT_EQUAL_UINT32(1u, p.line);
    TEST_ASSERT_EQUAL_UINT32(2u, p.character);
    IronLsp_Range r = (IronLsp_Range){.start = p, .end = p};
    TEST_ASSERT_EQUAL_UINT32(1u, r.end.line);
    TEST_ASSERT_EQUAL_UINT32(2u, r.end.character);
}

static void test_lsp_transport_reqid_tag(void) {
    IronLsp_ReqId id = {.kind = ILSP_REQID_INT, .u = {.n = 42}};
    TEST_ASSERT_EQUAL_INT(ILSP_REQID_INT, id.kind);
    TEST_ASSERT_EQUAL_INT64(42, id.u.n);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_yyjson_version_available);
    RUN_TEST(test_lsp_facade_types_sized);
    RUN_TEST(test_lsp_transport_reqid_tag);
    return UNITY_END();
}
