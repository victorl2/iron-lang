#!/usr/bin/env python3
"""Keep exhaustive CTest oracles from oversubscribing small CI runners."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def properties(test: dict) -> dict:
    return {entry["name"]: entry["value"] for entry in test["properties"]}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_oracle_ctest_scheduling.py <build-dir>", file=sys.stderr)
        return 2

    result = subprocess.run(
        ["ctest", "--test-dir", sys.argv[1], "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = {test["name"]: test for test in json.loads(result.stdout)["tests"]}
    failed = False
    acceptance = properties(tests["test_v4_acceptance"])
    if acceptance.get("PROCESSORS", 0) < 4:
        print("test_v4_acceptance: PROCESSORS must be at least 4", file=sys.stderr)
        failed = True
    if acceptance.get("TIMEOUT", 0) < 7200:
        print("test_v4_acceptance: TIMEOUT must be at least 7200 seconds", file=sys.stderr)
        failed = True
    for name in ("test_opt_parity", "test_leak_check"):
        props = properties(tests[name])
        if props.get("RUN_SERIAL") is not True:
            print(f"{name}: RUN_SERIAL must be true", file=sys.stderr)
            failed = True
        if props.get("TIMEOUT", 0) < 7200:
            print(f"{name}: TIMEOUT must be at least 7200 seconds", file=sys.stderr)
            failed = True
    parallel_fuzz = (
        "prog_fuzz",
        "prog_fuzz_types",
        "prog_fuzz_conc",
        "prog_fuzz_arena",
        "prog_fuzz_sync",
        "prog_fuzz_stdlib",
        "prog_fuzz_edges_num",
        "prog_fuzz_edges_types",
        "prog_fuzz_edges_mem",
    )
    for name in parallel_fuzz:
        props = properties(tests[name])
        if props.get("PROCESSORS", 0) < 4:
            print(f"{name}: PROCESSORS must be at least 4", file=sys.stderr)
            failed = True
        if props.get("TIMEOUT", 0) < 1800:
            print(f"{name}: TIMEOUT must be at least 1800 seconds", file=sys.stderr)
            failed = True

    four_worker_tests = (
        "mod_fuzz_pkg",
        "mod_fuzz_incr",
        "mod_fuzz_tests",
        "cli_fuzz",
    )
    for name in four_worker_tests:
        if properties(tests[name]).get("PROCESSORS", 0) < 4:
            print(f"{name}: PROCESSORS must be at least 4", file=sys.stderr)
            failed = True

    two_worker_tests = (
        "json_fuzz",
        "test_manual",
        "test_algorithms",
        "test_composite",
        "test_compile_fail",
        "test_v4_acceptance_fail",
        "test_v4_migrated_from_v3",
    )
    for name in two_worker_tests:
        if properties(tests[name]).get("PROCESSORS", 0) < 2:
            print(f"{name}: PROCESSORS must be at least 2", file=sys.stderr)
            failed = True

    runner = Path(__file__).with_name("run_tests.sh")
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as cpu_max:
        cpu_max.write("400000 100000\n")
        cpu_max.flush()
        env = dict(os.environ, IRON_TEST_PRINT_JOBS="1",
                   IRON_TEST_CPU_MAX_FILE=cpu_max.name)
        env.pop("IRON_TEST_JOBS", None)
        jobs = subprocess.run(["bash", str(runner)], check=True, capture_output=True,
                              text=True, env=env).stdout.strip()
        if jobs != "2":
            print(f"run_tests.sh: 4-CPU cgroup quota selected {jobs} workers, want 2",
                  file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
