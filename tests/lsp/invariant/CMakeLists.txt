# Phase 7 Plan 07-01 Task 01 (HARD-14) -- CTest shell invariant for the
# crash-dump pipeline. Task 02 extends this file with the parent-death
# invariant.
#
# The test scripts live in this directory and expect `ironls`'s absolute
# path as their only argument. They create their own tmpdir and
# $XDG_STATE_HOME to stay hermetic.
#
# Labelled phase-m6-invariant so `ctest -L phase-m6-invariant` exercises
# the whole Phase 7 surface.

if(IRON_BUILD_LSP)
    # test_crash_dump_written.sh: spawn ironls, kill -SEGV, assert a
    # 3-section .dmp file is produced under $XDG_STATE_HOME/iron-lsp/
    # crashes/.
    add_test(NAME test_crash_dump_written
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test_crash_dump_written.sh"
                $<TARGET_FILE:ironls>)
    set_tests_properties(test_crash_dump_written PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 30)
endif()
