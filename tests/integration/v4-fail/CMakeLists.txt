# v4-acceptance negative corpus: every §3-§10 compile error pinned to a
# fixture + .expected diagnostic substring. Phase 15 baseline = 0% PASS;
# fixtures may "pass for the wrong reason" at baseline (compiler may exit
# non-zero on unknown v4 keyword rather than the spec'd error) — the
# substring check is the real validation gate.
if(NOT WIN32)
    add_test(
        NAME test_v4_acceptance_fail
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                v4-fail
                ${CMAKE_BINARY_DIR}/ironc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    # Keep the negative-corpus harness on the same phase gate as the
    # per-fixture rows (run_tests.sh defaults to 15 without the env var).
    set_tests_properties(test_v4_acceptance_fail PROPERTIES
        LABELS "v4-acceptance"
        ENVIRONMENT "IRON_CURRENT_PHASE=${IRON_CURRENT_PHASE}")
endif()
