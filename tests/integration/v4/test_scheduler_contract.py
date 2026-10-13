#!/usr/bin/env python3
"""Check that CTest reserves the workers used inside optimizer parity."""

import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: test_scheduler_contract.py <ctest> <build-dir> <expected-workers>",
            file=sys.stderr,
        )
        return 2
    ctest, build_dir, expected_text = sys.argv[1:]
    expected = int(expected_text)
    result = subprocess.run(
        [ctest, "--test-dir", str(Path(build_dir)), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = {test["name"]: test for test in json.loads(result.stdout)["tests"]}
    parity = tests.get("test_opt_parity")
    if parity is None:
        print("test_opt_parity is not registered", file=sys.stderr)
        return 1
    properties = {item["name"]: item["value"] for item in parity["properties"]}
    if properties.get("PROCESSORS") != expected:
        print(
            f"test_opt_parity PROCESSORS is {properties.get('PROCESSORS')!r}, "
            f"expected {expected}",
            file=sys.stderr,
        )
        return 1
    print(f"test_opt_parity reserves all {expected} internal workers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
