cmake_minimum_required(VERSION 3.25)
project(iron VERSION 0.0.5 LANGUAGES C)

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
    src/analyzer/init_check.c
    src/analyzer/analyzer.c
    src/comptime/comptime.c
    src/hir/hir.c
    src/hir/hir_print.c
    src/hir/hir_verify.c
    src/hir/hir_lower.c
    src/hir/hir_to_lir.c
    src/lir/lir.c
    src/lir/emit_c.c
    src/lir/print.c
    src/lir/verify.c
    src/lir/lir_optimize.c
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
add_subdirectory(tests/lir)
add_subdirectory(tests/hir)
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
    src/pkg/init.c
    src/pkg/pkg_build.c
    src/pkg/fetcher.c
    src/pkg/resolver.c
    src/pkg/lockfile.c
    src/cli/toml.c
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

add_test(
    NAME test_iron_no_color
    COMMAND ${CMAKE_COMMAND} -E env NO_COLOR=1 $<TARGET_FILE:iron>
)
set_tests_properties(test_iron_no_color PROPERTIES
    PASS_REGULAR_EXPRESSION "Package Commands:"
)

# iron init tests
if(NOT WIN32)
    add_test(
        NAME test_iron_init
        COMMAND ${CMAKE_COMMAND} -E env PATH=$<TARGET_FILE_DIR:iron>:$ENV{PATH}
            sh -c "rm -rf /tmp/iron_test_init && mkdir -p /tmp/iron_test_init && cd /tmp/iron_test_init && $<TARGET_FILE:iron> init && test -f iron.toml && test -f src/main.iron && test -f .gitignore && grep -q '\\[package\\]' iron.toml && grep -q 'println' src/main.iron"
    )

    add_test(
        NAME test_iron_init_lib
        COMMAND ${CMAKE_COMMAND} -E env PATH=$<TARGET_FILE_DIR:iron>:$ENV{PATH}
            sh -c "rm -rf /tmp/iron_test_init_lib && mkdir -p /tmp/iron_test_init_lib && cd /tmp/iron_test_init_lib && $<TARGET_FILE:iron> init --lib && test -f iron.toml && test -f src/lib.iron && grep -q 'type = \"lib\"' iron.toml"
    )
endif()

# iron package-mode tests
if(NOT WIN32)
    # iron build in package mode
    add_test(
        NAME test_iron_build_package
        COMMAND sh -c "rm -rf /tmp/iron_test_build && mkdir -p /tmp/iron_test_build && cd /tmp/iron_test_build && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> build && test -f target/iron_test_build"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_build_package PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # iron run in package mode
    add_test(
        NAME test_iron_run_package
        COMMAND sh -c "rm -rf /tmp/iron_test_run && mkdir -p /tmp/iron_test_run && cd /tmp/iron_test_run && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> run"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_run_package PROPERTIES
        PASS_REGULAR_EXPRESSION "Hello, Iron!"
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # iron check in package mode
    add_test(
        NAME test_iron_check_package
        COMMAND sh -c "rm -rf /tmp/iron_test_check && mkdir -p /tmp/iron_test_check && cd /tmp/iron_test_check && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> check"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_check_package PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # Regression: iron build still works for projects with no dependencies
    add_test(
        NAME test_iron_build_no_deps_still_works
        COMMAND sh -c "set -e && rm -rf /tmp/iron_test_nodeps && mkdir -p /tmp/iron_test_nodeps && cd /tmp/iron_test_nodeps && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> build && test -f target/iron_test_nodeps && ./target/iron_test_nodeps | grep -q Hello"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_build_no_deps_still_works PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # Test: iron build creates NO iron.lock when project has no deps
    add_test(
        NAME test_iron_dep_lockfile
        COMMAND sh -c "set -e && rm -rf /tmp/iron_test_dep && mkdir -p /tmp/iron_test_dep/src && printf '[package]\nname = \"test-dep-project\"\nversion = \"0.1.0\"\n\n[dependencies]\n' > /tmp/iron_test_dep/iron.toml && printf 'func main() {\n    println(\"hello deps\")\n}\n' > /tmp/iron_test_dep/src/main.iron && cd /tmp/iron_test_dep && $<TARGET_FILE:iron> build && test -f target/test-dep-project && test ! -f iron.lock"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_dep_lockfile PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )
endif()

# ── Benchmark smoke test ───────────────────────────────────────────────────
add_test(NAME benchmark_smoke
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmarks/run_benchmarks.sh
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(benchmark_smoke PROPERTIES
    TIMEOUT 300
    LABELS "benchmark")

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
