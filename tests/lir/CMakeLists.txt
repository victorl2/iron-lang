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

# ── Phase 29 Atomic Refcount Elision — phase29-invariant Wave 0 RED anchors ──
# These two harnesses fail to LINK against iron_compiler until Plans 02/03 add
# func_summary.c (iron_lir_compute_func_summaries et al.) and the
# run_rc_pair_elimination definition in lir_optimize.c — the intended TDD RED.
add_executable(test_rc_pair_elim test_rc_pair_elim.c)
target_link_libraries(test_rc_pair_elim iron_compiler unity)
add_test(NAME test_rc_pair_elim COMMAND test_rc_pair_elim)
set_tests_properties(test_rc_pair_elim PROPERTIES LABELS "lir;phase29-invariant")

# OPT-01 gate row: a separate CTest NAME pointing at the same executable. The
# gate-off contract (pass mutates only when called) is asserted by
# test_gate_off_leaves_count_zero within the same binary.
add_test(NAME test_rc_pair_elim_gate COMMAND test_rc_pair_elim)
set_tests_properties(test_rc_pair_elim_gate PROPERTIES LABELS "lir;phase29-invariant")

add_executable(test_func_summary test_func_summary.c)
target_link_libraries(test_func_summary iron_compiler unity)
add_test(NAME test_func_summary COMMAND test_func_summary)
set_tests_properties(test_func_summary PROPERTIES LABELS "lir;phase29-invariant")
