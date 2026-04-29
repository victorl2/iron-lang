# LSP unit tests -- Phase 2 M1 LSP Core.
# Per-test executables following the tests/unit/ pattern. Subsequent plans
# (02-02 .. 02-06) add test_lsp_frame, test_lsp_json, test_lsp_utf, etc.
# The phase-m1-invariant label marks tests that must never regress while
# the Phase 2 milestone is open.

if(IRON_BUILD_LSP)
    add_executable(test_lsp_build_smoke test_lsp_build_smoke.c)
    target_include_directories(test_lsp_build_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_lsp_build_smoke iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_build_smoke COMMAND test_lsp_build_smoke)
    set_tests_properties(test_lsp_build_smoke PROPERTIES
        LABELS "unit;phase-m1-invariant")

    # Phase 2 Plan 02 Task 01: Content-Length frame parser.
    add_executable(test_lsp_frame test_lsp_frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c)
    target_include_directories(test_lsp_frame PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_lsp_json iron_compiler iron_runtime unity)
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -w")
    add_test(NAME test_lsp_json COMMAND test_lsp_json)
    set_tests_properties(test_lsp_json PROPERTIES
        LABELS "unit;phase-m1-invariant")

    # Phase 2 Plan 02 Task 03: Writer queue + reader thread.
    # Requires pthread for the writer/reader thread primitives and _GNU_SOURCE
    # for fmemopen / open_memstream / memmem on glibc. Plan 06: reader +
    # writer use ilsp_log for EPIPE/EOF events, so obs/log.c is now part
    # of the link set for any TU that pulls in transport sources.
    add_executable(test_lsp_writer_queue test_lsp_writer_queue.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/reader.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c)
    target_include_directories(test_lsp_writer_queue PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_writer_queue PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_writer_queue
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        # Phase 7 Plan 07-01 (HARD-14, D-02): dispatch.c calls
        # ilsp_crash_ring_push/pop around handler invocation. Every test
        # linking dispatch.c must resolve these symbols. Rule 3: blocking
        # link-graph fix inherited from Phase 7.
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/crash_dump.c
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
        # Phase 3 Plan 02: handlers_document.c invalidates workspace_index
        # entries on didChangeWatchedFiles; every test linking
        # handlers_document.c must link workspace_index.c too.
        ${CMAKE_SOURCE_DIR}/src/lsp/store/workspace_index.c
        # Phase 3 Plan 02/03 + Phase 4 Plans 04-02/05: completion buckets
        # and organize_imports query stdlib_cache + dep_map. These were
        # previously only linked through ironls; tests that link
        # facade/edit/**.c must also link them.
        ${CMAKE_SOURCE_DIR}/src/lsp/store/stdlib_cache.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/dep_map.c
        # dep_map.c consumes src/pkg/resolver + src/cli/toml — pull them in.
        ${CMAKE_SOURCE_DIR}/src/pkg/resolver.c
        ${CMAKE_SOURCE_DIR}/src/pkg/fetcher.c
        ${CMAKE_SOURCE_DIR}/src/pkg/lockfile.c
        ${CMAKE_SOURCE_DIR}/src/cli/toml.c
        # Plan 05: handlers_document.c now starts + stops per-doc workers;
        # tests that link handlers_document.c must also link the worker
        # subsystem and the facade stubs.
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/mailbox.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/ast_worker.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/abort_handler.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        # Phase 3 Plan 03: handlers_nav.c registers 5 NAV rows in
        # dispatch.c; every test linking dispatch.c must also link
        # handlers_nav.c plus the facade/nav endpoint TUs it delegates to.
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_nav.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/symbol_id.c
        # Phase 10 Plan 10-01: visibility predicate consumed by every
        # nav-facade TU (definition/type_definition/references/hover/
        # implementation/workspace_symbol/rename/apply). Linked here so
        # m1-invariant tests using _LSP_PLAN03_SERVER_SRC resolve
        # ilsp_vis_can_see + ilsp_vis_is_public symbols.
        # Phase 11 Plan 11-02 (Rule 3 link-graph fix): retroactive
        # registration; Plan 10-01/11-01 shipped the source file but only
        # registered it in _LSP_PHASE3_NAV_FACADE_SRC, leaving m1 binaries
        # with broken links to implementation.c's patch + visibility refs.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/visibility.c
        # Phase 11 Plan 11-01: patch_lookup helpers consumed by
        # implementation.c (PATCH-01) + type_hierarchy.c (PATCH-02) +
        # complete/buckets.c (PATCH-03) + hover.c/references.c (Plan 11-03).
        # Same Rule 3 link-graph fix as visibility.c above.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/patch_lookup.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/fuzzy.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/definition.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/declaration.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/type_definition.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/document_symbol.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/workspace_symbol.c
        # Phase 3 Plan 04: references_index + references + hover +
        # signature_help. Tests linking dispatch.c (which now has 3
        # new NAV rows) must resolve these symbols.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/references_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/references.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/hover.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/signature_help.c
        # Phase 3 Plan 05: iface_workspace + implementation + type_hierarchy.
        # Tests linking dispatch.c (now with textDocument/implementation,
        # textDocument/prepareTypeHierarchy, typeHierarchy/subtypes,
        # typeHierarchy/supertypes rows) must resolve the facade entries
        # + the aggregator's ilsp_iface_ws_* symbols used by workspace_index.c.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/iface_workspace.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/implementation.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/type_hierarchy.c
        # Phase 3 Plan 06: workspace/diagnostic facade + handler + refresh.
        # dispatch.c now references ilsp_handle_workspace_diagnostic and
        # handlers_document.c calls ilsp_send_workspace_diagnostic_refresh
        # + ilsp_ws_diag_cache_evict_path.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/workspace_diagnostic.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_workspace_diag.c
        # Phase 4 (M3 Editing Assistance): dispatch.c adds 9 EDIT handler
        # rows (textDocument/completion, completionItem/resolve,
        # textDocument/codeAction, codeAction/resolve,
        # textDocument/prepareRename, textDocument/rename,
        # textDocument/documentHighlight, textDocument/foldingRange,
        # textDocument/selectionRange). handlers_edit.c hosts all 9
        # handlers; they delegate to facade/edit/ TUs. Every test linking
        # dispatch.c must resolve these symbols.
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_edit.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/complete.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/buckets.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/context_classify.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/resolve.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/auto_import.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/snippet.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/codeaction.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/registry.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/resolve.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/organize_imports.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_undefined_var.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_unused_import.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_missing_return.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/prepare.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/collision.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/apply.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/highlight.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/folding_range.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/selection_range.c
        # Phase 5 (M4 Formatting): dispatch.c adds 3 fmt handler rows
        # (textDocument/formatting, textDocument/rangeFormatting,
        # textDocument/onTypeFormatting). handlers_fmt.c hosts all 3
        # and delegates to facade/fmt/ TUs. Every test linking
        # dispatch.c must resolve these symbols (Plan 05-05 deviation
        # Rule 3: blocking link-graph fix inherited from Plans 05-02..05-04).
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_fmt.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/range_format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/on_type_format.c
        # Phase 7 Plan 07-07 (HARD-22, D-10): handlers_lifecycle.c calls
        # ilsp_server_version() for the initialize response server-info block.
        # Every test linking dispatch.c must resolve this symbol.
        # Rule 3 (08-02 Task 1): blocking link-graph fix inherited from Phase 7.
        ${CMAKE_SOURCE_DIR}/src/lsp/cli/version.c)
    set(_LSP_PLAN03_TRANSPORT_SRC
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/reader.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        # Plan 06: reader+writer use ilsp_log for EPIPE/EOF events.
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)

    # Task 01: lifecycle FSM (6 RUN_TESTs)
    add_executable(test_lsp_lifecycle_fsm test_lsp_lifecycle_fsm.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_lifecycle_fsm PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_lifecycle_fsm PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_lifecycle_fsm
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_lsp_lifecycle_fsm COMMAND test_lsp_lifecycle_fsm)
    set_tests_properties(test_lsp_lifecycle_fsm PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 02: capability matrix (derived from handler table)
    add_executable(test_lsp_initialize_capabilities test_lsp_initialize_capabilities.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_lsp_initialize_capabilities PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_initialize_capabilities PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_initialize_capabilities
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_position_encoding_negotiation
        PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_position_encoding_negotiation
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_dynamic_registration PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_dynamic_registration
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_cancel_request_e2e PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_cancel_request_e2e
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_lsp_line_starts_index iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_line_starts_index COMMAND test_lsp_line_starts_index)
    set_tests_properties(test_lsp_line_starts_index PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 01c: SHA-256 known-vector test (4 RUN_TESTs)
    add_executable(test_lsp_sync_integrity_hash test_lsp_sync_integrity_hash.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c)
    target_include_directories(test_lsp_sync_integrity_hash PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_lsp_sync_integrity_hash iron_compiler iron_runtime unity)
    add_test(NAME test_lsp_sync_integrity_hash COMMAND test_lsp_sync_integrity_hash)
    set_tests_properties(test_lsp_sync_integrity_hash PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant")

    # Task 02a: document sync corpus (11 RUN_TESTs)
    add_executable(test_lsp_document_sync_corpus test_lsp_document_sync_corpus.c
        ${_LSP_PLAN04_STORE_SRC})
    target_include_directories(test_lsp_document_sync_corpus PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_watched_files PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_watched_files
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_lsp_mailbox_coalesce
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c   # Plan 06: writer uses ilsp_log
        ${CMAKE_SOURCE_DIR}/src/lsp/server/notifications.c
        ${_LSP_PLAN04_STORE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_lsp_diagnostic_debounce PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_diagnostic_debounce PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_diagnostic_debounce
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c   # Plan 06: writer uses ilsp_log
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
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_sigabrt_boundary PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_sigabrt_boundary
        iron_compiler iron_runtime iron_stdlib unity pthread m)
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
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c   # Plan 06: writer uses ilsp_log
        ${_LSP_PLAN04_STORE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_lsp_pull_diagnostics PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_pull_diagnostics PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_pull_diagnostics
        iron_compiler iron_runtime iron_stdlib unity pthread)
    add_test(NAME test_lsp_pull_diagnostics COMMAND test_lsp_pull_diagnostics)
    set_tests_properties(test_lsp_pull_diagnostics PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant"
        TIMEOUT 30)

    # ── Phase 2 Plan 06 (observability + EPIPE/EOF + smoke wiring) ────────
    # Task 01: XDG log resolution + JSON-line format + level filter +
    # thread-safe writes (6 RUN_TESTs).
    add_executable(test_lsp_log_path_xdg test_lsp_log_path_xdg.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c)
    target_include_directories(test_lsp_log_path_xdg PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_log_path_xdg PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_log_path_xdg
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_lsp_log_path_xdg COMMAND test_lsp_log_path_xdg)
    set_tests_properties(test_lsp_log_path_xdg PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant"
        TIMEOUT 30)

    # Task 02: SIGPIPE / EOF detection in writer + reader. Uses real
    # pipe(2) FDs wrapped in FILE* via fdopen so the production
    # writer_write_one / reader_thread_main paths are exercised.
    add_executable(test_lsp_sigpipe_shutdown test_lsp_sigpipe_shutdown.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/reader.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c)
    target_include_directories(test_lsp_sigpipe_shutdown PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_lsp_sigpipe_shutdown PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lsp_sigpipe_shutdown
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_lsp_sigpipe_shutdown COMMAND test_lsp_sigpipe_shutdown)
    set_tests_properties(test_lsp_sigpipe_shutdown PROPERTIES
        LABELS "unit;lsp-unit;phase-m1-invariant"
        TIMEOUT 30)

    # The yyjson vendor TU must survive the repo-global -Werror strictness.
    # One set_source_files_properties entry covers every test above that
    # compiles yyjson.c directly (directory-scoped property).
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -w")

    # ── Phase 3 Plan 01 Wave 0: Unity test stubs ─────────────────────────
    # Each stub file calls TEST_IGNORE_MESSAGE so Unity exits 0 and ctest
    # counts the test as PASS. Plans 01..06 swap TEST_IGNORE_MESSAGE for
    # real assertions one test at a time; when a stub flips to real, the
    # target's source list is extended below to pull in the TUs under test.
    # Label "unit;phase-m2-invariant" marks these as the Phase 3 gate.
    #
    # Plan 06 final flip: test_workspace_diagnostic now has a real target
    # further below; all prior stubs have likewise been flipped.
    # _PHASE3_WAVE0_STUBS is intentionally empty at Phase-3 close.
    set(_PHASE3_WAVE0_STUBS
        # Plan 04 flipped: test_nav_references, test_hover_formatter,
        # test_signature_help now have real targets further below.
        # Plan 05 flipped: test_iface_workspace + test_type_hierarchy.
        # Plan 06 flipped: test_workspace_diagnostic.
        )
    foreach(_stub IN LISTS _PHASE3_WAVE0_STUBS)
        add_executable(${_stub} ${_stub}.c)
        target_include_directories(${_stub} PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/vendor)
        target_link_libraries(${_stub} unity pthread)
        add_test(NAME ${_stub} COMMAND ${_stub})
        set_tests_properties(${_stub} PROPERTIES
            LABELS "unit;phase-m2-invariant"
            TIMEOUT 30)
    endforeach()

    # ── Phase 3 Plan 01 Task 02 (NAV-14): lexer doc-comment tests ────────
    # test_lexer_doc_comment exercises the `///` tokenization + parser
    # run-aggregation end-to-end; it links the lexer + parser surface from
    # iron_compiler. iron_compiler also brings iron_runtime (stb_ds_impl).
    add_executable(test_lexer_doc_comment test_lexer_doc_comment.c)
    target_include_directories(test_lexer_doc_comment PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_lexer_doc_comment
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_lexer_doc_comment COMMAND test_lexer_doc_comment)
    set_tests_properties(test_lexer_doc_comment PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 30)

    # test_lexer_doc_comment_parity asserts that stripping `///` lines
    # produces a byte-equivalent Iron_DiagList to the unstripped source.
    add_executable(test_lexer_doc_comment_parity test_lexer_doc_comment_parity.c)
    target_include_directories(test_lexer_doc_comment_parity PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_lexer_doc_comment_parity
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_lexer_doc_comment_parity
             COMMAND test_lexer_doc_comment_parity)
    set_tests_properties(test_lexer_doc_comment_parity PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 30)

    # ── Phase 3 Plan 01 Task 03 (NAV-15): sealed-AST contract test ───────
    # Verifies analyzer sets program->sealed; macro is no-op on unsealed;
    # fork()+SIGABRT proof that IRON_AST_ASSERT_UNSEALED actually aborts
    # under !NDEBUG. Links iron_stdlib because iron_analyze_buffer ends up
    # pulling stdlib-dependent passes through iron_runtime.
    add_executable(test_ast_sealed test_ast_sealed.c)
    target_include_directories(test_ast_sealed PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_ast_sealed
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_ast_sealed COMMAND test_ast_sealed)
    set_tests_properties(test_ast_sealed PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 30)

    # ── Phase 3 Plan 01 Task 04 (NAV-16): symbol_id + node_at ────────────
    # test_symbol_id exercises the (canonical_path, name_path, kind) +
    # precomputed FNV-1a 64-bit hash, including determinism across a
    # re-analyze into a fresh arena.
    add_executable(test_symbol_id test_symbol_id.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/symbol_id.c)
    target_include_directories(test_symbol_id PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_symbol_id
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_symbol_id COMMAND test_symbol_id)
    set_tests_properties(test_symbol_id PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 30)

    # test_node_at resolves LSP Position to the innermost Iron_Node
    # whose Iron_Span covers the cursor. Links the document store for
    # line_idx + utf conversion plus the nav helper itself.
    add_executable(test_node_at test_node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c)
    target_include_directories(test_node_at PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_node_at
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_node_at COMMAND test_node_at)
    set_tests_properties(test_node_at PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 30)

    # ── Phase 9 Plan 01 (AST-02 / AST-03): v3 walker + corpus regression ─
    # test_v3_node_at: D-12 cases 1-3 fixture-driven verification that the
    # NAV-16 walker descends into init / named-init / patch-method bodies.
    # Wave 0 ships TEST_IGNORE'd; Task 2 flips to real assertions.
    add_executable(test_v3_node_at test_v3_node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c)
    target_include_directories(test_v3_node_at PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_node_at PRIVATE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_node_at
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_node_at COMMAND test_v3_node_at)
    set_tests_properties(test_v3_node_at PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # test_v3_symbol_id_corpus: D-04/D-05 zero-churn + zero-collision drift
    # guard. Wave 0 ships TEST_IGNORE'd plus a REGEN_SYMBOL_ID_BASELINE=1
    # one-shot regen path; Task 2 flips assertions live.
    add_executable(test_v3_symbol_id_corpus test_v3_symbol_id_corpus.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/symbol_id.c)
    target_include_directories(test_v3_symbol_id_corpus PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_symbol_id_corpus PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_symbol_id_corpus
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_symbol_id_corpus COMMAND test_v3_symbol_id_corpus)
    set_tests_properties(test_v3_symbol_id_corpus PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 120)

    # ── Phase 10 Plan 10-01 (VIS-01..04, D-02 + D-12) ────────────────────
    # Visibility predicate (D-02) + symbol identity drift-guard (D-12).
    # Both compile in the production visibility.c + nav_common.c so the
    # predicate is exercised against real bytes. The drift-guard also
    # links symbol_id.c + iron_compiler so ilsp_symbol_id_derive resolves.
    add_executable(test_v3_visibility_predicate test_v3_visibility_predicate.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/visibility.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_v3_visibility_predicate PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_v3_visibility_predicate
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_v3_visibility_predicate COMMAND test_v3_visibility_predicate)
    set_tests_properties(test_v3_visibility_predicate PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_v3_visibility_symbol_identity_drift
        test_v3_visibility_symbol_identity_drift.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/symbol_id.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/visibility.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_v3_visibility_symbol_identity_drift PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_v3_visibility_symbol_identity_drift
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_visibility_symbol_identity_drift
        COMMAND test_v3_visibility_symbol_identity_drift)
    set_tests_properties(test_v3_visibility_symbol_identity_drift PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 3 Plan 02 Task 01 (NAV-01): workspace_index ────────────────
    # Per-file arena-scoped IndexEntry map with warm-seed, lazy analyze,
    # LRU cap, and path + dep invalidation.  Test drives the store API
    # directly against a /tmp fixture workspace.
    # ── Phase 3 Plan 02 Task 02 (NAV-01, D-14): stdlib_cache ─────────────
    # Process-lifetime pre-parsed cache for src/stdlib/*.iron +
    # src/runtime/*.iron, pthread_once guarded. Links the iron_import_detect
    # module via iron_compiler (library-linked; no subprocess).
    add_executable(test_stdlib_cache test_stdlib_cache.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/stdlib_cache.c)
    target_include_directories(test_stdlib_cache PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_stdlib_cache PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(test_stdlib_cache
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_stdlib_cache COMMAND test_stdlib_cache)
    set_tests_properties(test_stdlib_cache PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 3 Plan 02 Task 03 (NAV-01): dep_map ────────────────────────
    # iron.toml-resolved dep symbol map via library-linked resolver.c.
    # Test uses ilsp_dep_map_inject_for_test to avoid the GitHub-touching
    # fetcher path while still exercising lookup/invalidate/path-escape.
    add_executable(test_dep_map test_dep_map.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/dep_map.c
        ${CMAKE_SOURCE_DIR}/src/pkg/resolver.c
        ${CMAKE_SOURCE_DIR}/src/pkg/fetcher.c
        ${CMAKE_SOURCE_DIR}/src/pkg/lockfile.c
        ${CMAKE_SOURCE_DIR}/src/cli/toml.c)
    target_include_directories(test_dep_map PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_dep_map PRIVATE _GNU_SOURCE)
    target_link_libraries(test_dep_map
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_dep_map COMMAND test_dep_map)
    set_tests_properties(test_dep_map PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_workspace_index test_workspace_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/workspace_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        # Plan 04 Task 01: workspace_index.c now calls into
        # references_index via drop/populate; test link set must
        # pull in the new TU + its transitive symbol_id/nav_common deps.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/references_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/symbol_id.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        # Plan 05 Task 01: workspace_index.c now also calls into
        # iface_workspace for drop/populate; link the aggregator TU.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/iface_workspace.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        # Phase 5 Plan 05-02: workspace_index.c now calls iron_toml_parse
        # via ilsp_workspace_fmt_opts_load (fmt_opts cache for iron.toml
        # [fmt]). Link cli/toml.c for the TOML parser and dependent
        # package-resolution sources.
        ${CMAKE_SOURCE_DIR}/src/cli/toml.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_workspace_index PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_workspace_index PRIVATE _GNU_SOURCE)
    target_link_libraries(test_workspace_index
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_workspace_index COMMAND test_workspace_index)
    set_tests_properties(test_workspace_index PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 3 Plan 03 Task 01 (D-11): fuzzy matcher ───────────────────
    # Pure-C17 scorer; golden corpus unit-tests against 100+ (query,
    # candidate) rows. No facade or store dependency.
    add_executable(test_fuzzy_matcher test_fuzzy_matcher.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/fuzzy.c)
    target_include_directories(test_fuzzy_matcher PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_fuzzy_matcher
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_fuzzy_matcher COMMAND test_fuzzy_matcher)
    set_tests_properties(test_fuzzy_matcher PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 30)

    # ── Phase 3 Plan 03 Task 02/03: nav facade unit tests ──────────────
    # Shared source list for the three nav-facade Unity targets
    # (definition, document_symbol, workspace_symbol). All three drive
    # ilsp_facade_nav_* in-process; they need the full nav facade +
    # store stack.
    set(_LSP_PHASE3_NAV_FACADE_SRC
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/symbol_id.c
        # Phase 10 Plan 10-01: visibility predicate consumed by every
        # nav-facade test that exercises ilsp_vis_can_see in 10-02.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/visibility.c
        # Phase 11 Plan 11-01: patch_lookup helpers consumed by the
        # implementation/type_hierarchy/hover/references/completion
        # surfaces. Routed through the shared nav-facade source list
        # so every nav unit test links the patch helpers.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/patch_lookup.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/fuzzy.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/definition.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/declaration.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/type_definition.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/document_symbol.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/workspace_symbol.c
        # Phase 3 Plan 04: references + hover + signatureHelp.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/references_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/references.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/hover.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/signature_help.c
        # Phase 3 Plan 05: iface_workspace + implementation + type_hierarchy.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/iface_workspace.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/implementation.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/type_hierarchy.c
        # Phase 3 Plan 06: workspace/diagnostic facade + handler.
        # handlers_workspace_diag.c calls ilsp_cancel_register so cancel.c
        # is required in the link set.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/workspace_diagnostic.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_workspace_diag.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/cancel.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/workspace.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/workspace_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/stdlib_cache.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/dep_map.c
        ${CMAKE_SOURCE_DIR}/src/pkg/resolver.c
        ${CMAKE_SOURCE_DIR}/src/pkg/fetcher.c
        ${CMAKE_SOURCE_DIR}/src/pkg/lockfile.c
        ${CMAKE_SOURCE_DIR}/src/cli/toml.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)

    add_executable(test_nav_definition test_nav_definition.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_nav_definition PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_nav_definition PRIVATE _GNU_SOURCE)
    target_link_libraries(test_nav_definition
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_nav_definition COMMAND test_nav_definition)
    set_tests_properties(test_nav_definition PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_document_symbol test_document_symbol.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_document_symbol PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_document_symbol PRIVATE _GNU_SOURCE)
    target_link_libraries(test_document_symbol
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_document_symbol COMMAND test_document_symbol)
    set_tests_properties(test_document_symbol PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_workspace_symbol test_workspace_symbol.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_workspace_symbol PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_workspace_symbol PRIVATE _GNU_SOURCE)
    target_link_libraries(test_workspace_symbol
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_workspace_symbol COMMAND test_workspace_symbol)
    set_tests_properties(test_workspace_symbol PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 3 Plan 04: references + hover + signatureHelp ─────────────
    # Each of these three tests drives the facade entry directly in
    # memory against a minimal fixture; they link the full nav facade
    # stack (incl. Plan 04's references_index / references / hover /
    # signature_help) so the behaviour-under-test is exercised end to
    # end without spawning a server process.
    add_executable(test_nav_references test_nav_references.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_nav_references PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_nav_references PRIVATE _GNU_SOURCE)
    target_link_libraries(test_nav_references
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_nav_references COMMAND test_nav_references)
    set_tests_properties(test_nav_references PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_hover_formatter test_hover_formatter.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_hover_formatter PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_hover_formatter PRIVATE _GNU_SOURCE)
    target_link_libraries(test_hover_formatter
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_hover_formatter COMMAND test_hover_formatter)
    set_tests_properties(test_hover_formatter PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_signature_help test_signature_help.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_signature_help PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_signature_help PRIVATE _GNU_SOURCE)
    target_link_libraries(test_signature_help
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_signature_help COMMAND test_signature_help)
    set_tests_properties(test_signature_help PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 3 Plan 05: iface_workspace + type_hierarchy ───────────────
    # iface_workspace aggregates per-compile Iron_IfaceRegistry across
    # files; type_hierarchy drives prepare + supertypes + subtypes per
    # D-08. Both link the full NAV facade stack via _LSP_PHASE3_NAV_FACADE_SRC
    # (which now includes iface_workspace.c + implementation.c).
    add_executable(test_iface_workspace test_iface_workspace.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_iface_workspace PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_iface_workspace PRIVATE _GNU_SOURCE)
    target_link_libraries(test_iface_workspace
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_iface_workspace COMMAND test_iface_workspace)
    set_tests_properties(test_iface_workspace PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_type_hierarchy test_type_hierarchy.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_type_hierarchy PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_type_hierarchy PRIVATE _GNU_SOURCE)
    target_link_libraries(test_type_hierarchy
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_type_hierarchy COMMAND test_type_hierarchy)
    set_tests_properties(test_type_hierarchy PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 10 Plan 10-02 Wave 0 stubs (VIS-01..04) ────────────────────
    # 5 nav-facade tests (references / workspace_symbol / definition /
    # type_definition / implementation) + 1 rename test (rename). Each
    # links the full nav facade stack so the visibility predicate (Plan
    # 10-01) is exercised against real bytes via _LSP_PHASE3_NAV_FACADE_SRC.
    # The rename test additionally needs apply.c + notifications.c +
    # rename/prepare.c + rename/collision.c (rename apply orchestration).
    # Stubs land in Wave 0 with TEST_IGNORE_MESSAGE; flipped to real
    # assertions in Task 5.
    add_executable(test_v3_visibility_references test_v3_visibility_references.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_visibility_references PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_visibility_references PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_visibility_references
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_visibility_references COMMAND test_v3_visibility_references)
    set_tests_properties(test_v3_visibility_references PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_v3_visibility_workspace_symbol
        test_v3_visibility_workspace_symbol.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_visibility_workspace_symbol PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_visibility_workspace_symbol PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_visibility_workspace_symbol
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_visibility_workspace_symbol
        COMMAND test_v3_visibility_workspace_symbol)
    set_tests_properties(test_v3_visibility_workspace_symbol PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_v3_visibility_definition test_v3_visibility_definition.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_visibility_definition PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_visibility_definition PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_visibility_definition
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_visibility_definition COMMAND test_v3_visibility_definition)
    set_tests_properties(test_v3_visibility_definition PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_v3_visibility_type_definition
        test_v3_visibility_type_definition.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_visibility_type_definition PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_visibility_type_definition PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_visibility_type_definition
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_visibility_type_definition
        COMMAND test_v3_visibility_type_definition)
    set_tests_properties(test_v3_visibility_type_definition PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    add_executable(test_v3_visibility_implementation
        test_v3_visibility_implementation.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_visibility_implementation PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_visibility_implementation PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_visibility_implementation
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_visibility_implementation
        COMMAND test_v3_visibility_implementation)
    set_tests_properties(test_v3_visibility_implementation PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # Rename test (m3 invariant): needs the rename apply orchestration
    # stack on top of the nav facade -- apply.c + collision.c + prepare.c,
    # plus notifications.c for the VIS-04 window/showMessage emit.
    add_executable(test_v3_visibility_rename test_v3_visibility_rename.c
        ${_LSP_PHASE3_NAV_FACADE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/apply.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/collision.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/prepare.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/notifications.c)
    target_include_directories(test_v3_visibility_rename PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_visibility_rename PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_visibility_rename
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_visibility_rename COMMAND test_v3_visibility_rename)
    set_tests_properties(test_v3_visibility_rename PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 60)

    # ── Phase 11 Plan 11-01 (PATCH-01 + D-15 helpers) ───────────────────
    # Predicate test: minimal link set (patch_lookup.c + visibility.c +
    # nav_common.c + line_index/utf/yyjson) plus iron_compiler for the
    # registry build path (real fixture parse in test 8). Mirrors Phase
    # 10 test_v3_visibility_predicate shape (lines 678-694).
    add_executable(test_v3_patch_predicate test_v3_patch_predicate.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/patch_lookup.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/visibility.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_v3_patch_predicate PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_patch_predicate PRIVATE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_patch_predicate
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_v3_patch_predicate COMMAND test_v3_patch_predicate)
    set_tests_properties(test_v3_patch_predicate PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # PATCH-01 facade test: full nav facade link set (patch_lookup.c is
    # already on _LSP_PHASE3_NAV_FACADE_SRC) + IRON_SOURCE_TREE_ROOT
    # define so fixture_path() can locate tests/lsp/unit/v3_patch/*.iron.
    add_executable(test_v3_patch_implementation test_v3_patch_implementation.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_patch_implementation PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_patch_implementation PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_patch_implementation
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_patch_implementation COMMAND test_v3_patch_implementation)
    set_tests_properties(test_v3_patch_implementation PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 11 Plan 11-02 (PATCH-02 + PATCH-03) ───────────────────────
    # Task 5 upgraded the link sets from Wave 0 minimal to full nav-facade
    # stack so the real Unity assertions can drive the facade entry points
    # (subtypes via _LSP_PHASE3_NAV_FACADE_SRC; completion-buckets via the
    # nav-facade stack + buckets.c + context_classify.c).
    #
    # PATCH-02 subtypes test: drives ilsp_facade_nav_type_hierarchy_subtypes
    # which lives inside _LSP_PHASE3_NAV_FACADE_SRC (type_hierarchy.c +
    # patch_lookup.c are already linked there). IRON_SOURCE_TREE_ROOT
    # define lets find_fixture() locate tests/lsp/unit/v3_patch/*.iron.
    add_executable(test_v3_patch_subtypes test_v3_patch_subtypes.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_patch_subtypes PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_patch_subtypes PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_patch_subtypes
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_patch_subtypes COMMAND test_v3_patch_subtypes)
    set_tests_properties(test_v3_patch_subtypes PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # PATCH-03 completion test: drives ilsp_complete_buckets_build through
    # ILSP_CCTX_MEMBER_AFTER_DOT. emit_member_fields lives in buckets.c
    # and references ilsp_patch_for_each_method (Plan 11-01) via the new
    # patch-member walk. Mirror test_v3_tier_completion's link set:
    # full nav-facade stack + buckets.c + context_classify.c.
    add_executable(test_v3_patch_completion test_v3_patch_completion.c
        ${_LSP_PHASE3_NAV_FACADE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/buckets.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/context_classify.c)
    target_include_directories(test_v3_patch_completion PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_patch_completion PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_patch_completion
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_patch_completion COMMAND test_v3_patch_completion)
    set_tests_properties(test_v3_patch_completion PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 11 Plan 11-03 (PATCH-04 hover + PATCH-05 references) ──────
    # Task 5 upgraded the link sets from Wave 0 minimal to full nav-facade
    # stack so the real Unity assertions can drive the facade entry points
    # (hover via ilsp_facade_hover; references via
    # ilsp_facade_nav_references). IRON_SOURCE_TREE_ROOT compile-define lets
    # find_fixture() locate tests/lsp/unit/v3_patch/*.iron from a build dir.
    add_executable(test_v3_patch_hover test_v3_patch_hover.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_patch_hover PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_patch_hover PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_patch_hover
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_patch_hover COMMAND test_v3_patch_hover)
    set_tests_properties(test_v3_patch_hover PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 60)

    add_executable(test_v3_patch_references test_v3_patch_references.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_patch_references PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_patch_references PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_patch_references
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_patch_references COMMAND test_v3_patch_references)
    set_tests_properties(test_v3_patch_references PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

    # ── Phase 12 Plan 12-01 (Wave 0 stubs) ──────────────────────────────
    # 3 new test binaries that Plans 12-02 / 12-03 will flip to real
    # assertions. Wave 0 contract: each binary builds, runs, and exits 0
    # reporting its single test as IGNORED. Labels phase-m1-invariant +
    # phase-12-invariant per CONTEXT.md D-37 + VALIDATION.md "Wave 0
    # Requirements".
    #
    # test_v3_quickfix_corpus: drives ilsp_facade_code_action +
    # ilsp_facade_code_action_resolve once Plan 12-03 wires fixtures.
    # Full LSP server stack via _LSP_PLAN03_SERVER_SRC + transport
    # sources (handlers_*.c reach into the writer queue + json
    # serializers); downstream plans may add new quickfix .c sources to
    # the server source list.
    add_executable(test_v3_quickfix_corpus test_v3_quickfix_corpus.c
        ${_LSP_PLAN03_SERVER_SRC}
        ${_LSP_PLAN03_TRANSPORT_SRC})
    target_include_directories(test_v3_quickfix_corpus PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_quickfix_corpus PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_quickfix_corpus
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_quickfix_corpus COMMAND test_v3_quickfix_corpus)
    set_tests_properties(test_v3_quickfix_corpus PROPERTIES
        LABELS "unit;phase-m1-invariant;phase-12-invariant"
        TIMEOUT 60)

    # test_v3_keyword_filter: minimal stack — Plan 12-02 will add
    # keyword_filter.c when it ships. Today the stub touches no sources
    # beyond Unity + iron_compiler. Mirrors test_v3_patch_predicate's
    # minimal-link shape (CMakeLists.txt:1138-1156).
    add_executable(test_v3_keyword_filter test_v3_keyword_filter.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c)
    target_include_directories(test_v3_keyword_filter PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_keyword_filter PRIVATE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_keyword_filter
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_v3_keyword_filter COMMAND test_v3_keyword_filter)
    set_tests_properties(test_v3_keyword_filter PROPERTIES
        LABELS "unit;phase-m1-invariant;phase-12-invariant"
        TIMEOUT 60)

    # test_v3_keyword_drift: single-source binary mirroring
    # test_completion_keyword_mirror.c (Phase 4 D-01). Plan 12-03 wires
    # the in-process drift loop against the regenerated keyword_mirror.h.
    add_executable(test_v3_keyword_drift test_v3_keyword_drift.c)
    target_include_directories(test_v3_keyword_drift PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_keyword_drift PRIVATE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_keyword_drift
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_v3_keyword_drift COMMAND test_v3_keyword_drift)
    set_tests_properties(test_v3_keyword_drift PROPERTIES
        LABELS "unit;phase-m1-invariant;phase-12-invariant"
        TIMEOUT 60)

    # ── Phase 10 Plan 10-03 (TIER-03 + TIER-04) ─────────────────────────
    # 2 facade tests (completion detail tier prefix + signature_help label
    # tier prefix). Each links the full nav facade stack so the v3 tier
    # rendering shipped by Tasks 3 + 4 is exercised against real bytes.
    # The TIER-03 completion test additionally pulls in the edit/complete
    # source family (buckets.c + context_classify.c) since it drives
    # ilsp_complete_buckets_build directly. Wave 0 stubs landed with
    # TEST_IGNORE_MESSAGE; flipped to real assertions in Task 5.
    add_executable(test_v3_tier_completion test_v3_tier_completion.c
        ${_LSP_PHASE3_NAV_FACADE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/buckets.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/context_classify.c)
    target_include_directories(test_v3_tier_completion PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_tier_completion PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_tier_completion
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_tier_completion COMMAND test_v3_tier_completion)
    set_tests_properties(test_v3_tier_completion PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 60)

    add_executable(test_v3_tier_signature_help test_v3_tier_signature_help.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_v3_tier_signature_help PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_v3_tier_signature_help PRIVATE
        _GNU_SOURCE
        IRON_SOURCE_TREE_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_v3_tier_signature_help
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_v3_tier_signature_help COMMAND test_v3_tier_signature_help)
    set_tests_properties(test_v3_tier_signature_help PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 60)

    # ── Phase 3 Plan 06: workspace/diagnostic facade + refresh push ──────
    # Flipped from Wave 0 stub: exercises the cache (hit/miss/evict),
    # the 2 s budget loop termination, and the refresh-notification
    # emitter. Full NAV facade stack is linked so the facade entry can
    # run real analyses against in-memory fixtures.
    # capabilities.c is additionally linked because Test 07 verifies
    # diagnosticProvider.workspaceDiagnostics == true.
    add_executable(test_workspace_diagnostic test_workspace_diagnostic.c
        ${_LSP_PHASE3_NAV_FACADE_SRC}
        ${CMAKE_SOURCE_DIR}/src/lsp/server/capabilities.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/dispatch.c
        # Phase 7 Plan 07-01: dispatch.c now calls crash_ring_push/pop.
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/crash_dump.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/lifecycle.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/dyn_register.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/notifications.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_lifecycle.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_nav.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/mailbox.c
        ${CMAKE_SOURCE_DIR}/src/lsp/workers/ast_worker.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/abort_handler.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/reader.c
        # Phase 4 (M3 Editing Assistance): dispatch.c registers 9 EDIT
        # handler rows; handlers_edit.c hosts them and delegates to
        # facade/edit/ TUs. Every test linking dispatch.c must resolve
        # these symbols.
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_edit.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/complete.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/buckets.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/context_classify.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/resolve.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/auto_import.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/snippet.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/codeaction.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/registry.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/resolve.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/organize_imports.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_undefined_var.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_unused_import.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_missing_return.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/prepare.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/collision.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/apply.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/highlight.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/folding_range.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/selection_range.c
        # Phase 5 (M4 Formatting): 3 fmt handler rows + facade TUs.
        ${CMAKE_SOURCE_DIR}/src/lsp/server/handlers_fmt.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/range_format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/on_type_format.c
        # Phase 7 Plan 07-07 (HARD-22, D-10): handlers_lifecycle.c calls
        # ilsp_server_version() for the initialize response server-info block.
        # Rule 3 (08-02 Task 1): blocking link-graph fix inherited from Phase 7.
        ${CMAKE_SOURCE_DIR}/src/lsp/cli/version.c)
    target_include_directories(test_workspace_diagnostic PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_workspace_diagnostic PRIVATE _GNU_SOURCE)
    target_link_libraries(test_workspace_diagnostic
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_workspace_diagnostic COMMAND test_workspace_diagnostic)
    set_tests_properties(test_workspace_diagnostic PROPERTIES
        LABELS "unit;phase-m2-invariant"
        TIMEOUT 60)

else()
    message(STATUS "Skipping tests/lsp/unit: IRON_BUILD_LSP=OFF")
endif()
