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

    # Phase 7 Plan 07-02 Task 01 (HARD-15, D-03) -- test_rss_cap_trips_exit_42.sh:
    # export IRON_LSP_RSS_CAP_BYTES=65536 (64 KiB tiny cap), spawn ironls,
    # keep stdin open via FIFO holder, wait for the 5s sampler to trip the
    # cap, assert exit code exactly 42 AND that the rss-restart-<iso8601>.log
    # marker file was written under $XDG_STATE_HOME/iron-lsp/.
    add_test(NAME test_rss_cap_trips_exit_42
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test_rss_cap_trips_exit_42.sh"
                $<TARGET_FILE:ironls>)
    set_tests_properties(test_rss_cap_trips_exit_42 PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 45)

    # Phase 7 Plan 07-02 Task 02 (HARD-16, D-04) -- Python soak harness
    # self-test. Validates analyze() logic (peak + linear-regression
    # growth) with synthetic RSS curves WITHOUT actually running a
    # full soak. CI-time guard against regressions in the analyzer
    # that would otherwise only surface on a real 30-min/8h run.
    add_test(NAME test_soak_harness_self_test
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/lsp/soak/harness.py"
                --self-test)
    set_tests_properties(test_soak_harness_self_test PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 30)

    # Phase 7 Plan 07-02 Task 02 (HARD-16, D-04) -- lock-in guard for
    # the committed workload files. Confirms workload.jsonl has exactly
    # 28,800 events and short.jsonl has exactly 1,800 events, mitigating
    # T-07-02-02 (workload drift).
    add_test(NAME test_soak_workload_counts
        COMMAND python3 -c "
import sys
full = open('${CMAKE_SOURCE_DIR}/tests/lsp/soak/workload.jsonl').readlines()
short = open('${CMAKE_SOURCE_DIR}/tests/lsp/soak/short.jsonl').readlines()
assert len(full) == 28800, f'workload.jsonl: {len(full)} != 28800'
assert len(short) == 1800, f'short.jsonl: {len(short)} != 1800'
print('OK: 28800 + 1800')")
    set_tests_properties(test_soak_workload_counts PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 10)

    # Phase 7 Plan 07-03 Task 01 (HARD-17, D-05) -- TSAN driver
    # self-test. Validates detect_tsan_finding() against synthetic
    # WARNING/clean/multi-warning/embedded-quote fixtures AND asserts
    # the committed workload.jsonl has exactly 300 events. Mitigates
    # T-07-03-01 (detector regression masking real races) cheaply
    # enough (< 1 s) that every normal ctest run exercises it.
    add_test(NAME test_tsan_driver_self_test
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/lsp/tsan/driver.py"
                --self-test)
    set_tests_properties(test_tsan_driver_self_test PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 30)

    # Phase 7 Plan 07-03 Task 01 (HARD-17, D-05) -- lock-in guard for
    # the committed TSAN workload. 300 is the D-05-locked event count;
    # drift is caught here so tsan.yml doesn't silently run a shorter
    # soak.
    add_test(NAME test_tsan_workload_count
        COMMAND python3 -c "
wl = open('${CMAKE_SOURCE_DIR}/tests/lsp/tsan/workload.jsonl').readlines()
assert len(wl) == 300, f'tsan/workload.jsonl: {len(wl)} != 300'
print('OK: 300')")
    set_tests_properties(test_tsan_workload_count PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 10)
endif()
