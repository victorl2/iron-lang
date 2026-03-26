cmake_minimum_required(VERSION 3.25)
project(iron C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Werror -Wpedantic)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── Unity test framework ────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.1
)
FetchContent_MakeAvailable(unity)
# Enable double-precision float assertions for all Unity test targets
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

# ── iron_runtime static library ────────────────────────────────────────────
# stb_ds_impl.c lives here so runtime tests can link without iron_compiler.
add_library(iron_runtime STATIC
    src/util/stb_ds_impl.c
    src/runtime/iron_string.c
    src/runtime/iron_rc.c
    src/runtime/iron_builtins.c
    src/runtime/iron_threads.c
    src/runtime/iron_collections.c
)
target_include_directories(iron_runtime PUBLIC src src/vendor)
target_link_libraries(iron_runtime pthread)

# ── iron_compiler static library ───────────────────────────────────────────
# NOTE: More source files will be added in later plans.
add_library(iron_compiler STATIC
    src/util/arena.c
    src/util/strbuf.c
    src/diagnostics/diagnostics.c
    src/lexer/lexer.c
    src/parser/ast.c
    src/parser/parser.c
    src/parser/printer.c
    src/analyzer/types.c
    src/analyzer/scope.c
    src/analyzer/resolve.c
    src/analyzer/typecheck.c
    src/analyzer/escape.c
    src/analyzer/concurrency.c
    src/analyzer/analyzer.c
    src/codegen/codegen.c
    src/codegen/gen_types.c
    src/codegen/gen_stmts.c
    src/codegen/gen_exprs.c
)
target_include_directories(iron_compiler PUBLIC src src/vendor)
target_link_libraries(iron_compiler iron_runtime)

# ── Tests ──────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_arena tests/test_arena.c)
target_link_libraries(test_arena iron_compiler unity)
add_test(NAME test_arena COMMAND test_arena)

add_executable(test_diagnostics tests/test_diagnostics.c)
target_link_libraries(test_diagnostics iron_compiler unity)
add_test(NAME test_diagnostics COMMAND test_diagnostics)

add_executable(test_lexer tests/test_lexer.c)
target_link_libraries(test_lexer iron_compiler unity)
add_test(NAME test_lexer COMMAND test_lexer)

add_executable(test_parser tests/test_parser.c)
target_link_libraries(test_parser iron_compiler unity)
add_test(NAME test_parser COMMAND test_parser)

add_executable(test_parser_errors tests/test_parser_errors.c)
target_link_libraries(test_parser_errors iron_compiler unity)
add_test(NAME test_parser_errors COMMAND test_parser_errors)

add_executable(test_interp tests/test_interp.c)
target_link_libraries(test_interp iron_compiler unity)
add_test(NAME test_interp COMMAND test_interp)

add_executable(test_printer tests/test_printer.c)
target_link_libraries(test_printer iron_compiler unity)
add_test(NAME test_printer COMMAND test_printer)

add_executable(test_types tests/test_types.c)
target_link_libraries(test_types iron_compiler unity)
add_test(NAME test_types COMMAND test_types)

add_executable(test_resolver tests/test_resolver.c)
target_link_libraries(test_resolver iron_compiler unity)
add_test(NAME test_resolver COMMAND test_resolver)

add_executable(test_typecheck tests/test_typecheck.c)
target_link_libraries(test_typecheck iron_compiler unity)
add_test(NAME test_typecheck COMMAND test_typecheck)

add_executable(test_escape tests/test_escape.c)
target_link_libraries(test_escape iron_compiler unity)
add_test(NAME test_escape COMMAND test_escape)

add_executable(test_concurrency tests/test_concurrency.c)
target_link_libraries(test_concurrency iron_compiler unity)
add_test(NAME test_concurrency COMMAND test_concurrency)

add_executable(test_codegen tests/test_codegen.c)
target_link_libraries(test_codegen iron_compiler unity)
add_test(NAME test_codegen COMMAND test_codegen)

add_executable(test_pipeline tests/test_pipeline.c)
target_link_libraries(test_pipeline iron_compiler unity)
add_test(NAME test_pipeline COMMAND test_pipeline)

add_executable(test_runtime_string tests/test_runtime_string.c)
target_link_libraries(test_runtime_string iron_runtime unity)
add_test(NAME test_runtime_string COMMAND test_runtime_string)

add_executable(test_runtime_threads tests/test_runtime_threads.c)
target_link_libraries(test_runtime_threads iron_runtime unity)
add_test(NAME test_runtime_threads COMMAND test_runtime_threads)

add_executable(test_runtime_collections tests/test_runtime_collections.c)
target_link_libraries(test_runtime_collections iron_runtime unity)
add_test(NAME test_runtime_collections COMMAND test_runtime_collections)

# ── iron_stdlib static library ─────────────────────────────────────────────
add_library(iron_stdlib STATIC
    src/stdlib/iron_math.c
    src/stdlib/iron_io.c
    src/stdlib/iron_time.c
    src/stdlib/iron_log.c
)
target_include_directories(iron_stdlib PUBLIC src)
target_link_libraries(iron_stdlib iron_runtime m)  # -lm for math

add_executable(test_stdlib tests/test_stdlib.c)
target_link_libraries(test_stdlib iron_stdlib iron_runtime unity)
add_test(NAME test_stdlib COMMAND test_stdlib)

# ── iron CLI executable ─────────────────────────────────────────────────────
add_executable(iron
    src/cli/main.c
    src/cli/build.c
    src/cli/check.c
)
target_include_directories(iron PRIVATE src)
target_link_libraries(iron iron_compiler iron_runtime iron_stdlib m pthread)
# Inject the absolute path to src/ so the CLI can find runtime sources at build time
target_compile_definitions(iron PRIVATE
    IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
)

# ── CLI smoke test ──────────────────────────────────────────────────────────
# iron with no args exits with code 1, but outputs "Usage: iron" to stderr.
# PASS_REGULAR_EXPRESSION matches stderr+stdout output regardless of exit code.
add_test(
    NAME test_cli_help
    COMMAND iron
)
set_tests_properties(test_cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: iron"
)
