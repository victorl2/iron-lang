# tests/fuzz/lsp/CMakeLists.txt — Phase 7 Plan 07-05 (HARD-19).
#
# LSP-specific libFuzzer harnesses. Mirrors tests/fuzz/CMakeLists.txt but
# targets the LSP transport + server + store layers rather than the
# compiler pipeline.
#
# Four harnesses land here:
#   - fuzz_lsp_frame     (Plan 07-05 Task 1) Content-Length framer
#   - fuzz_lsp_json      (Plan 07-05 Task 1) yyjson + Iron_Arena binding
#   - fuzz_lsp_dispatch  (Plan 07-05 Task 2) method-table lookup + route
#   - fuzz_lsp_didChange (Plan 07-05 Task 2) document-sync state machine
#
# Every harness compiles the specific LSP source files it needs DIRECTLY
# into the fuzz executable (rather than linking ironls) so the coverage
# instrumentation reaches every line of the target code. iron_compiler +
# iron_runtime + iron_stdlib are still linked -- they already carry the
# fuzzer-no-link instrumentation applied by tests/fuzz/CMakeLists.txt,
# which is a PRIVATE option (propagates through STATIC linkage).
#
# Gating: this file is only added-subdirectory'd when -DIRON_ENABLE_FUZZING
# is ON (root CMakeLists.txt:265) AND the compiler is clang (enforced by
# the clang-only check at CMakeLists.txt:129-133). No secondary gate here.
#
# The LSP binary itself is NOT built here: these targets are library-like
# harnesses, not end-to-end ironls runs. The target production binary
# (ironls) continues to be built separately via -DIRON_BUILD_LSP=ON.
#
# NOT added to ctest (mirrors tests/fuzz/CMakeLists.txt:156-158). These
# executables are invoked from .github/workflows/fuzz.yml only.

# Paths into src/lsp — individual harnesses pull in only what they need.
set(_LSP_SRC "${CMAKE_SOURCE_DIR}/src/lsp")

# Shared include dirs for every LSP fuzz target.
function(iron_lsp_fuzz_common_includes tgt)
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_SOURCE_DIR}/tests/fuzz
        ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

# Shared compile + link flags for every LSP fuzz target. Mirrors
# tests/fuzz/CMakeLists.txt iron_add_fuzz_target (ll. 79-101).
function(iron_lsp_fuzz_common_flags tgt)
    target_compile_options(${tgt} PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -g)
    target_link_options(${tgt} PRIVATE
        -fsanitize=fuzzer,address,undefined)
    if(_IRON_FUZZ_APPLE_LIBCXX_PATH)
        target_link_options(${tgt} PRIVATE
            "-L${_IRON_FUZZ_APPLE_LIBCXX_PATH}"
            "-Wl,-rpath,${_IRON_FUZZ_APPLE_LIBCXX_PATH}")
    endif()
    if(NOT WIN32)
        target_link_libraries(${tgt} PRIVATE m pthread)
    endif()
endfunction()

# ── fuzz_lsp_frame ────────────────────────────────────────────────────
# Content-Length framer (src/lsp/transport/frame.c). Self-contained --
# depends only on the stdlib + the framer's own TU.
add_executable(fuzz_lsp_frame
    ${CMAKE_CURRENT_SOURCE_DIR}/frame/fuzz_target.c
    ${_LSP_SRC}/transport/frame.c)
iron_lsp_fuzz_common_includes(fuzz_lsp_frame)
iron_lsp_fuzz_common_flags(fuzz_lsp_frame)

# ── fuzz_lsp_json ─────────────────────────────────────────────────────
# yyjson + Iron_Arena binding (src/lsp/transport/json.c). Pulls in yyjson
# as a compiled TU so the arena allocator's realloc path is fully
# instrumented, plus the Iron_Arena TU + stb_ds impl from iron_runtime.
add_executable(fuzz_lsp_json
    ${CMAKE_CURRENT_SOURCE_DIR}/json/fuzz_target.c
    ${_LSP_SRC}/transport/json.c
    ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
iron_lsp_fuzz_common_includes(fuzz_lsp_json)
iron_lsp_fuzz_common_flags(fuzz_lsp_json)
target_link_libraries(fuzz_lsp_json PRIVATE iron_runtime)

# ── fuzz_lsp_dispatch ─────────────────────────────────────────────────
# Method dispatcher (src/lsp/server/dispatch.c). Links the real handler
# table + bsearch lookup + full dispatch.c so ilsp_handler_lookup is
# fuzzed against adversarial method names. The harness never calls
# ilsp_dispatch_route (which would need a fully-wired server); it only
# reaches the pure table-lookup path. Unused handler addresses are
# satisfied by the stubs TU below, which __builtin_trap() if ever
# invoked -- accidental escape from the documented harness surface
# will surface immediately in CI.
add_executable(fuzz_lsp_dispatch
    ${CMAKE_CURRENT_SOURCE_DIR}/dispatch/fuzz_target.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dispatch/handler_stubs.c
    ${_LSP_SRC}/server/dispatch.c
    ${_LSP_SRC}/server/lifecycle.c
    ${_LSP_SRC}/transport/json.c
    ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
iron_lsp_fuzz_common_includes(fuzz_lsp_dispatch)
iron_lsp_fuzz_common_flags(fuzz_lsp_dispatch)
target_link_libraries(fuzz_lsp_dispatch PRIVATE iron_runtime)

# ── fuzz_lsp_didChange ────────────────────────────────────────────────
# Document store state machine (src/lsp/store/document.c + line_index.c
# + utf.c + sha256.c). Exercises create -> incremental/full-replace×N ->
# destroy through adversarial (range, text, version) sequences; asserts
# the T-02-09 / T-02-13 must-not-mutate-on-reject invariant by snapshot
# diff after each apply call.
add_executable(fuzz_lsp_didChange
    ${CMAKE_CURRENT_SOURCE_DIR}/didChange/fuzz_target.c
    ${_LSP_SRC}/store/document.c
    ${_LSP_SRC}/store/line_index.c
    ${_LSP_SRC}/store/utf.c
    ${_LSP_SRC}/store/sha256.c)
iron_lsp_fuzz_common_includes(fuzz_lsp_didChange)
iron_lsp_fuzz_common_flags(fuzz_lsp_didChange)
target_link_libraries(fuzz_lsp_didChange PRIVATE iron_runtime)
