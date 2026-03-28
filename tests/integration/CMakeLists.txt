# Integration tests: compile .iron files to native binaries and verify output.
# Shell-script based tests cannot run on Windows.
if(NOT WIN32)
    add_test(
        NAME test_integration
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                integration
                ${CMAKE_BINARY_DIR}/ironc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_integration PROPERTIES LABELS "integration")
endif()
