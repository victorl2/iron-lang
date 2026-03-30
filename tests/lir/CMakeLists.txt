# IR tests: C test executables linked against iron_compiler and Unity.
# All tests are labelled "ir" for ctest -L ir filtering.

add_executable(test_lir_data test_lir_data.c)
target_link_libraries(test_lir_data iron_compiler unity)
add_test(NAME test_lir_data COMMAND test_lir_data)
set_tests_properties(test_lir_data PROPERTIES LABELS "lir")

add_executable(test_lir_print test_lir_print.c)
target_link_libraries(test_lir_print iron_compiler unity)
add_test(NAME test_lir_print COMMAND test_lir_print
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_lir_print PROPERTIES LABELS "lir")

add_executable(test_lir_verify test_lir_verify.c)
target_link_libraries(test_lir_verify iron_compiler unity)
add_test(NAME test_lir_verify COMMAND test_lir_verify)
set_tests_properties(test_lir_verify PROPERTIES LABELS "lir")

add_executable(test_lir_emit test_lir_emit.c)
target_link_libraries(test_lir_emit iron_compiler unity)
add_test(NAME test_lir_emit COMMAND test_lir_emit)
set_tests_properties(test_lir_emit PROPERTIES LABELS "lir")

add_executable(test_lir_optimize test_lir_optimize.c)
target_link_libraries(test_lir_optimize iron_compiler unity)
add_test(NAME test_lir_optimize COMMAND test_lir_optimize)
set_tests_properties(test_lir_optimize PROPERTIES LABELS "lir")
