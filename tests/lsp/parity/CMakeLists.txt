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
    LABELS "integration;phase-m0-invariant;phase-m1-invariant;phase-m6-invariant"
    TIMEOUT 300)

# Phase 5 Plan 05-02 (D-08, FMT-02) -- CLI <-> LSP formatter parity.
#
# Sweeps tests/integration/*.iron and, for every fixture that parses
# cleanly, asserts iron_format_source (CLI path) and
# ilsp_facade_format_full (LSP facade path) produce byte-for-byte
# identical formatted output. Both route through the same
# iron_format_source primitive underneath -- this test protects the
# facade's post-processing (trailing newline + BOM normalization)
# from drifting out of lockstep with the CLI.
add_executable(test_parity_ironc_lsp_fmt
    test_parity_ironc_lsp_fmt.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/format.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c)
target_include_directories(test_parity_ironc_lsp_fmt PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor)
target_compile_definitions(test_parity_ironc_lsp_fmt PRIVATE
    _GNU_SOURCE
    "TESTS_INTEGRATION_DIR=\"${CMAKE_SOURCE_DIR}/tests/integration\"")
target_link_libraries(test_parity_ironc_lsp_fmt
    iron_compiler
    iron_runtime
    iron_stdlib
    unity
    m
    pthread)
add_test(NAME test_parity_ironc_lsp_fmt
         COMMAND test_parity_ironc_lsp_fmt
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_parity_ironc_lsp_fmt PROPERTIES
    LABELS "integration;phase-m4-invariant;phase-m6-invariant"
    TIMEOUT 300)

# Phase 9 Plan 09-02 Task 1 — Wave 0 v2 printer baseline capture utility.
#
# One-shot utility used to populate tests/lsp/fixtures/printer_v2_baseline/
# BEFORE any printer change in Plan 09-02. Run once per refresh; the captured
# .printed files are the byte-for-byte invariant guard for Pitfall 3
# (zero v2 regression) per 09-RESEARCH.md §3 and §5 Wave 0. Not registered as
# a ctest — invoked manually from the executor agent during plan execution.
add_executable(printer_baseline_capture
    printer_baseline_capture.c)
target_include_directories(printer_baseline_capture PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor)
target_link_libraries(printer_baseline_capture
    iron_compiler
    iron_runtime
    iron_stdlib
    m
    pthread)

# Phase 9 Plan 09-02 Task 1 — v3 fixed-point parity test.
#
# Asserts iron_print_ast(parse(iron_print_ast(parse(src)))) ==
#         iron_print_ast(parse(src))    (byte-identical)
# for every tests/integration/v3_*.iron fixture. Also asserts every non-v3
# fixture produces byte-identical iron_format_source output vs the Wave 0
# baseline snapshot in tests/lsp/fixtures/printer_v2_baseline/ — the
# Pitfall 3 zero-regression invariant.
add_executable(test_parity_v3_print_fixed_point
    test_parity_v3_print_fixed_point.c)
target_include_directories(test_parity_v3_print_fixed_point PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor)
target_compile_definitions(test_parity_v3_print_fixed_point PRIVATE
    _GNU_SOURCE
    "TESTS_INTEGRATION_DIR=\"${CMAKE_SOURCE_DIR}/tests/integration\""
    "PRINTER_V2_BASELINE_DIR=\"${CMAKE_SOURCE_DIR}/tests/lsp/fixtures/printer_v2_baseline\"")
target_link_libraries(test_parity_v3_print_fixed_point
    iron_compiler
    iron_runtime
    iron_stdlib
    unity
    m
    pthread)
add_test(NAME test_parity_v3_print_fixed_point
         COMMAND test_parity_v3_print_fixed_point
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_parity_v3_print_fixed_point PROPERTIES
    LABELS "integration;phase-m4-invariant;phase-m6-invariant"
    TIMEOUT 300)
