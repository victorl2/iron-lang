# LSP unit tests -- Phase 2 M1 LSP Core.
# Per-test executables following the tests/unit/ pattern. Subsequent plans
# (02-02 .. 02-06) add test_lsp_frame, test_lsp_json, test_lsp_utf, etc.
# The phase-m1-invariant label marks tests that must never regress while
# the Phase 2 milestone is open.

if(IRON_BUILD_LSP)
    add_executable(test_lsp_build_smoke test_lsp_build_smoke.c)
    target_include_directories(test_lsp_build_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_build_smoke iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_build_smoke COMMAND test_lsp_build_smoke)
    set_tests_properties(test_lsp_build_smoke PROPERTIES
        LABELS "unit;phase-m1-invariant")

    # Phase 2 Plan 02 Task 01: Content-Length frame parser.
    add_executable(test_lsp_frame test_lsp_frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c)
    target_include_directories(test_lsp_frame PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_frame iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_frame COMMAND test_lsp_frame)
    set_tests_properties(test_lsp_frame PROPERTIES
        LABELS "unit;phase-m1-invariant")

    # Phase 2 Plan 02 Task 02: yyjson-to-Iron_Arena wrapper.
    # Compiles yyjson.c into this test executable with warnings silenced,
    # matching the ironls target's treatment of the vendored TU.
    add_executable(test_lsp_json test_lsp_json.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_lsp_json PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_json iron_compiler iron_runtime unity)
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -w")
    add_test(NAME test_lsp_json COMMAND test_lsp_json)
    set_tests_properties(test_lsp_json PROPERTIES
        LABELS "unit;phase-m1-invariant")

    # Phase 2 Plan 02 Task 03: Writer queue + reader thread.
    # Requires pthread for the writer/reader thread primitives and _GNU_SOURCE
    # for fmemopen / open_memstream / memmem on glibc.
    add_executable(test_lsp_writer_queue test_lsp_writer_queue.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/reader.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c)
    target_include_directories(test_lsp_writer_queue PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_writer_queue PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_writer_queue
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_writer_queue COMMAND test_lsp_writer_queue)
    set_tests_properties(test_lsp_writer_queue PROPERTIES
        LABELS "unit;phase-m1-invariant")
else()
    message(STATUS "Skipping tests/lsp/unit: IRON_BUILD_LSP=OFF")
endif()
