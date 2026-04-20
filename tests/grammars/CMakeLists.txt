# Phase 6 Plan 06-01 Task 2 (EXT-01, EXT-03, D-02, D-14): CTest wiring for
# grammar invariants — TextMate ReDoS lint + snapshot corpus. The two
# keyword-drift tests live in the root CMakeLists.txt (they only need
# `cmake -E compare_files`; no Node tooling required). This file is
# gated on `npm` + `node` availability — missing tooling produces SKIP
# shim tests rather than configure-time failures, so the grammar CI gate
# degrades gracefully on hosts without Node.
#
# After Plan 06-02 this file expands with tree-sitter corpus runners.
# Task 3 wires tests/editors/ as a nested add_subdirectory so the
# shared diag_error.iron sanity test lands here too.

find_program(IRON_NPM_EXECUTABLE npm)
find_program(IRON_NODE_EXECUTABLE node)

set(_gr_tm_dir "${CMAKE_SOURCE_DIR}/tests/grammars/textmate")
set(_gr_tm_committed "${CMAKE_SOURCE_DIR}/grammars/textmate/iron.tmLanguage.json")

if(IRON_BUILD_GRAMMARS AND IRON_NPM_EXECUTABLE AND IRON_NODE_EXECUTABLE)
    # Setup fixture: install Node dev deps once per build. Subsequent tests
    # depend on this via FIXTURES_REQUIRED so npm install runs once and is
    # reused. --prefer-offline keeps CI green in cache-hit scenarios.
    add_test(NAME test_grammars_npm_install
        COMMAND ${IRON_NPM_EXECUTABLE} install
                --no-audit --no-fund --prefer-offline
        WORKING_DIRECTORY "${_gr_tm_dir}")
    set_tests_properties(test_grammars_npm_install PROPERTIES
        LABELS "phase-m5-invariant"
        FIXTURES_SETUP grammars_node_deps
        TIMEOUT 180)

    add_test(NAME test_grammar_redos_clean
        COMMAND ${IRON_NODE_EXECUTABLE}
                "${_gr_tm_dir}/redos_check.js"
                "${_gr_tm_committed}"
        WORKING_DIRECTORY "${_gr_tm_dir}")
    set_tests_properties(test_grammar_redos_clean PROPERTIES
        LABELS "phase-m5-invariant"
        FIXTURES_REQUIRED grammars_node_deps)

    add_test(NAME test_grammar_textmate_snapshot
        COMMAND ${IRON_NODE_EXECUTABLE}
                "${_gr_tm_dir}/snapshot.test.js"
        WORKING_DIRECTORY "${_gr_tm_dir}")
    set_tests_properties(test_grammar_textmate_snapshot PROPERTIES
        LABELS "phase-m5-invariant"
        FIXTURES_REQUIRED grammars_node_deps
        SKIP_RETURN_CODE 77)
else()
    # SKIP shim: record why the tests were not registered so CI log tells
    # the reviewer which tool is missing. Exit 77 is CTest's SKIP code.
    add_test(NAME test_grammar_redos_clean
        COMMAND ${CMAKE_COMMAND} -E echo
                "SKIPPED: npm and/or node not found on PATH (Phase 6 ReDoS lint)")
    set_tests_properties(test_grammar_redos_clean PROPERTIES
        LABELS "phase-m5-invariant"
        SKIP_REGULAR_EXPRESSION "SKIPPED")

    add_test(NAME test_grammar_textmate_snapshot
        COMMAND ${CMAKE_COMMAND} -E echo
                "SKIPPED: npm and/or node not found on PATH (Phase 6 snapshot)")
    set_tests_properties(test_grammar_textmate_snapshot PROPERTIES
        LABELS "phase-m5-invariant"
        SKIP_REGULAR_EXPRESSION "SKIPPED")
endif()

# Nested add_subdirectory: the tests/editors/ subtree lands here instead of
# at the root CMakeLists.txt level so the whole Phase 6 test graph activates
# under IRON_BUILD_GRAMMARS. Plans 06-03..06-06 extend tests/editors/ with
# real e2e CI harnesses; Plan 06-01 only ships the shared diag_error.iron
# sanity gate (see tests/editors/CMakeLists.txt).
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/editors
                 ${CMAKE_BINARY_DIR}/tests/editors)
