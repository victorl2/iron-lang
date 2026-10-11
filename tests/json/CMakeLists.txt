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

# The float formatter (Ryu) against a brute-force shortest search.
add_executable(json_float_check json_float_check.c)
target_include_directories(json_float_check PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/vendor)
target_link_libraries(json_float_check iron_runtime)
if(NOT WIN32)
    target_link_libraries(json_float_check m)
endif()
add_test(NAME test_json_float_format COMMAND json_float_check 2000000)
set_tests_properties(test_json_float_format PROPERTIES LABELS "stdlib")
