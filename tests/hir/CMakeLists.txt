# HIR tests: C test executables linked against iron_compiler and Unity.
# All tests are labelled "hir" for ctest -L hir filtering.

add_executable(test_hir_data test_hir_data.c)
target_link_libraries(test_hir_data iron_compiler unity)
add_test(NAME test_hir_data COMMAND test_hir_data
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_hir_data PROPERTIES LABELS "hir")
