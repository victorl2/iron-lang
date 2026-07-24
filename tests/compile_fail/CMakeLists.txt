# Negative corpus: each fixture MUST fail `ironc check` and stderr MUST
# contain the sibling .expected substring (full message text, richer than the
# bare-code assertions in v4-fail). Runs through the same run_tests.sh
# negative handler as v4-fail. Shell-script based tests cannot run on Windows.
if(NOT WIN32)
    add_test(
        NAME test_compile_fail
        COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
                compile_fail
                ${CMAKE_BINARY_DIR}/ironc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_compile_fail PROPERTIES LABELS "integration;compile-fail")
endif()
