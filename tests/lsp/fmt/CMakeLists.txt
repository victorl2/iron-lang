# Phase 5 Plan 05-05 Task 2 (FMT-06, D-07): CI gate asserting every
# Phase 4 P1 quickfix produces iron-fmt-clean output.
#
# Dual-label registration (phase-m3-invariant + phase-m4-invariant) per
# D-07: a Phase 4 quickfix regression surfaces under EITHER milestone's
# CI sweep.
#
# IRON_BUILD_LSP-gated like sibling lsp test directories (parity/,
# smoke/, unit/).

if(IRON_BUILD_LSP)
    add_executable(test_fmt_quickfix_clean
        quickfix/test_fmt_quickfix_clean.c
        # Quickfix registry + per-code handler TUs.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/registry.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_undefined_var.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_unused_import.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_missing_return.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c
        # Phase 12 Plan 12-02 (QF-01): single command-style handler shared
        # by codes 260 + 261 (registry.c references it via two rows).
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_v3_receiver_syntax.c
        # Phase 12 Plan 12-01 (D-23): shared body-indent helper consumed
        # by quickfix_missing_return.c.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/codeaction_indent.c
        # Span -> LSP range converter (ilsp_span_to_lsp_range).
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        # Document store (line_idx + ilsp_byte_of_line used by
        # quickfix_unused_import.line_is_blank and
        # quickfix_missing_return.derive_body_indent).
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c)
    target_include_directories(test_fmt_quickfix_clean PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_fmt_quickfix_clean PRIVATE
        _GNU_SOURCE
        "QUICKFIX_FIXTURE_DIR=\"${CMAKE_SOURCE_DIR}/tests/lsp/fmt/quickfix\"")
    target_link_libraries(test_fmt_quickfix_clean
        iron_compiler
        iron_runtime
        iron_stdlib
        unity
        m
        pthread)
    add_test(NAME test_fmt_quickfix_clean
             COMMAND test_fmt_quickfix_clean
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # D-07 dual-label: surfaces under BOTH M3 and M4 CI runs.
    set_tests_properties(test_fmt_quickfix_clean PROPERTIES
        LABELS "integration;phase-m3-invariant;phase-m4-invariant"
        TIMEOUT 60)
else()
    message(STATUS "Skipping tests/lsp/fmt: IRON_BUILD_LSP=OFF")
endif()
