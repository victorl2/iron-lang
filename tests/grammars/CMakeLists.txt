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

# ─ Tree-sitter corpus + integration-corpus parse gates (Plan 06-02) ─────
#
# Two CTests added here, both under phase-m5-invariant:
#   - test_tree_sitter_corpus              — validates the hand-curated
#                                            >= 20-fixture corpus under
#                                            grammars/tree-sitter/iron/test/
#                                            corpus/ against the grammar
#   - test_tree_sitter_parses_integration_corpus — structural parity fence
#                                            (D-12): every tests/integration/
#                                            *.iron parses with zero
#                                            ERROR/MISSING nodes (with a
#                                            documented known-skip list)
#
# Both gate on tree-sitter-cli availability. The local node_modules install
# uses the package.json pin (tree-sitter-cli@0.25.10 for glibc 2.34+);
# CI installs 0.26.x globally via the tree-sitter-wasm job (Plan 06-02
# Task 3). Tests degrade gracefully (exit 77 SKIP) when the CLI is absent.

find_program(BASH_EXECUTABLE bash)

if(IRON_BUILD_GRAMMARS AND IRON_NPM_EXECUTABLE AND IRON_NODE_EXECUTABLE AND BASH_EXECUTABLE)
    set(_gr_ts_dir "${CMAKE_SOURCE_DIR}/grammars/tree-sitter/iron")

    # Setup fixture: install tree-sitter-cli locally via npm once per build.
    # Shared with test_tree_sitter_corpus + test_tree_sitter_parses_integration_corpus
    # via FIXTURES_REQUIRED so the install cost is amortized.
    add_test(NAME test_tree_sitter_npm_install
        COMMAND ${IRON_NPM_EXECUTABLE} install
                --no-audit --no-fund --prefer-offline
        WORKING_DIRECTORY "${_gr_ts_dir}")
    set_tests_properties(test_tree_sitter_npm_install PROPERTIES
        LABELS "phase-m5-invariant"
        FIXTURES_SETUP tree_sitter_deps
        TIMEOUT 180)

    add_test(NAME test_tree_sitter_corpus
        COMMAND ${BASH_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tests/grammars/tree_sitter/run_corpus.sh")
    set_tests_properties(test_tree_sitter_corpus PROPERTIES
        LABELS "phase-m5-invariant"
        FIXTURES_REQUIRED tree_sitter_deps
        SKIP_RETURN_CODE 77
        TIMEOUT 60)

    add_test(NAME test_tree_sitter_parses_integration_corpus
        COMMAND ${BASH_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tests/grammars/tree_sitter/parse_fixtures.sh")
    set_tests_properties(test_tree_sitter_parses_integration_corpus PROPERTIES
        LABELS "phase-m5-invariant"
        FIXTURES_REQUIRED tree_sitter_deps
        SKIP_RETURN_CODE 77
        TIMEOUT 120)
else()
    add_test(NAME test_tree_sitter_corpus
        COMMAND ${CMAKE_COMMAND} -E echo
                "SKIPPED: tree-sitter-cli pre-reqs missing (npm/node/bash)")
    set_tests_properties(test_tree_sitter_corpus PROPERTIES
        LABELS "phase-m5-invariant"
        SKIP_REGULAR_EXPRESSION "SKIPPED")

    add_test(NAME test_tree_sitter_parses_integration_corpus
        COMMAND ${CMAKE_COMMAND} -E echo
                "SKIPPED: tree-sitter-cli pre-reqs missing (npm/node/bash)")
    set_tests_properties(test_tree_sitter_parses_integration_corpus PROPERTIES
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
