# Unit tests: C test executables linked against iron_compiler, iron_runtime,
# iron_stdlib, and the Unity test framework.
# All tests are labelled "unit" for ctest -L unit filtering.

# ── Compiler unit tests ──────────────────────────────────────────────────────
add_executable(test_arena test_arena.c)
target_link_libraries(test_arena iron_compiler unity)
add_test(NAME test_arena COMMAND test_arena)
set_tests_properties(test_arena PROPERTIES LABELS "unit")

add_executable(test_diagnostics test_diagnostics.c)
target_link_libraries(test_diagnostics iron_compiler unity)
add_test(NAME test_diagnostics COMMAND test_diagnostics)
set_tests_properties(test_diagnostics PROPERTIES LABELS "unit")

add_executable(test_lexer test_lexer.c)
target_link_libraries(test_lexer iron_compiler unity)
add_test(NAME test_lexer COMMAND test_lexer)
set_tests_properties(test_lexer PROPERTIES LABELS "unit")

add_executable(test_parser test_parser.c)
target_link_libraries(test_parser iron_compiler unity)
add_test(NAME test_parser COMMAND test_parser)
set_tests_properties(test_parser PROPERTIES LABELS "unit")

add_executable(test_parser_errors test_parser_errors.c)
target_link_libraries(test_parser_errors iron_compiler unity)
add_test(NAME test_parser_errors COMMAND test_parser_errors)
set_tests_properties(test_parser_errors PROPERTIES LABELS "unit")

add_executable(test_interp test_interp.c)
target_link_libraries(test_interp iron_compiler unity)
add_test(NAME test_interp COMMAND test_interp)
set_tests_properties(test_interp PROPERTIES LABELS "unit")

add_executable(test_printer test_printer.c)
target_link_libraries(test_printer iron_compiler unity)
add_test(NAME test_printer COMMAND test_printer)
set_tests_properties(test_printer PROPERTIES LABELS "unit")

add_executable(test_types test_types.c)
target_link_libraries(test_types iron_compiler unity)
add_test(NAME test_types COMMAND test_types)
set_tests_properties(test_types PROPERTIES LABELS "unit")

add_executable(test_resolver test_resolver.c)
target_link_libraries(test_resolver iron_compiler unity)
add_test(NAME test_resolver COMMAND test_resolver)
set_tests_properties(test_resolver PROPERTIES LABELS "unit")

add_executable(test_typecheck test_typecheck.c)
target_link_libraries(test_typecheck iron_compiler unity)
add_test(NAME test_typecheck COMMAND test_typecheck)
set_tests_properties(test_typecheck PROPERTIES LABELS "unit")

add_executable(test_escape test_escape.c)
target_link_libraries(test_escape iron_compiler unity)
add_test(NAME test_escape COMMAND test_escape)
set_tests_properties(test_escape PROPERTIES LABELS "unit")

add_executable(test_concurrency test_concurrency.c)
target_link_libraries(test_concurrency iron_compiler unity)
add_test(NAME test_concurrency COMMAND test_concurrency)
set_tests_properties(test_concurrency PROPERTIES LABELS "unit")

add_executable(test_init_check test_init_check.c)
target_link_libraries(test_init_check iron_compiler unity)
add_test(NAME test_init_check COMMAND test_init_check)
set_tests_properties(test_init_check PROPERTIES LABELS "unit")

add_executable(test_comptime test_comptime.c)
target_link_libraries(test_comptime iron_compiler unity)
add_test(NAME test_comptime COMMAND test_comptime)
set_tests_properties(test_comptime PROPERTIES LABELS "unit")

# ── Runtime unit tests ───────────────────────────────────────────────────────
add_executable(test_runtime_string test_runtime_string.c)
target_link_libraries(test_runtime_string iron_runtime unity)
add_test(NAME test_runtime_string COMMAND test_runtime_string)
set_tests_properties(test_runtime_string PROPERTIES LABELS "unit")

add_executable(test_runtime_threads test_runtime_threads.c)
target_link_libraries(test_runtime_threads iron_runtime unity)
add_test(NAME test_runtime_threads COMMAND test_runtime_threads)
set_tests_properties(test_runtime_threads PROPERTIES LABELS "unit")

add_executable(test_runtime_collections test_runtime_collections.c)
target_link_libraries(test_runtime_collections iron_runtime unity)
add_test(NAME test_runtime_collections COMMAND test_runtime_collections)
set_tests_properties(test_runtime_collections PROPERTIES LABELS "unit")

# ── Stdlib unit test ─────────────────────────────────────────────────────────
add_executable(test_stdlib test_stdlib.c)
target_link_libraries(test_stdlib iron_stdlib iron_runtime unity)
add_test(NAME test_stdlib COMMAND test_stdlib)
set_tests_properties(test_stdlib PROPERTIES LABELS "unit")
