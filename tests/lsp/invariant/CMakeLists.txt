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

    # Phase 7 Plan 07-01 Task 02 (HARD-20, D-08) -- test_parent_death_detection.sh:
    # fork a fake editor that spawns ironls + exits; assert ironls exits within
    # 5s via SIGTERM delivered by prctl(PR_SET_PDEATHSIG) on Linux. Linux-only:
    # macOS's kqueue watcher is identical in intent but the SIGTERM path is
    # routed via a background thread whose shutdown is covered by the per-TU
    # Unity test (test_parent_watch_prctl is Linux-only; macOS coverage is
    # v1.x tracking).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME test_parent_death_detection
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test_parent_death_detection.sh"
                    $<TARGET_FILE:ironls>)
        set_tests_properties(test_parent_death_detection PROPERTIES
            LABELS "integration;phase-m6-invariant"
            TIMEOUT 30)
    endif()
endif()
