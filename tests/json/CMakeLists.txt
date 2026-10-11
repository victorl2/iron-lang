# JSON module conformance: nst/JSONTestSuite through the json module.
if(NOT WIN32)
    add_test(
        NAME test_json_suite
        COMMAND ${CMAKE_SOURCE_DIR}/tests/json/run_json_suite.sh
                ${CMAKE_BINARY_DIR}/ironc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_json_suite PROPERTIES LABELS "stdlib")
endif()
