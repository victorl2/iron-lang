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
)
target_include_directories(iron_compiler PUBLIC src src/vendor)

# ── Tests ──────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_arena tests/test_arena.c)
target_link_libraries(test_arena iron_compiler unity)
add_test(NAME test_arena COMMAND test_arena)
