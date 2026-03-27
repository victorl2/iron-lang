# Composite tests: .iron files that combine multiple language features.
# Shell-script based tests cannot run on Windows.
if(NOT WIN32)
    add_test(
        NAME test_composite
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                composite
                ${CMAKE_BINARY_DIR}/iron
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_composite PROPERTIES LABELS "composite")
endif()
