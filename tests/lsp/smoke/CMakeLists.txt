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
                --ignore=${CMAKE_CURRENT_SOURCE_DIR}/nav
                --ignore=${CMAKE_CURRENT_SOURCE_DIR}/edit
                --ignore=${CMAKE_CURRENT_SOURCE_DIR}/fmt
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_smoke PROPERTIES
            LABELS "smoke;phase-m1-invariant"
            TIMEOUT 120
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 3 Plan 01 Wave 0: lsp_nav_smoke -- 7 pytest stubs under
        # tests/lsp/smoke/nav/ for the NAV endpoints. Stubs use
        # pytestmark = pytest.mark.skip so the target exits 0 until Plans
        # 02..06 flip them to real assertions.
        add_test(
            NAME lsp_nav_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/nav
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_nav_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m2-invariant"
            TIMEOUT 120
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 4 Plan 04-01 Wave 0: lsp_edit_smoke -- pytest harness root
        # under tests/lsp/smoke/edit/ for the EDIT endpoints (EDIT-01..15).
        # Task 01 registers the conftest.py scaffold + a pytest.mark.skip
        # scaffold test; subsequent plans flip stubs to real assertions.
        add_test(
            NAME lsp_edit_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/edit
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_edit_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m3-invariant"
            TIMEOUT 120
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 5 Plan 05-02 (FMT-02, D-13): lsp_fmt_smoke -- pytest harness
        # for tests/lsp/smoke/fmt covering textDocument/formatting.
        # 4 scenarios: full-doc + refusal + iron.toml load + iron.toml
        # reload. Plans 05-03/04 append range + on-type coverage under the
        # same label.
        add_test(
            NAME lsp_fmt_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/fmt
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_fmt_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m4-invariant"
            TIMEOUT 120
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 14 Plan 14-02 (CMD-01, CMD-03): test_iron_migrate_command --
        # Wave 0 smoke for workspace/executeCommand iron.migrate.
        # Covers: capability-advertised, workspace-edit happy path,
        # CMD-03 version-gate failure path.
        # Labels: phase-m6-invariant (Phase 7 family) + phase-14-invariant.
        add_test(
            NAME test_iron_migrate_command
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_iron_migrate_command.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(test_iron_migrate_command PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m6-invariant;phase-14-invariant"
            TIMEOUT 120
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 14 Plan 14-02 (MIG-06): test_hard_refuse_v3 --
        # Unit-style version-range guard. Does NOT require a running ironls.
        # Asserts that v1.x is REJECTED and v3.x is ACCEPTED by
        # the '>= 3.0.0, < 4.0.0' extension range.
        # Labels: phase-m6-invariant + phase-14-invariant.
        add_test(
            NAME test_hard_refuse_v3
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_hard_refuse_v3.py
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(test_hard_refuse_v3 PROPERTIES
            LABELS "smoke;phase-m6-invariant;phase-14-invariant"
            TIMEOUT 60
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

        # Phase 3 Plan 01 Wave 0: same SKIP shim for the nav harness so
        # ctest -L phase-m2-invariant stays green when pytest-lsp is absent.
        add_test(
            NAME lsp_nav_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_nav_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m2-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 4 Plan 04-01 Wave 0: same SKIP shim for the edit harness so
        # ctest -L phase-m3-invariant stays green when pytest-lsp is absent.
        add_test(
            NAME lsp_edit_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_edit_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m3-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 5 Plan 05-02: same SKIP shim for the fmt smoke harness so
        # ctest -L phase-m4-invariant stays green when pytest-lsp is absent.
        add_test(
            NAME lsp_fmt_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_fmt_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m4-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 14 Plan 14-02: SKIP shim for test_iron_migrate_command when
        # pytest-lsp is absent. ctest -L phase-m6-invariant stays green.
        add_test(
            NAME test_iron_migrate_command
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(test_iron_migrate_command PROPERTIES
            LABELS "smoke;lsp_smoke;phase-m6-invariant;phase-14-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 14 Plan 14-02: SKIP shim for test_hard_refuse_v3.
        # This test has no pytest-lsp dependency (pure Python unit test)
        # but we keep the SKIP shim for the case where Python3 itself
        # is missing — the outer `if(Python3_FOUND ...)` gate already
        # handles that. When Python3 is present, no shim is needed for
        # test_hard_refuse_v3 since it doesn't need pytest-lsp; however
        # the SKIP shim is registered here for consistency with the
        # else() branch structure.
        add_test(
            NAME test_hard_refuse_v3
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest not installed'); raise SystemExit(77)")
        set_tests_properties(test_hard_refuse_v3 PROPERTIES
            LABELS "smoke;phase-m6-invariant;phase-14-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)
    endif()
else()
    message(STATUS
        "Skipping tests/lsp/smoke: Python3=${Python3_FOUND}, "
        "IRON_BUILD_LSP=${IRON_BUILD_LSP}")
endif()
