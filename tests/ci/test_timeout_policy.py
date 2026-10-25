#!/usr/bin/env python3
"""Verify that the central timeout multiplier reaches CTest metadata."""

import json
import subprocess
import sys
from pathlib import Path


def properties(test: dict) -> dict:
    return {entry["name"]: entry["value"] for entry in test["properties"]}


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_timeout_policy.py BUILD_DIR SCALE", file=sys.stderr)
        return 2

    build_dir = Path(sys.argv[1])
    scale = int(sys.argv[2])
    listed = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = {test["name"]: test for test in json.loads(listed.stdout)["tests"]}

    timeout = properties(tests["test_sh_hijack_canary"])["TIMEOUT"]
    assert timeout == 15 * scale, (
        f"test_sh_hijack_canary timeout is {timeout}, expected {15 * scale}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
