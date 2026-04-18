# HARD-12 (Phase 1 Plan 05) — end-to-end parity harness.
#
# test_parity_ironc_lsp sweeps every tests/integration/*.iron fixture through
# iron_analyze_buffer() twice in CLI mode (proving determinism) and once in
# LSP mode (proving the only divergence surface is the comptime FS gate, and
# that gate only fires on fixtures that actually use comptime or contain the
# known cascade markers the CLI cascade-suppression path elides).
#
# WORKING_DIRECTORY = CMAKE_SOURCE_DIR is load-bearing: the test body uses the
# relative path `tests/integration` to opendir() the fixtures without needing
# to be configured at build time. This mirrors tests/hir/CMakeLists.txt.

# Plan 05 CORE-22: extend the harness with a 4th pass that drives
# ilsp_facade_compile_pure directly, so facade output is compared
# against the analyzer's direct-LSP output byte-for-byte on every
# fixture. Needs the facade + store TUs linked in.
add_executable(test_parity_ironc_lsp
    test_parity_ironc_lsp.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
    ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
    ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
    ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
    # Plan 06: transport writer uses ilsp_log for EPIPE events.
    ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
    ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
target_include_directories(test_parity_ironc_lsp PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor)
target_compile_definitions(test_parity_ironc_lsp PRIVATE _GNU_SOURCE)
target_link_libraries(test_parity_ironc_lsp
    iron_compiler
    iron_runtime
    iron_stdlib
    unity
    m
    pthread)
# Silence -Werror on the vendored yyjson TU (mirrors the ironls target
# treatment). The set_source_files_properties entry is directory-scoped,
# so it also applies to the tests/lsp/unit/ TUs that compile yyjson.c.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
    PROPERTIES COMPILE_FLAGS "-Wno-error -w")
add_test(NAME test_parity_ironc_lsp
         COMMAND test_parity_ironc_lsp
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# Keep the existing phase-m0-invariant label (Phase 1 still enforces the
# CLI/LSP parity surface) AND add phase-m1-invariant so Plan 05's CORE-22
# parity gate runs in Phase 2's CI sweep as well.
set_tests_properties(test_parity_ironc_lsp PROPERTIES
    LABELS "integration;phase-m0-invariant;phase-m1-invariant"
    TIMEOUT 300)
