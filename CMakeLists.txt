cmake_minimum_required(VERSION 3.25)
project(iron VERSION 0.0.2 LANGUAGES C)

# ── Version info baked at compile time ─────────────────────────────────────
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE IRON_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT IRON_GIT_HASH)
    set(IRON_GIT_HASH "unknown")
endif()

string(TIMESTAMP IRON_BUILD_DATE "%Y-%m-%d" UTC)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
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
if(NOT WIN32)
  target_link_libraries(iron_runtime pthread)
endif()

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
    src/comptime/comptime.c
    src/ir/ir.c
    src/ir/lower.c
    src/ir/lower_types.c
    src/ir/lower_exprs.c
    src/ir/lower_stmts.c
    src/ir/emit_c.c
    src/ir/print.c
    src/ir/verify.c
)
target_include_directories(iron_compiler PUBLIC src src/vendor)
target_link_libraries(iron_compiler iron_runtime)

# ── iron_stdlib static library ─────────────────────────────────────────────
add_library(iron_stdlib STATIC
    src/stdlib/iron_math.c
    src/stdlib/iron_io.c
    src/stdlib/iron_time.c
    src/stdlib/iron_log.c
)
target_include_directories(iron_stdlib PUBLIC src)
if(NOT WIN32)
  target_link_libraries(iron_stdlib iron_runtime m)  # -lm for math
else()
  target_link_libraries(iron_stdlib iron_runtime)
endif()

# ── Tests ──────────────────────────────────────────────────────────────────
enable_testing()

add_subdirectory(tests/unit)
add_subdirectory(tests/ir)
add_subdirectory(tests/integration)
add_subdirectory(tests/algorithms)
add_subdirectory(tests/composite)
add_subdirectory(tests/manual)

# ── ironc compiler executable ────────────────────────────────────────────────
add_executable(ironc
    src/cli/main.c
    src/cli/build.c
    src/cli/check.c
    src/cli/fmt.c
    src/cli/test_runner.c
    src/cli/toml.c
)
target_include_directories(ironc PRIVATE src)
if(NOT WIN32)
  target_link_libraries(ironc iron_compiler iron_runtime iron_stdlib m pthread)
else()
  target_link_libraries(ironc iron_compiler iron_runtime iron_stdlib)
endif()
# Inject the absolute path to src/ so the CLI can find runtime sources at build time
target_compile_definitions(ironc PRIVATE
    IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
    IRON_VERSION_STRING="${PROJECT_VERSION}"
    IRON_GIT_HASH="${IRON_GIT_HASH}"
    IRON_BUILD_DATE="${IRON_BUILD_DATE}"
    IRON_BINARY_NAME="ironc"
)

# ── iron package manager executable ─────────────────────────────────────────
# iron does NOT link iron_compiler or iron_runtime -- clean process boundary.
# It only spawns ironc as a subprocess.
add_executable(iron
    src/pkg/main.c
)
target_include_directories(iron PRIVATE src)
target_compile_definitions(iron PRIVATE
    IRON_VERSION_STRING="${PROJECT_VERSION}"
    IRON_GIT_HASH="${IRON_GIT_HASH}"
    IRON_BUILD_DATE="${IRON_BUILD_DATE}"
)

# ── CLI smoke tests ──────────────────────────────────────────────────────────
add_test(
    NAME test_ironc_help
    COMMAND ironc
)
set_tests_properties(test_ironc_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: ironc"
)

add_test(
    NAME test_ironc_version
    COMMAND ironc --version
)
set_tests_properties(test_ironc_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^ironc [0-9]"
)

# iron CLI tests
add_test(
    NAME test_iron_help
    COMMAND iron
)
set_tests_properties(test_iron_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Package Commands:"
)

add_test(
    NAME test_iron_version
    COMMAND iron --version
)
set_tests_properties(test_iron_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^iron [0-9]"
)

# ---- Install rules ----
install(TARGETS ironc iron DESTINATION bin)

# Install runtime C source files (needed by ironc at compile time)
install(DIRECTORY src/runtime/ DESTINATION lib/runtime FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
# Install util files used by the build pipeline
install(FILES
    src/util/stb_ds_impl.c
    src/util/arena.c
    src/util/arena.h
    src/util/strbuf.c
    src/util/strbuf.h
    DESTINATION lib/util
)
# Install all header files from src/ that ironc passes via -I (runtime, vendor)
install(DIRECTORY src/vendor/ DESTINATION lib/vendor)
# Install stdlib C implementations and headers
install(DIRECTORY src/stdlib/ DESTINATION lib/stdlib FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
# Install stdlib .iron files
install(DIRECTORY src/stdlib/ DESTINATION lib/stdlib FILES_MATCHING PATTERN "*.iron")
