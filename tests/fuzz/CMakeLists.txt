# tests/fuzz/CMakeLists.txt — Phase 68 fuzz target build rules.
#
# This file is only added when -DIRON_ENABLE_FUZZING=ON is passed at
# configure time (see the root CMakeLists.txt guard). The clang-only
# check lives in the root file; by the time this subdir is added we
# are guaranteed to be running under clang.
#
# Three executables are defined here:
#   - fuzz_parser      (FUZZ-01, real body lands in Plan 03)
#   - fuzz_typecheck   (FUZZ-02, real body lands in Plan 04)
#   - fuzz_hir_to_lir  (FUZZ-03, real body lands in Plan 05)
# Plus a non-fuzz build-time helper:
#   - iron_seed_blobs  (copies tests/integration/*.iron into the parser
#                       corpus dir; real blob-serialization logic lands
#                       in Plan 02)
#
# All three fuzz executables link the existing iron_compiler +
# iron_runtime + iron_stdlib static libs, which are rebuilt with
# -fsanitize=fuzzer-no-link so SanitizerCoverage instrumentation reaches
# every compiler source file. See 68-RESEARCH.md Pitfall 1 for why the
# no-link variant is mandatory.

# Rebuild the existing compiler libs with coverage + ASan/UBSan instrumentation.
# PRIVATE compile options — these only apply when IRON_ENABLE_FUZZING=ON
# because this entire subdirectory is guarded in the root CMakeLists.txt.
target_compile_options(iron_compiler PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
    -g)
target_link_options(iron_compiler PUBLIC
    -fsanitize=address,undefined)

target_compile_options(iron_runtime PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
    -g)
target_link_options(iron_runtime PUBLIC
    -fsanitize=address,undefined)

target_compile_options(iron_stdlib PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
    -g)
target_link_options(iron_stdlib PUBLIC
    -fsanitize=address,undefined)

# Shared structure-aware generator + blob codec (skeleton in Plan 01;
# real logic in Plan 02).
add_library(iron_gen STATIC iron_gen.c)
target_include_directories(iron_gen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor)
target_compile_options(iron_gen PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
    -g)

# On macOS, Homebrew LLVM's libclang_rt.fuzzer_osx.a is built against
# Homebrew's libc++ (which ships __hash_memory and friends), but Apple's
# system linker prefers the macOS system libc++ which is older. Without
# this rpath override, linking any fuzz target fails with hundreds of
# "std::__1::__hash_memory ... not found" symbols. Linux CI runs under
# ubuntu-latest clang where this is a no-op, so the block is gated on
# APPLE. If CMAKE_C_COMPILER points at a non-Homebrew clang on macOS the
# path below is simply absent and the flag is harmless.
set(_IRON_FUZZ_APPLE_LIBCXX_PATH "")
if(APPLE)
    get_filename_component(_iron_clang_bin "${CMAKE_C_COMPILER}" REALPATH)
    get_filename_component(_iron_clang_dir "${_iron_clang_bin}" DIRECTORY)
    get_filename_component(_iron_llvm_root "${_iron_clang_dir}" DIRECTORY)
    if(EXISTS "${_iron_llvm_root}/lib/c++/libc++.dylib")
        set(_IRON_FUZZ_APPLE_LIBCXX_PATH "${_iron_llvm_root}/lib/c++")
        message(STATUS "Fuzz: using Homebrew libc++ at ${_IRON_FUZZ_APPLE_LIBCXX_PATH}")
    endif()
endif()

# Helper: define one fuzz target executable with the full flag set.
function(iron_add_fuzz_target NAME SRC)
    add_executable(${NAME} ${SRC})
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_options(${NAME} PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -g)
    target_link_options(${NAME} PRIVATE
        -fsanitize=fuzzer,address,undefined)
    if(_IRON_FUZZ_APPLE_LIBCXX_PATH)
        target_link_options(${NAME} PRIVATE
            "-L${_IRON_FUZZ_APPLE_LIBCXX_PATH}"
            "-Wl,-rpath,${_IRON_FUZZ_APPLE_LIBCXX_PATH}")
    endif()
    target_link_libraries(${NAME} PRIVATE
        iron_compiler iron_runtime iron_stdlib iron_gen)
    if(NOT WIN32)
        target_link_libraries(${NAME} PRIVATE m pthread)
    endif()
endfunction()

iron_add_fuzz_target(fuzz_parser      fuzz_parser.c)
iron_add_fuzz_target(fuzz_typecheck   fuzz_typecheck.c)
iron_add_fuzz_target(fuzz_hir_to_lir  fuzz_hir_to_lir.c)

# Build-time helper (NOT a fuzz target — no -fsanitize=fuzzer).
# Copies tests/integration/*.iron into corpus/parser/ and creates the
# two post-parser corpus directories (which stay empty until Plan 02
# fills them with binary blobs).
add_executable(iron_seed_blobs seed_blobs.c)
target_include_directories(iron_seed_blobs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor)
target_compile_options(iron_seed_blobs PRIVATE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
    -g)
target_link_options(iron_seed_blobs PRIVATE
    -fsanitize=address,undefined)
target_link_libraries(iron_seed_blobs PRIVATE
    iron_compiler iron_runtime iron_stdlib iron_gen)
if(NOT WIN32)
    target_link_libraries(iron_seed_blobs PRIVATE m pthread)
endif()

set(_FUZZ_INTEGRATION_DIR "${CMAKE_SOURCE_DIR}/tests/integration")
set(_FUZZ_CORPUS_DIR      "${CMAKE_CURRENT_BINARY_DIR}/corpus")

# POST_BUILD: create corpus dirs and invoke iron_seed_blobs to populate them.
# Uses cmake -E copy_directory for the parser corpus (not symlinks — Pitfall 6
# in 68-RESEARCH.md explains why symlinks break on Windows and across
# `git clone`). Marginal disk cost (~5 MB per build dir).
#
# ASAN_OPTIONS=detect_leaks=0 is load-bearing: iron_seed_blobs is built with
# -fsanitize=address,undefined and parses every .iron file in tests/integration
# through the full pipeline. The parser's documented batch-compiler tradeoff
# (see src/parser/parser.c:15-49 "FIX-03 / AUDIT-04 §1: SAFETY") deliberately
# leaks stb_ds backing buffers at process exit for 17 arrput-into-AST transfer
# sites — Iron_Parse_Pattern in match-statement bodies is one of them. Without
# this env override, LSan turns every seed-tool invocation on a Linux runner
# (where LSan is on by default) into a non-zero exit and blocks the fuzz build
# at the POST_BUILD step. Mirrors the runtime detect_leaks=0 already set in
# .github/workflows/fuzz.yml for the libFuzzer step.
add_custom_command(TARGET iron_seed_blobs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_FUZZ_CORPUS_DIR}/parser"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_FUZZ_CORPUS_DIR}/typecheck"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_FUZZ_CORPUS_DIR}/hir_to_lir"
    COMMAND ${CMAKE_COMMAND} -E env "ASAN_OPTIONS=detect_leaks=0"
            $<TARGET_FILE:iron_seed_blobs>
            "${_FUZZ_INTEGRATION_DIR}"
            "${_FUZZ_CORPUS_DIR}"
    COMMENT "Seeding fuzz corpora from tests/integration/*.iron")

# Intentionally NOT calling add_test() — fuzz targets are excluded from
# ctest so developers running `ctest --test-dir build -j4` do not
# accidentally trigger 10-minute fuzz runs. CONTEXT.md section "Fuzz
# build integration model" locks this decision.
