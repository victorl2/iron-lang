# Manual tests: .iron files that require manual inspection or interaction.
# Shell-script based tests cannot run on Windows.
if(NOT WIN32)
    add_test(
        NAME test_manual
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                manual
                ${CMAKE_BINARY_DIR}/iron
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_manual PROPERTIES LABELS "manual")
endif()
