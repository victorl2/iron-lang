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

    # Plan 03 server source list. Plan 04 extends dispatch.c with 5 new
    # document-sync handler-table rows; those reference ilsp_handle_did*
    # symbols which live in handlers_document.c, which in turn pulls in
    # the store/ subsystem. Tests that link the dispatcher therefore also
    # link the store/ TUs.
    set(_LSP_PLAN03_SERVER_SRC
        ${CMAKE_SOURCE_DIR}/src/lsp/server/dispatch.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/lifecycle.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/capabilities.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/cancel.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/dyn_register.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_lifecycle.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/notifications.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/workspace.c
        # Plan 05: handlers_document.c now starts + stops per-doc workers;
        # tests that link handlers_document.c must also link the worker
        # subsystem and the facade stubs.
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/mailbox.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/ast_worker.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/abort_handler.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c)
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

    # Task 03: $/cancelRequest end-to-end + thread-visibility smoke
    add_executable(test_lsp_cancel_request_e2e test_lsp_cancel_request_e2e.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_cancel_request_e2e PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_cancel_request_e2e PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_cancel_request_e2e
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_cancel_request_e2e
             COMMAND test_lsp_cancel_request_e2e)
    set_tests_properties(test_lsp_cancel_request_e2e PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # ── Phase 2 Plan 04 (document store + UTF + SHA-256 + watched files) ────
    # Shared source lists so each test executable re-uses the same compile
    # artifacts. Plan 04 extends Plan 03's server source set with the new
    # store/ subsystem + handlers_document.c.
    set(_LSP_PLAN04_STORE_SRC
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/workspace.c)

    # Task 01a: UTF math (8 RUN_TESTs across 6 scenarios)
    add_executable(test_lsp_utf_translation test_lsp_utf_translation.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c)
    target_include_directories(test_lsp_utf_translation PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_utf_translation iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_utf_translation COMMAND test_lsp_utf_translation)
    set_tests_properties(test_lsp_utf_translation PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 01b: line_starts[] index (6 RUN_TESTs)
    # stb_ds_impl.c is provided by iron_runtime -- link, don't compile twice.
    add_executable(test_lsp_line_starts_index test_lsp_line_starts_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c)
    target_include_directories(test_lsp_line_starts_index PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_line_starts_index iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_line_starts_index COMMAND test_lsp_line_starts_index)
    set_tests_properties(test_lsp_line_starts_index PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 01c: SHA-256 known-vector test (4 RUN_TESTs)
    add_executable(test_lsp_sync_integrity_hash test_lsp_sync_integrity_hash.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c)
    target_include_directories(test_lsp_sync_integrity_hash PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_sync_integrity_hash iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_sync_integrity_hash COMMAND test_lsp_sync_integrity_hash)
    set_tests_properties(test_lsp_sync_integrity_hash PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 02a: document sync corpus (11 RUN_TESTs)
    add_executable(test_lsp_document_sync_corpus test_lsp_document_sync_corpus.c
        ${_LSP_PLAN04_STORE_SRC})
    target_include_directories(test_lsp_document_sync_corpus PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_document_sync_corpus
        iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_document_sync_corpus
             COMMAND test_lsp_document_sync_corpus)
    set_tests_properties(test_lsp_document_sync_corpus PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 02b: watched files (4 RUN_TESTs; dispatcher-driven for the 4th)
    add_executable(test_lsp_watched_files test_lsp_watched_files.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_watched_files PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_watched_files PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_watched_files
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_watched_files COMMAND test_lsp_watched_files)
    set_tests_properties(test_lsp_watched_files PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # ── Phase 2 Plan 05 (mailbox + ASTWorker + SIGABRT boundary) ──────────
    # Source list for the Plan 05 tests that exercise the workers/ and
    # obs/ subsystems. test_lsp_diagnostic_debounce defines its own stubs
    # for ilsp_facade_compile + ilsp_facade_pull_diagnostic so it does
    # NOT link src/lsp/facade/compile.c; test_lsp_sigabrt_boundary does
    # link notifications.c (which is needed by ast_worker.c's strike
    # helper).

    # Task 01 Unity 1: coalescing mailbox (mailbox.c only, no threads).
    add_executable(test_lsp_mailbox_coalesce test_lsp_mailbox_coalesce.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/mailbox.c)
    target_include_directories(test_lsp_mailbox_coalesce PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_link_libraries(test_lsp_mailbox_coalesce
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_mailbox_coalesce COMMAND test_lsp_mailbox_coalesce)
    set_tests_properties(test_lsp_mailbox_coalesce PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 01 Unity 2: debounce timing. Links ast_worker + mailbox; does
    # NOT link facade/compile.c -- the test provides its own facade stubs
    # as link-time overrides. notifications.c IS linked (the worker's
    # abort branch calls ilsp_send_window_showmessage, which would emit
    # to server->writer, but in this test server.writer is NULL so the
    # helper is a no-op). store/document.c is linked because mailbox + a
    # few test helpers touch IronLsp_Document fields.
    add_executable(test_lsp_diagnostic_debounce test_lsp_diagnostic_debounce.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/mailbox.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/ast_worker.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/abort_handler.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/notifications.c
        ${_LSP_PLAN04_STORE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_lsp_diagnostic_debounce PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_diagnostic_debounce PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_diagnostic_debounce
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_diagnostic_debounce
             COMMAND test_lsp_diagnostic_debounce)
    set_tests_properties(test_lsp_diagnostic_debounce PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant"
        TIMEOUT 30)

    # Task 01 Unity 3: SIGABRT boundary + window/showMessage emission.
    # Links everything needed to drive a real IronLsp_Writer backed by
    # open_memstream and inspect the emitted window/showMessage JSON.
    # Links facade/compile.c (stub or real -- symbol needs to exist).
    add_executable(test_lsp_sigabrt_boundary test_lsp_sigabrt_boundary.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/mailbox.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/ast_worker.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/abort_handler.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/notifications.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        ${_LSP_PLAN04_STORE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_lsp_sigabrt_boundary PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_sigabrt_boundary PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_sigabrt_boundary
        iron_compiler iron_runtime unity pthread)
    add_test(NAME test_lsp_sigabrt_boundary COMMAND test_lsp_sigabrt_boundary)
    set_tests_properties(test_lsp_sigabrt_boundary PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant"
        TIMEOUT 30)

    # Task 02 Unity: pull diagnostics via ilsp_facade_pull_diagnostic
    # against a real IronLsp_Writer backed by open_memstream.
    add_executable(test_lsp_pull_diagnostics test_lsp_pull_diagnostics.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        ${_LSP_PLAN04_STORE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_lsp_pull_diagnostics PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor)
    target_compile_definitions(test_lsp_pull_diagnostics PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_pull_diagnostics
        iron_compiler iron_runtime iron_stdlib unity pthread)
    add_test(NAME test_lsp_pull_diagnostics COMMAND test_lsp_pull_diagnostics)
    set_tests_properties(test_lsp_pull_diagnostics PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant"
        TIMEOUT 30)

    # The yyjson vendor TU must survive the repo-global -Werror strictness.
    # One set_source_files_properties entry covers every test above that
    # compiles yyjson.c directly (directory-scoped property).
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -w")
else()
    message(STATUS "Skipping tests/lsp/unit: IRON_BUILD_LSP=OFF")
endif()
