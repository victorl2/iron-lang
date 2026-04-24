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

    # Phase 7 Plan 07-04 Task 01 (HARD-18, D-06) -- SLO measurement
    # harness self-test. Validates the pure-Python percentile function,
    # the threshold enforcement decision, the LSP framing round-trip,
    # the medium-fixture LoC window, and the D-06 locked literal
    # thresholds (20/100/500/50 ms) in < 1 s. Mitigates T-07-04-06
    # (percentile regression masking threshold enforcement) cheaply
    # enough that every normal ctest run exercises it pre-build.
    add_test(NAME test_slo_measurement_self_test
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/lsp/slos/measurement.py"
                --self-test)
    set_tests_properties(test_slo_measurement_self_test PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 30)

    # Phase 7 Plan 07-04 Task 01 (HARD-18, D-06) -- medium fixture
    # LoC guard. Plan done-criterion locks medium_500loc.iron to
    # 495..505 lines; this invariant catches accidental drift.
    add_test(NAME test_slo_medium_fixture_loc
        COMMAND python3 -c "
with open('${CMAKE_SOURCE_DIR}/tests/lsp/slos/fixtures/medium_500loc.iron') as f:
    n = sum(1 for _ in f)
assert 495 <= n <= 505, f'medium_500loc.iron: {n} outside 495..505'
print(f'OK: {n}')")
    set_tests_properties(test_slo_medium_fixture_loc PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 10)

    # Phase 7 Plan 07-04 Task 02 (HARD-23, D-06) -- build-time regression
    # script self-test. Validates FIFO-trim per-OS window behaviour +
    # threshold decision at 1.15 + warmup tolerance + literal constants
    # (1.15 threshold, 20-sample window) in < 1 s. Catches drift in the
    # rolling-baseline logic before any real build is measured.
    add_test(NAME test_build_time_self_test
        COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/ci/build_time_check.py"
                --self-test)
    set_tests_properties(test_build_time_self_test PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 30)

    # Phase 7 Plan 07-04 Task 02 (HARD-23, D-06) -- baseline JSON shape
    # guard. The committed build-time-baseline.json MUST be a JSON
    # array of objects with required keys {sha, ts, seconds, os} and
    # must contain at least 5 samples per OS (warmup tolerance seed).
    # T-07-04-03 mitigation: catches accidental corruption of the
    # baseline file in PRs that don't touch the workflow.
    add_test(NAME test_build_time_baseline_shape
        COMMAND python3 -c "
import json
b = json.load(open('${CMAKE_SOURCE_DIR}/build-time-baseline.json'))
assert isinstance(b, list), 'baseline must be a JSON array'
assert len(b) >= 5, f'baseline needs >= 5 seed samples, got {len(b)}'
for i, e in enumerate(b):
    for k in ('sha', 'ts', 'seconds', 'os'):
        assert k in e, f'entry {i} missing {k}'
per_os = {}
for e in b:
    per_os[e['os']] = per_os.get(e['os'], 0) + 1
for os_name, count in per_os.items():
    assert count >= 5, f'{os_name} has {count} samples; need >= 5 for warmup'
print(f'OK: {len(b)} total; ' + ', '.join(f'{k}={v}' for k, v in sorted(per_os.items())))")
    set_tests_properties(test_build_time_baseline_shape PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 10)

    # Phase 7 Plan 07-07 Task 01 (HARD-22, D-10) -- version stamp
    # coherence across all three binaries. Runs `iron --version`,
    # `ironc --version`, `ironls --version` and asserts every output
    # carries the same IRON_VERSION_FULL semver string. This pins the
    # single-source-of-truth invariant documented in CONTEXT.md D-10:
    # the CMakeLists.txt IRON_VERSION_FULL variable flows through the
    # IRON_VERSION_STRING compile-time define into each binary, and
    # the initialize response's serverInfo.version plus the
    # --version print all use the same accessor.
    #
    # Failure modes the test catches:
    #   * a hardcoded literal in one binary drifting from IRON_VERSION_FULL
    #   * the ironls initialize.c forgetting to call ilsp_server_version()
    #   * a stale build picking up old objects for one binary but not another
    #   * an install-layout regression where IRON_VERSION_STRING isn't
    #     plumbed into a newly-added target
    add_test(NAME test_version_stamp_coherence
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test_version_stamp_coherence.sh"
                $<TARGET_FILE:iron>
                $<TARGET_FILE:ironc>
                $<TARGET_FILE:ironls>)
    set_tests_properties(test_version_stamp_coherence PROPERTIES
        LABELS "integration;phase-m6-invariant"
        TIMEOUT 15)

    # Phase 7 Plan 07-08 Task 01 (HARD-24, D-11) -- test_phase7_audit.sh:
    # meta-gate that verifies every Phase 7 D-14 artifact still exists,
    # every phase-m{1..6}-invariant CTest label is populated, and both
    # parity harnesses are registered in the CTest graph. This is the
    # regression fence that catches accidental removal of Phase 7
    # critical infrastructure (for example parity.yml) BEFORE branch
    # protection would catch the missing `parity` required status check.
    #
    # Passes WORKING_DIRECTORY so the script can resolve REPO_ROOT
    # robustly regardless of where ctest invokes it from.
    add_test(NAME test_phase7_audit
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test_phase7_audit.sh"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(test_phase7_audit PROPERTIES
        LABELS "integration;phase-m6-invariant"
        ENVIRONMENT "IRON_BUILD_DIR=${CMAKE_BINARY_DIR}"
        TIMEOUT 30)
endif()
