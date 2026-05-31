# Phase 34 Plan 34-04 — quickfix driver tests for LSP-06..10.
#
# Each test driver:
#   - synthesizes an Iron_Diagnostic at the fixture's known coords,
#   - dispatches the handler directly through ilsp_facade_compile_for_nav,
#   - diffs the resulting IronLsp_CodeAction against the paired
#     .expected_edit file.
#
# Synthesizing the diagnostic decouples handler-correctness verification
# from the compiler-side emit-site readiness (codes 176/606/613/820 still
# need analyzer-side emission in later Phase 34 plans; code 296 is emitted
# today by typecheck.c).
#
# Source files compiled into every binary:
#   - the handler TU under test
#   - the registry TU + every other quickfix TU (registry is a single
#     compilation unit; we don't link partial subsets)
#   - lsp/facade/compile.c (CORE-22 single iron_analyze_buffer site)
#   - lsp/facade/{span,diagnostics}.c + lsp/facade/nav/{nav_common,node_at}.c
#   - lsp/store/{document,line_index,utf,sha256}.c
#   - lsp/transport/{json,writer,frame}.c + lsp/obs/log.c + yyjson
#
# Mirrors the link-graph of tests/unit/test_codeaction_registry.c.

if(IRON_BUILD_LSP)

set(_PHASE34_QF_CODEACTION_SRC
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/registry.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_undefined_var.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_unused_import.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_missing_return.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_v3_receiver_syntax.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_object_no_init.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_v3_inline_default.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_readonly_write_self.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_readonly_calls_mutating.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_missing_val_var.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_forgotten_free.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_unused_var_alloc.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_amp_rc.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/codeaction_indent.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
    ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
    ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
    ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
    ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
    ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
    ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
    ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)

set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
    PROPERTIES COMPILE_FLAGS "-Wno-error -w")

foreach(slug
        lsp_06_missing_val_var
        lsp_07_forgotten_free
        lsp_08_unused_var_alloc
        lsp_09_amp_rc)
    add_executable(test_quickfix_${slug}
        test_quickfix_${slug}.c
        ${_PHASE34_QF_CODEACTION_SRC})
    target_include_directories(test_quickfix_${slug} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_SOURCE_DIR}/tests/lsp/quickfix
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_quickfix_${slug} PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_quickfix_${slug}
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_quickfix_${slug}
        COMMAND test_quickfix_${slug})
    set_tests_properties(test_quickfix_${slug} PROPERTIES
        LABELS "phase34-invariant;quickfix"
        TIMEOUT 60)
endforeach()

endif() # IRON_BUILD_LSP
