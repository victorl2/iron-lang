#!/usr/bin/env python3
"""Keep exhaustive CTest oracles from oversubscribing small CI runners."""

import json
import re
import subprocess
import sys
from pathlib import Path


def properties(test: dict) -> dict:
    return {entry["name"]: entry["value"] for entry in test["properties"]}


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_aggregate_scheduling.py BUILD_DIR WORKFLOW", file=sys.stderr)
        return 2

    build_dir = Path(sys.argv[1])
    listed = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = {test["name"]: test for test in json.loads(listed.stdout)["tests"]}

    names = ("test_v4_acceptance", "test_opt_parity", "test_leak_check")
    for name in names:
        props = properties(tests[name])
        assert props.get("PROCESSORS", 0) >= 1, f"{name} must reserve CTest slots"
        assert props.get("RESOURCE_LOCK") == ["iron_exhaustive_corpus"], (
            f"{name} must share the exhaustive-corpus resource lock"
        )
        assert props.get("TIMEOUT", 0) >= 7200, f"{name} needs two hours of headroom"

    leak_labels = properties(tests["test_leak_check"])["LABELS"]
    assert "v4-acceptance" not in leak_labels, "the acceptance gate must not run valgrind"

    workflow = Path(sys.argv[2]).read_text(encoding="utf-8")
    timeout = re.search(r"^\s*timeout-minutes:\s*(\d+)\s*$", workflow, re.MULTILINE)
    assert timeout and int(timeout.group(1)) >= 60, "the acceptance job needs 60 minutes"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
