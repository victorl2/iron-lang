#!/usr/bin/env python3
"""Keep long test timeouts large enough for contended CI runs."""
import json
import subprocess
import sys


def main():
    ctest, build, test_name, minimum_text, *flags = sys.argv[1:]
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
    if "serial" in flags and not props.get("RUN_SERIAL", False):
        print(f"{test_name} must run serially to avoid starving other tests")
        return 1
    environment = props.get("ENVIRONMENT", [])
    if isinstance(environment, str):
        environment = environment.split(";")
    for flag in flags:
        if flag.startswith("env:") and flag[4:] not in environment:
            print(f"{test_name} environment is missing {flag[4:]}")
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
