# LSP unit tests -- Phase 2 M1 LSP Core.
# Per-test executables following the tests/unit/ pattern. Subsequent plans
# (02-02 .. 02-06) add test_lsp_frame, test_lsp_json, test_lsp_utf, etc.
# The phase-m1-invariant label marks tests that must never regress while
# the Phase 2 milestone is open.

if(IRON_BUILD_LSP)
    add_executable(test_lsp_build_smoke test_lsp_build_smoke.c)
    target_include_directories(test_lsp_build_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_build_smoke iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_build_smoke COMMAND test_lsp_build_smoke)
    set_tests_properties(test_lsp_build_smoke PROPERTIES
        LABELS "unit;phase-m1-invariant")
else()
    message(STATUS "Skipping tests/lsp/unit: IRON_BUILD_LSP=OFF")
endif()
