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

    acceptance = tuple(f"test_v4_acceptance_{i}" for i in range(1, 9))
    leaks = tuple(f"test_leak_check_{i}" for i in range(1, 9))
    for name in acceptance + leaks:
        props = properties(tests[name])
        assert props.get("PROCESSORS", 1) == 1, f"{name} must use one CTest slot"
        assert props.get("TIMEOUT", 0) >= 7200, f"{name} needs two hours of headroom"

    parity = properties(tests["test_opt_parity"])
    assert parity.get("PROCESSORS", 0) >= 1, "optimizer parity must reserve its worker pool"
    assert parity.get("RESOURCE_LOCK") == ["iron_exhaustive_corpus"]
    assert parity.get("TIMEOUT", 0) >= 7200

    for name in leaks:
        leak_labels = properties(tests[name])["LABELS"]
        assert "v4-acceptance" not in leak_labels, "the acceptance gate must not run valgrind"

    leak_fixtures: set[Path] = set()
    for name in leaks:
        command = tests[name]["command"]
        fixtures = {Path(argument) for argument in command[2:]}
        assert all(path.suffix == ".iron" for path in fixtures), (
            f"{name} contains a non-fixture argument"
        )
        assert not leak_fixtures & fixtures, f"{name} overlaps another leak shard"
        leak_fixtures.update(fixtures)
    source_dir = Path(sys.argv[2]).resolve().parents[2]
    expected_leaks = set((source_dir / "tests/integration/v4").rglob("*.iron"))
    assert leak_fixtures == expected_leaks, "leak shards must partition every v4 fixture"

    workflow = Path(sys.argv[2]).read_text(encoding="utf-8")
    timeout = re.search(r"^\s*timeout-minutes:\s*(\d+)\s*$", workflow, re.MULTILINE)
    assert timeout and int(timeout.group(1)) >= 60, "the acceptance job needs 60 minutes"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
