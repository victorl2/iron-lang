# IR tests: C test executables linked against iron_compiler and Unity.
# All tests are labelled "ir" for ctest -L ir filtering.

add_executable(test_ir_data test_ir_data.c)
target_link_libraries(test_ir_data iron_compiler unity)
add_test(NAME test_ir_data COMMAND test_ir_data)
set_tests_properties(test_ir_data PROPERTIES LABELS "ir")

add_executable(test_ir_print test_ir_print.c)
target_link_libraries(test_ir_print iron_compiler unity)
add_test(NAME test_ir_print COMMAND test_ir_print
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_ir_print PROPERTIES LABELS "ir")

add_executable(test_ir_verify test_ir_verify.c)
target_link_libraries(test_ir_verify iron_compiler unity)
add_test(NAME test_ir_verify COMMAND test_ir_verify)
set_tests_properties(test_ir_verify PROPERTIES LABELS "ir")

add_executable(test_ir_lower test_ir_lower.c)
target_link_libraries(test_ir_lower iron_compiler unity)
add_test(NAME test_ir_lower COMMAND test_ir_lower
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_ir_lower PROPERTIES LABELS "ir")

add_executable(test_ir_emit test_ir_emit.c)
target_link_libraries(test_ir_emit iron_compiler unity)
add_test(NAME test_ir_emit COMMAND test_ir_emit)
set_tests_properties(test_ir_emit PROPERTIES LABELS "ir")

add_executable(test_ir_optimize test_ir_optimize.c)
target_link_libraries(test_ir_optimize iron_compiler unity)
add_test(NAME test_ir_optimize COMMAND test_ir_optimize)
set_tests_properties(test_ir_optimize PROPERTIES LABELS "ir")
