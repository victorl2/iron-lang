# Algorithm tests: .iron files that verify algorithm correctness end-to-end.
# Shell-script based tests cannot run on Windows.
if(NOT WIN32)
    add_test(
        NAME test_algorithms
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                algorithms
                ${CMAKE_BINARY_DIR}/ironc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_algorithms PROPERTIES LABELS "algorithms")
endif()
