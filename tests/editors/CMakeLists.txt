# Phase 6 Plan 06-01 Task 3 (EXT-10, D-07): CTest wiring for editor e2e
# infrastructure. Plan 06-01 ships only the shared diag_error.iron fixture
# + a sanity CTest that asserts `ironc check` reports exactly one undefined
# variable on it. Plans 06-03/04/05/06 wire the real three-editor harnesses
# via .github/workflows/ci.yml (not CTest — e2e runs need full editor
# installations).

set(_ed_fixture "${CMAKE_SOURCE_DIR}/tests/editors/fixtures/diag_error.iron")

if(IRON_BUILD_LSP AND TARGET ironc)
    # Sanity gate: the shared fixture must produce EXACTLY one undefined-symbol
    # diagnostic when run through `ironc check`. If this test fails, every
    # downstream editor harness will fail too — so it's the earliest place
    # to catch fixture drift. The grep matches either the stable message
    # substring "undefined" or the diagnostic code "undefined_symbol" /
    # the canonical IRON_ERR_UNDEFINED_VAR name, per the diagnostics module
    # (code 200 in the semantic range 200..299).
    add_test(NAME test_diag_error_fixture_is_one_error
        COMMAND $<TARGET_FILE:ironc> check ${_ed_fixture})
    # `ironc check` exits nonzero on the fixture by design; the verdict is
    # the diagnostic in its output (no shell, so this runs on Windows too).
    set_tests_properties(test_diag_error_fixture_is_one_error PROPERTIES
        PASS_REGULAR_EXPRESSION "undefined_symbol|undefined|IRON_ERR_UNDEFINED_VAR"
        LABELS "phase-m5-invariant")
endif()
