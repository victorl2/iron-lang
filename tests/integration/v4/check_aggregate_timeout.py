#!/usr/bin/env python3
"""Keep long test timeouts large enough for contended CI runs."""
import json
import subprocess
import sys


def main():
    ctest, build, test_name, minimum_text = sys.argv[1:]
    minimum = int(minimum_text)
    raw = subprocess.check_output(
        [ctest, "--test-dir", build, "--show-only=json-v1"], text=True
    )
    tests = {test["name"]: test for test in json.loads(raw)["tests"]}
    props = {
        prop["name"]: prop["value"]
        for prop in tests[test_name]["properties"]
    }
    timeout = props.get("TIMEOUT", 0)
    if timeout < minimum:
        print(f"{test_name} timeout is {timeout}, expected at least {minimum}")
        return 1
    if test_name == "test_opt_parity" and not props.get("RUN_SERIAL", False):
        print("test_opt_parity must run serially to avoid starving other tests")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
