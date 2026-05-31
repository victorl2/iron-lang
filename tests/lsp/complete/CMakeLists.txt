# Phase 34 LSP-03 Plan 34-03 — completion driver tests for the 10 new v3/v4
# keyword arms + the defer-free snippet. Each executable is a Unity test
# linking only the minimal slice of src/lsp/ + iron_compiler needed by the
# predicate / snippet renderer it exercises. Mirrors the spirit of
# tests/lsp/unit/test_v3_keyword_filter (Phase 12 Plan 12-02): one focused
# binary per surface, no dispatcher / transport / worker stack pulled in.

if(IRON_BUILD_LSP)
    # ── Task 1: v3/v4 keyword visibility ─────────────────────────────────
    add_executable(test_complete_v4_keywords test_complete_v4_keywords.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/keyword_filter.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/context_classify.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c)
    target_include_directories(test_complete_v4_keywords PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(test_complete_v4_keywords
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_complete_v4_keywords COMMAND test_complete_v4_keywords)
    set_tests_properties(test_complete_v4_keywords PROPERTIES
        LABELS "phase34-invariant;complete"
        TIMEOUT 60)

    # ── Task 2: `defer free <binding>` snippet ──────────────────────────
    # Wired by Task 2 of Plan 34-03.
endif()
