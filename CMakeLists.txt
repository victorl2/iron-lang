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

# ── iron_compiler static library ───────────────────────────────────────────
# NOTE: More source files will be added in later plans.
add_library(iron_compiler STATIC
    src/util/arena.c
    src/util/strbuf.c
    src/util/stb_ds_impl.c
    src/diagnostics/diagnostics.c
    src/lexer/lexer.c
    src/parser/ast.c
    src/parser/parser.c
    src/parser/printer.c
    src/analyzer/types.c
    src/analyzer/scope.c
    src/analyzer/resolve.c
)
target_include_directories(iron_compiler PUBLIC src src/vendor)

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
