# Phase 2 pytest-lsp smoke harness. Populated incrementally by plans
# 02-03..02-06 (initialize lifecycle, didOpen/didChange, publishDiagnostics,
# shutdown). For now we register a placeholder Python invocation so the
# phase-m1-invariant label has a smoke-side test from day 1. The harness
# gracefully skips when Python3 is not on the builder.

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND IRON_BUILD_LSP)
    add_test(NAME test_lsp_smoke_placeholder
             COMMAND ${Python3_EXECUTABLE} -c "print('lsp smoke placeholder ok')"
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(test_lsp_smoke_placeholder PROPERTIES
        LABELS "smoke;phase-m1-invariant"
        TIMEOUT 30)
else()
    message(STATUS
        "Skipping tests/lsp/smoke: Python3=${Python3_FOUND}, "
        "IRON_BUILD_LSP=${IRON_BUILD_LSP}")
endif()
