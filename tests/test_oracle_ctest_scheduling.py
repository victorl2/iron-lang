#!/usr/bin/env python3
"""Keep exhaustive CTest oracles from oversubscribing small CI runners."""

import json
import subprocess
import sys


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
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
