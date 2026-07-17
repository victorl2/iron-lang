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
    set_tests_properties(test_v4_acceptance PROPERTIES LABELS "v4-acceptance")
endif()

# Phase 35 MIG-09: migrated-from-v3/ subdir hosts the hand-migrated v3 corpus.
add_subdirectory(migrated-from-v3)
