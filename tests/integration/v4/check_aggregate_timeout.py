#!/usr/bin/env python3
"""Keep the optimizer parity timeout large enough for contended CI runs."""
import json
import subprocess
import sys


def main():
    ctest, build = sys.argv[1:]
    raw = subprocess.check_output(
        [ctest, "--test-dir", build, "--show-only=json-v1"], text=True
    )
    tests = {test["name"]: test for test in json.loads(raw)["tests"]}
    props = {
        prop["name"]: prop["value"]
        for prop in tests["test_opt_parity"]["properties"]
    }
    timeout = props.get("TIMEOUT", 0)
    if timeout < 7200:
        print(f"test_opt_parity timeout is {timeout}, expected at least 7200")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
