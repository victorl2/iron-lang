# v4-acceptance positive corpus: §3-§8 spec examples plus 5+ programs per
# closed-set policy / checked-pointer / unchecked / readonly / drop-copy /
# arena scenario. Phase 15 baseline = 0% PASS (every fixture XFAIL because
# no v4 implementation exists yet). Each subsequent phase increases the pass
# count by >= N (N set per phase based on the corpus subset that phase enables).
if(NOT WIN32)
    add_test(
        NAME test_v4_acceptance
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                v4
                ${CMAKE_BINARY_DIR}/iron
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    # Wire the configured phase gate into the harness: run_tests.sh falls back
    # to IRON_CURRENT_PHASE=15 when the env var is absent, which silently
    # decouples this aggregate from the gate the per-fixture ctest rows use.
    set_tests_properties(test_v4_acceptance PROPERTIES
        LABELS "v4-acceptance"
        ENVIRONMENT "IRON_CURRENT_PHASE=${IRON_CURRENT_PHASE}")
endif()

# Optimizer parity oracle: every runnable v4 / algorithms fixture must behave
# the same when built with and without --no-optimize (see
# tests/oracles/opt_parity.sh). The two pipelines implement one language, so a
# difference is a compiler bug even when the optimized build matches .expected.
if(NOT WIN32)
    add_test(
        NAME test_opt_parity
        COMMAND ${CMAKE_SOURCE_DIR}/tests/oracles/opt_parity.sh
                ${CMAKE_BINARY_DIR}/iron
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    # Not in the v4-acceptance label: it builds every fixture twice, which
    # pushed that 30-minute gate over its limit. The full test jobs run it.
    # Under ASan on a 4-core CI runner it takes about 25 minutes next to the
    # other rows, so its own cap is an hour.
    set_tests_properties(test_opt_parity PROPERTIES
        LABELS "oracle"
        TIMEOUT 3600)
endif()

# Phase 35 MIG-09: migrated-from-v3/ subdir hosts the hand-migrated v3 corpus.
add_subdirectory(migrated-from-v3)

# Leak oracle (#193): every Map / Set fixture runs under `leaks` (macOS) or
# valgrind (Linux) and must release everything it allocated. The containers
# allocate with malloc, which the runtime's own IRON_LEAK_CHECK tracker does
# not see. Skips itself when no detector is installed.
# Leak oracle (#231): every runnable fixture of the corpus runs under leaks
# (macOS) or valgrind (Linux); the few that leak on purpose carry a
# `-- @leaks-by-design` marker.
if(NOT WIN32)
    file(GLOB _leak_dirs LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/tests/integration/v4/*/ ${CMAKE_SOURCE_DIR}/tests/integration/v4/*/*/)
    add_test(
        NAME test_leak_check
        COMMAND ${CMAKE_SOURCE_DIR}/tests/oracles/leak_check.sh
                ${CMAKE_BINARY_DIR}/iron ${_leak_dirs}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_leak_check PROPERTIES
        LABELS "oracle;v4-acceptance"
        TIMEOUT 2400)
endif()

# Freestanding oracle (#235): the C generated for every fixture compiles with
# -ffreestanding -nostdinc, seeing only clang's resource headers and the Iron
# source tree. Generated code may use nothing from the platform directly.
if(NOT WIN32)
    file(GLOB _fs_dirs LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/tests/integration/v4/*/ ${CMAKE_SOURCE_DIR}/tests/integration/v4/*/*/)
    add_test(
        NAME test_freestanding
        COMMAND ${CMAKE_SOURCE_DIR}/tests/oracles/freestanding.sh
                ${CMAKE_BINARY_DIR}/iron ${_fs_dirs}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_freestanding PROPERTIES
        LABELS "oracle"
        TIMEOUT 1800)
endif()
