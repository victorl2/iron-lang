# Phase 2 Plan 06 Task 03 (CORE-22) -- pytest-lsp smoke harness.
#
# ctest invokes pytest on this directory and passes the built `ironls`
# binary via --lsp-binary. The harness spawns a fresh `ironls`
# subprocess per test (see conftest.py), drives it through the
# initialize / didOpen / didChange / publishDiagnostics / shutdown
# sequence, and asserts the per-test invariants.
#
# Skip policy: if Python 3 is not on the builder OR pytest-lsp is not
# installed, the smoke test is registered as a CTest skip (via a small
# Python probe that sets the skip return code). This keeps the local
# developer story "ctest -L phase-m1-invariant" green without pytest-lsp
# while the CI workflow always has it installed (see ci.yml).

find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_FOUND AND IRON_BUILD_LSP)
    # Probe whether pytest-lsp is importable. If not, register a SKIP
    # test that documents the missing dependency.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import pytest_lsp; print(pytest_lsp.__version__)"
        RESULT_VARIABLE PYTEST_LSP_PROBE
        OUTPUT_VARIABLE PYTEST_LSP_VERSION
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(PYTEST_LSP_PROBE EQUAL 0)
        message(STATUS "pytest-lsp ${PYTEST_LSP_VERSION} detected; wiring smoke harness")
        add_test(
            NAME lsp_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}
                --lsp-binary=$<TARGET_FILE:ironls>
                -v --tb=short
                -p no:cacheprovider
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_smoke PROPERTIES
            LABELS "smoke;phase-m1-invariant"
            TIMEOUT 120
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        message(STATUS
            "pytest-lsp not installed -- registering lsp_smoke as a skipping shim. "
            "Install with: pip install --pre 'pytest-lsp>=1.0.0b1' 'pytest>=7'")
        # CTest treats exit code 77 as SKIPPED when SKIP_RETURN_CODE is set.
        add_test(
            NAME lsp_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_smoke PROPERTIES
            LABELS "smoke;phase-m1-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)
    endif()
else()
    message(STATUS
        "Skipping tests/lsp/smoke: Python3=${Python3_FOUND}, "
        "IRON_BUILD_LSP=${IRON_BUILD_LSP}")
endif()
