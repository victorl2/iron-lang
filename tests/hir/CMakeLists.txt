# HIR tests: C test executables linked against iron_compiler and Unity.
# All tests are labelled "hir" for ctest -L hir filtering.

add_executable(test_hir_data test_hir_data.c)
target_link_libraries(test_hir_data iron_compiler unity)
add_test(NAME test_hir_data COMMAND test_hir_data
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_hir_data PROPERTIES LABELS "hir")

add_executable(test_hir_print test_hir_print.c)
target_link_libraries(test_hir_print iron_compiler unity)
add_test(NAME test_hir_print COMMAND test_hir_print
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_hir_print PROPERTIES LABELS "hir")

add_executable(test_hir_verify test_hir_verify.c)
target_link_libraries(test_hir_verify iron_compiler unity)
add_test(NAME test_hir_verify COMMAND test_hir_verify
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_hir_verify PROPERTIES LABELS "hir")

add_executable(test_hir_lower test_hir_lower.c)
target_link_libraries(test_hir_lower iron_compiler unity)
add_test(NAME test_hir_lower COMMAND test_hir_lower
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_hir_lower PROPERTIES LABELS "hir")

add_executable(test_hir_to_lir test_hir_to_lir.c)
target_link_libraries(test_hir_to_lir iron_compiler unity)
add_test(NAME test_hir_to_lir COMMAND test_hir_to_lir
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_hir_to_lir PROPERTIES LABELS "hir")
