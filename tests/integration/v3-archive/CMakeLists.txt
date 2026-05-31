# Phase 35 MIG-08: archived v3 corpus. Registered only when
# IRON_RUN_ARCHIVED_V3_CORPUS=ON (gate declared in root CMakeLists.txt and
# evaluated in tests/integration/CMakeLists.txt before this file is added).
#
# v3 fixtures are NOT expected to pass under the v4 compiler -- this test
# exists for archeology / diff purposes. The opt-in gate keeps the default
# ctest run from inheriting hundreds of XFAIL results on every build.
#
# The harness invocation mirrors test_v4_acceptance: run_tests.sh accepts
# the category name `v3-archive`, which is rerouted to
# ${SCRIPT_DIR}/integration/v3-archive inside the script.
if(NOT WIN32)
    add_test(
        NAME test_integration_v3_archive
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                v3-archive
                ${CMAKE_BINARY_DIR}/iron
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_integration_v3_archive PROPERTIES
        LABELS "v3-archive;archeology"
    )
endif()
