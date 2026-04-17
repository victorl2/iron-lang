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

add_executable(test_parity_ironc_lsp test_parity_ironc_lsp.c)
target_link_libraries(test_parity_ironc_lsp
    iron_compiler
    iron_runtime
    iron_stdlib
    unity
    m
    pthread)
add_test(NAME test_parity_ironc_lsp
         COMMAND test_parity_ironc_lsp
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_parity_ironc_lsp PROPERTIES
    LABELS "integration;phase-m0-invariant"
    TIMEOUT 300)
