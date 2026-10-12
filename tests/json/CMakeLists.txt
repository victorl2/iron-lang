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
add_test(NAME test_json_float_format COMMAND json_float_check 400000)
set_tests_properties(test_json_float_format PROPERTIES LABELS "stdlib")

# The benchmark's documented markdown formatter must be directly runnable.
if(NOT WIN32)
    add_test(
        NAME test_json_bench_table
        COMMAND ${CMAKE_SOURCE_DIR}/bench/json/table.py
                ${CMAKE_SOURCE_DIR}/tests/json/bench_table_fixture.txt
    )
    set_tests_properties(test_json_bench_table PROPERTIES
        LABELS "stdlib"
        PASS_REGULAR_EXPRESSION "Parse to a tree.*Iron \\(-O2\\).*1\\.000")
    add_test(
        NAME test_json_bench_capture
        COMMAND ${CMAKE_SOURCE_DIR}/tests/json/test_bench_capture.sh
    )
    set_tests_properties(test_json_bench_capture PROPERTIES LABELS "stdlib")
endif()

# Spec conformance beyond JSONTestSuite: JSON_checker, number round trips,
# escapes and UTF-8, structure and limits (tests/json/conformance).
if(NOT WIN32)
    add_test(
        NAME test_json_conformance
        COMMAND ${CMAKE_SOURCE_DIR}/tests/json/conformance/run_conformance.sh
                ${CMAKE_BINARY_DIR}/ironc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_json_conformance PROPERTIES LABELS "stdlib")
endif()

# Differential fuzzing against Python's json and yyjson: a short fixed-seed
# pass (tests/json/fuzz/run_difffuzz.sh runs longer ones).
add_executable(ref_yyjson ${CMAKE_SOURCE_DIR}/tests/json/fuzz/ref_yyjson.c
                          ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
target_include_directories(ref_yyjson PRIVATE ${CMAKE_SOURCE_DIR}/src/vendor/yyjson)
if(NOT WIN32)
    add_test(
        NAME test_json_difffuzz
        COMMAND ${CMAKE_SOURCE_DIR}/tests/json/fuzz/run_difffuzz.sh
                ${CMAKE_BINARY_DIR}/ironc $<TARGET_FILE:ref_yyjson> 3000 20261011
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_json_difffuzz PROPERTIES LABELS "stdlib")
endif()
