# Phase 35 MIG-09: v3 fixtures hand-migrated to v4 syntax.
# Registered as part of the v4-acceptance corpus.
#
# tests/run_tests.sh `v4` category already recurses (find -name '*.iron'),
# so fixtures under v4/migrated-from-v3/** are automatically picked up by
# test_v4_acceptance. This file adds a dedicated test name + category
# (v4-migrated) so the migrated corpus can be invoked in isolation for
# tighter iteration:
#
#   ctest --test-dir build -R test_v4_migrated_from_v3 --output-on-failure
#
# The v4-migrated category in run_tests.sh routes to
# tests/integration/v4/migrated-from-v3/ and uses the same recursive walk
# as the v4 category.
if(NOT WIN32)
    add_test(
        NAME test_v4_migrated_from_v3
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                v4-migrated
                ${CMAKE_BINARY_DIR}/iron
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_v4_migrated_from_v3 PROPERTIES
        LABELS "v4-acceptance;phase35-invariant"
    )
endif()
