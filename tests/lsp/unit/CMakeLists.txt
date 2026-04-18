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

    # ── Phase 2 Plan 03 (lifecycle + capabilities + cancel + dyn-register) ──
    # Every Plan 03 Unity test links the full server/transport stack so the
    # end-to-end dispatcher path can be exercised against a sink-backed
    # writer. The yyjson vendor TU is compiled per-executable with
    # -Wno-error -w (mirrors the ironls target).

    set(_LSP_PLAN03_SERVER_SRC
        ${CMAKE_SOURCE_DIR}/src/lsp/server/dispatch.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/lifecycle.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/capabilities.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/cancel.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/dyn_register.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_lifecycle.c)
    set(_LSP_PLAN03_TRANSPORT_SRC
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/reader.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)

    # Task 01: lifecycle FSM (6 RUN_TESTs)
    add_executable(test_lsp_lifecycle_fsm test_lsp_lifecycle_fsm.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_lifecycle_fsm PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_lifecycle_fsm PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_lifecycle_fsm
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_lifecycle_fsm COMMAND test_lsp_lifecycle_fsm)
    set_tests_properties(test_lsp_lifecycle_fsm PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 02: capability matrix (derived from handler table)
    add_executable(test_lsp_initialize_capabilities test_lsp_initialize_capabilities.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_initialize_capabilities PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_initialize_capabilities PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_initialize_capabilities
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_initialize_capabilities
             COMMAND test_lsp_initialize_capabilities)
    set_tests_properties(test_lsp_initialize_capabilities PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 02: positionEncoding negotiation (4 canonical scenarios)
    add_executable(test_lsp_position_encoding_negotiation
        test_lsp_position_encoding_negotiation.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_position_encoding_negotiation PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_position_encoding_negotiation
        PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_position_encoding_negotiation
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_position_encoding_negotiation
             COMMAND test_lsp_position_encoding_negotiation)
    set_tests_properties(test_lsp_position_encoding_negotiation PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 02: dynamic registration (watched-files glob payload)
    add_executable(test_lsp_dynamic_registration test_lsp_dynamic_registration.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_dynamic_registration PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_dynamic_registration PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_dynamic_registration
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_dynamic_registration
             COMMAND test_lsp_dynamic_registration)
    set_tests_properties(test_lsp_dynamic_registration PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # The yyjson vendor TU must survive the repo-global -Werror strictness.
    # One set_source_files_properties entry covers every test above that
    # compiles yyjson.c directly (directory-scoped property).
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -w")
else()
    message(STATUS "Skipping tests/lsp/unit: IRON_BUILD_LSP=OFF")
endif()
