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

        # Phase 17 Plan 03 closeout: VAL-01 publishDiagnostics
        # regression-anchor smoke. NOT a Wave 0 RED gate -- by the time
        # this test runs, Plans 17-01/02 have already committed
        # IRON_ERR_MISSING_VAL_VAR=176. This is a regression anchor
        # verifying the CORE-22 facade flows new compiler-side
        # diagnostics through publishDiagnostics with zero LSP-side
        # change. Phase 34 LSP-06 leans on this assertion shape when
        # wiring quickfix code-actions.
        add_test(
            NAME lsp_phase17_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_val_var.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase17_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase17-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 18 Plan 03 closeout: PARM-01 + PARM-03 publishDiagnostics
        # regression-anchor smoke. NOT a Wave 0 RED gate -- by the time
        # this test runs, Plans 18-01/02 have already committed
        # IRON_ERR_PARM_READ_ONLY=266 and
        # IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267. This is a regression
        # anchor verifying the CORE-22 facade flows new compiler-side
        # diagnostics through publishDiagnostics with zero LSP-side
        # change. Phase 34 LSP-06 leans on this assertion shape when
        # wiring quickfix code-actions for parameter modifiers.
        add_test(
            NAME lsp_phase18_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_parm_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase18_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase18-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 20 Plan 03 closeout: PTR-11 + PTR-13 publishDiagnostics
        # regression-anchor smoke. NOT a Wave 0 RED gate -- by the time
        # this test runs, Plans 20-01/02a/02b have already committed
        # IRON_ERR_PTR_NO_ARITH=268 and IRON_ERR_PTR_NULL_DEREF=272.
        # This is a regression anchor verifying the CORE-22 facade
        # flows new compiler-side diagnostics through publishDiagnostics
        # with zero LSP-side change. Phase 34 LSP-06 leans on this
        # assertion shape when wiring quickfix code-actions for
        # checked-pointer diagnostics.
        add_test(
            NAME lsp_phase20_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_ptr_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase20_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase20-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 21 Plan 03 closeout: POL-03 + POL-04 publishDiagnostics
        # regression-anchor smoke. NOT a Wave 0 RED gate -- by the time
        # this test runs, Plans 21-01/02 have already committed
        # IRON_ERR_HEAP_BAD_POSITION=273 (POL-03) and
        # IRON_ERR_FREE_NOT_BINDING=274 (POL-04). This is a regression
        # anchor verifying the CORE-22 facade flows new compiler-side
        # diagnostics through publishDiagnostics with zero LSP-side change.
        # Phase 34 LSP-06 leans on this assertion shape when wiring
        # quickfix code-actions for heap-policy diagnostics.
        add_test(
            NAME lsp_phase21_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_pol_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase21_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase21-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 22 Plan 03 closeout: READ-04 + READ-06 publishDiagnostics
        # regression-anchor smoke. NOT a Wave 0 RED gate -- by the time
        # this test runs, Plans 22-01/02 have already committed
        # IRON_ERR_READONLY_IO=278 (READ-04) and
        # IRON_ERR_READONLY_RETURN_TYPE=280 (READ-06). This is a regression
        # anchor verifying the CORE-22 facade flows new compiler-side
        # diagnostics through publishDiagnostics with zero LSP-side change.
        # Phase 34 LSP-10 leans on this assertion shape when wiring
        # quickfix code-actions for readonly diagnostics.
        add_test(
            NAME lsp_phase22_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_readonly_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase22_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase22-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 23 Plan 03 closeout: VEC-04 (E0282) + VEC-283 (E0283) publishDiagnostics
        # regression-anchor smoke. NOT a Wave 0 RED gate — by the time this test runs,
        # Plans 23-01/02 have already committed IRON_ERR_VEC_STRICT_LENGTH_MISMATCH=282
        # and IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN=283. Verifies CORE-22 facade
        # flows new VEC codes through publishDiagnostics with zero LSP-side change.
        # Phase 34 LSP-10 leans on this assertion shape when wiring quickfix
        # code-actions for VEC diagnostics.
        add_test(
            NAME lsp_phase23_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_vec_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase23_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase23-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 24 Plan 24-03 closeout: DROP-01 (E0284) + DROP-08 (E0286) publishDiagnostics
        # regression-anchor smoke. NOT a Wave 0 RED gate — by the time this test runs,
        # Plans 24-01/02 have already committed IRON_ERR_DROP_DUPLICATE=284 (DROP-01) and
        # IRON_ERR_COPY_OF_NOCOPY_TYPE=286 (DROP-08). Verifies CORE-22 facade flows new
        # DROP codes through publishDiagnostics with zero LSP-side change.
        # Phase 34 LSP-10 leans on this assertion shape when wiring quickfix
        # code-actions for DROP diagnostics.
        add_test(
            NAME lsp_phase24_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_drop_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase24_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase24-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 25 Plan 25-03 closeout: PTR-02/03 (E0289) + PTR-05 (E0294) +
        # UNCK-06 (E0295) publishDiagnostics regression-anchor smoke. NOT a
        # Wave 0 RED gate — by the time this test runs, Plans 25-01/02 have
        # already committed IRON_ERR_PTR_REGIME_MISMATCH=289,
        # IRON_ERR_PTR_AMP_NOT_UNCHECKED=294, IRON_ERR_PTR_ARITH_CHECKED=295.
        # Verifies CORE-22 facade flows new PTR + UNCK codes through
        # publishDiagnostics with zero LSP-side change.
        # Phase 34 LSP-06 leans on this assertion shape when wiring quickfix
        # code-actions for PTR-02/03/05 + UNCK-06 diagnostics.
        add_test(
            NAME lsp_phase25_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_unchecked_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase25_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase25-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 26 Plan 26-03 closeout: POL-07 (E0296) + POL-11 (E0297, E0298)
        # publishDiagnostics regression-anchor smoke. CORE-22 facade invariant:
        # zero src/lsp/ source modifications across Phase 26.
        # The E0298 assertion verifies the canonical closed set
        # {stack, heap, rc, weak rc} surfaces through publishDiagnostics
        # (Blocker #1: full lattice, NOT a reduced {heap, rc} subset).
        # Phase 34 LSP-06 leans on this assertion shape when wiring quickfix
        # code-actions for E0296/E0297/E0298 diagnostics.
        add_test(
            NAME lsp_phase26_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_rc_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase26_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase26-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 27 Plan 27-03 closeout: POL-08 (E0299, E0300) + Phase 27 GA4
        # E0296 message-extension publishDiagnostics regression-anchor smoke.
        # CORE-22 facade invariant: zero src/lsp/ source modifications across
        # Phase 27. The E0296 assertion verifies the extended message naming
        # both `rc` and `weak rc` (Phase 27 GA4 — no new code allocated;
        # message text proves the Plan 27-02 extension landed). Phase 34
        # LSP-06 leans on this assertion shape when wiring quickfix code-
        # actions for E0299/E0300/E0296-ext diagnostics.
        add_test(
            NAME lsp_phase27_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_weak_rc_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase27_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase27-invariant"
            TIMEOUT 60
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")

        # Phase 28 Plan 28-01 Task 3: lsp_phase28_smoke (CORE-22 arena facade).
        # Wave 0 RED regression-anchor verifying E0301 (rc/weak-rc-in-arena)
        # surfaces through publishDiagnostics for an `in arena { val x = rc T(...) }`
        # buffer. CORE-22 facade invariant: zero src/lsp/ source modifications —
        # the existing facade auto-pipes new compiler-side diagnostics, so LSP
        # E0301 == CLI E0301 with no LSP code change. RED until Plan 28-03 lands
        # IRON_ERR_RC_IN_ARENA=301; mirrors the Phase 27 lsp_phase27_smoke pair.
        # FLAT path (test_did_publish_arena_violation.py lives directly in
        # tests/lsp/smoke/, NOT a phase28/ subdir) so it shares the existing
        # conftest.py and is also picked up by the directory-level lsp_smoke row;
        # this dedicated row gives it an independent phase28-invariant gate.
        add_test(
            NAME lsp_phase28_smoke
            COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_did_publish_arena_violation.py
                --lsp-binary=$<TARGET_FILE:ironls>
                --no-header
                -p no:cacheprovider
                -v
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(lsp_phase28_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase28-invariant"
            TIMEOUT 60
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

        # Phase 17 Plan 03 closeout: SKIP shim for lsp_phase17_smoke when
        # pytest-lsp is absent. ctest -L phase17-invariant stays green.
        add_test(
            NAME lsp_phase17_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase17_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase17-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 18 Plan 03 closeout: SKIP shim for lsp_phase18_smoke when
        # pytest-lsp is absent. ctest -L phase18-invariant stays green.
        add_test(
            NAME lsp_phase18_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase18_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase18-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 20 Plan 03 closeout: SKIP shim for lsp_phase20_smoke when
        # pytest-lsp is absent. ctest -L phase20-invariant stays green.
        add_test(
            NAME lsp_phase20_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase20_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase20-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 21 Plan 03 closeout: SKIP shim for lsp_phase21_smoke when
        # pytest-lsp is absent. ctest -L phase21-invariant stays green.
        add_test(
            NAME lsp_phase21_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase21_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase21-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 22 Plan 03 closeout: SKIP shim for lsp_phase22_smoke when
        # pytest-lsp is absent. ctest -L phase22-invariant stays green.
        add_test(
            NAME lsp_phase22_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase22_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase22-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 23 Plan 03 closeout: SKIP shim for lsp_phase23_smoke when
        # pytest-lsp is absent. ctest -L phase23-invariant stays green.
        add_test(
            NAME lsp_phase23_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase23_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase23-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 24 Plan 24-03 closeout: SKIP shim for lsp_phase24_smoke when
        # pytest-lsp is absent. ctest -L phase24-invariant stays green.
        add_test(
            NAME lsp_phase24_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase24_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase24-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 25 Plan 25-03 closeout: SKIP shim for lsp_phase25_smoke when
        # pytest-lsp is absent. ctest -L phase25-invariant stays green.
        add_test(
            NAME lsp_phase25_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase25_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase25-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 27 Plan 27-03 closeout: SKIP shim for lsp_phase27_smoke when
        # pytest-lsp is absent. ctest -L phase27-invariant stays green.
        add_test(
            NAME lsp_phase27_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase27_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase27-invariant"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

        # Phase 28 Plan 28-01 Task 3: SKIP shim for lsp_phase28_smoke when
        # pytest-lsp is absent. ctest -L phase28-invariant stays green.
        add_test(
            NAME lsp_phase28_smoke
            COMMAND ${Python3_EXECUTABLE} -c "print('pytest-lsp not installed'); raise SystemExit(77)")
        set_tests_properties(lsp_phase28_smoke PROPERTIES
            LABELS "smoke;lsp_smoke;phase28-invariant"
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
