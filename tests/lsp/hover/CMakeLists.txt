# Phase 34 Plan 02 -- hover memory-model annotation block driver.
#
# Drives ilsp_facade_hover end-to-end against the 7 hover_*.iron / .expected
# fixtures reserved in Plan 34-01. Mirrors the link recipe used by
# test_hover_formatter (tests/lsp/unit/) -- both share the same nav-facade
# source set so any future facade refactor propagates here automatically.
#
# _LSP_PHASE3_NAV_FACADE_SRC is exported PARENT_SCOPE from
# tests/lsp/unit/CMakeLists.txt; add_subdirectory(unit) must run BEFORE
# add_subdirectory(hover) in tests/lsp/CMakeLists.txt for the variable
# to be visible here.

if(IRON_BUILD_LSP)
    add_executable(test_hover_memory_model
        test_hover_memory_model.c
        ${_LSP_PHASE3_NAV_FACADE_SRC})
    target_include_directories(test_hover_memory_model PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_hover_memory_model PRIVATE _GNU_SOURCE)
    target_link_libraries(test_hover_memory_model
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    # Fixture paths inside the driver are relative to the source tree
    # root (tests/lsp/fixtures/hover_*.iron) so the test must run with
    # WORKING_DIRECTORY = CMAKE_SOURCE_DIR. Same convention as the
    # parity tests (tests/lsp/parity/CMakeLists.txt).
    add_test(NAME test_hover_memory_model
        COMMAND test_hover_memory_model
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(test_hover_memory_model PROPERTIES
        LABELS "phase34-invariant;hover;lsp-unit"
        TIMEOUT 60)
endif()
