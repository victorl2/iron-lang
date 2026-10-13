# v4-acceptance positive corpus: §3-§8 spec examples plus 5+ programs per
# closed-set policy / checked-pointer / unchecked / readonly / drop-copy /
# arena scenario. Phase 15 baseline = 0% PASS (every fixture XFAIL because
# no v4 implementation exists yet). Each subsequent phase increases the pass
# count by >= N (N set per phase based on the corpus subset that phase enables).
if(NOT WIN32)
    # Independent serial shards let CTest show corpus progress and name the
    # failing slice. CTest supplies the parallelism, so each shard runs one
    # compiler at a time and the total stays inside its -j budget.
    foreach(_shard RANGE 1 8)
        add_test(
            NAME test_v4_acceptance_${_shard}
            COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                    v4
                    ${CMAKE_BINARY_DIR}/iron
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        # Wire the configured phase gate into the harness: run_tests.sh falls
        # back to 15 when absent, which would decouple this aggregate from the
        # per-fixture rows.
        set_tests_properties(test_v4_acceptance_${_shard} PROPERTIES
            LABELS "v4-acceptance"
            ENVIRONMENT "IRON_CURRENT_PHASE=${IRON_CURRENT_PHASE};IRON_TEST_SHARD=${_shard}/8;IRON_TEST_JOBS=1"
            PROCESSORS 1
            TIMEOUT 7200)
    endforeach()
endif()

# Optimizer parity oracle: every runnable v4 / algorithms fixture must behave
# the same when built with and without --no-optimize (see
# tests/oracles/opt_parity.sh). The two pipelines implement one language, so a
# difference is a compiler bug even when the optimized build matches .expected.
if(NOT WIN32)
    add_test(
        NAME test_opt_parity
        COMMAND ${CMAKE_SOURCE_DIR}/tests/oracles/opt_parity.sh
                ${CMAKE_BINARY_DIR}/ironc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    # Not in the v4-acceptance label: it builds every fixture twice, which
    # pushed that 30-minute gate over its limit. The full test jobs run it.
    # These exhaustive oracles each run an internal worker pool. Reserve a
    # four-core runner and serialize them with one another so ctest -j4 does
    # not multiply that parallelism into timeouts.
    set_tests_properties(test_opt_parity PROPERTIES
        LABELS "oracle"
        PROCESSORS ${IRON_TEST_JOBS}
        RESOURCE_LOCK iron_exhaustive_corpus
        TIMEOUT 7200)
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
    file(GLOB_RECURSE _leak_sources CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/integration/v4/*.iron)
    set(_leak_shard 1)
    foreach(_leak_source IN LISTS _leak_sources)
        list(APPEND _leak_sources_${_leak_shard} "${_leak_source}")
        math(EXPR _leak_shard "(${_leak_shard} % 8) + 1")
    endforeach()
    foreach(_shard RANGE 1 8)
        if(_leak_sources_${_shard})
            add_test(
                NAME test_leak_check_${_shard}
                COMMAND ${CMAKE_SOURCE_DIR}/tests/oracles/leak_check.sh
                        ${CMAKE_BINARY_DIR}/iron ${_leak_sources_${_shard}}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            )
            set_tests_properties(test_leak_check_${_shard} PROPERTIES
                LABELS "oracle;leak"
                ENVIRONMENT "LEAK_CHECK_JOBS=1"
                PROCESSORS 1
                TIMEOUT 7200)
        endif()
    endforeach()
endif()

# Crypto cross-check: the crypto and encoding modules against Python's
# hashlib, hmac, base64 and (when installed) cryptography on a few thousand
# random inputs with a fresh seed each run (printed on failure).
if(NOT WIN32)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_test(
            NAME test_crypto_crosscheck
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/oracles/crypto_crosscheck.py
                    ${CMAKE_BINARY_DIR}/iron --cases 3000
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(test_crypto_crosscheck PROPERTIES
            LABELS "oracle;crypto"
            TIMEOUT 600)
    endif()
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
