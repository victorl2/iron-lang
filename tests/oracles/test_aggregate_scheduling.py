#!/usr/bin/env python3
"""Heavy aggregate CTest rows must not overlap on a parallel runner."""

import json
import subprocess
import sys


ctest, build_dir = sys.argv[1:3]
result = subprocess.run(
    [ctest, "--test-dir", build_dir, "--show-only=json-v1"],
    check=True,
    capture_output=True,
    text=True,
)
tests = {test["name"]: test for test in json.loads(result.stdout)["tests"]}
required = (
    "test_v4_acceptance",
    "test_opt_parity",
    "test_leak_check",
    "test_freestanding",
    "test_v4_migrated_from_v3",
)

failed = []
for name in required:
    test = tests.get(name)
    if test is None:
        failed.append(f"{name}: missing")
        continue
    properties = {item["name"]: item["value"] for item in test.get("properties", [])}
    if properties.get("RUN_SERIAL") is not True:
        failed.append(f"{name}: RUN_SERIAL is not true")

if failed:
    print("\n".join(failed), file=sys.stderr)
    raise SystemExit(1)

print("aggregate scheduling: PASS")
