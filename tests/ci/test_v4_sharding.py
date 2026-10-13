#!/usr/bin/env python3
"""Prove that the eight acceptance shards partition the full v4 corpus."""

import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_v4_sharding.py REPO", file=sys.stderr)
        return 2

    repo = Path(sys.argv[1])
    expected = {
        str(path.resolve())
        for path in (repo / "tests/integration/v4").rglob("*.iron")
    }
    observed: set[str] = set()
    for shard in range(1, 9):
        env = os.environ.copy()
        env.update(
            IRON_TEST_LIST_ONLY="1",
            IRON_TEST_SHARD=f"{shard}/8",
            IRON_TEST_JOBS="1",
        )
        result = subprocess.run(
            [str(repo / "tests/run_tests.sh"), "v4", "/bin/true"],
            cwd=repo,
            env=env,
            check=True,
            capture_output=True,
            text=True,
        )
        fixtures = {
            line.removeprefix("FIXTURE ")
            for line in result.stdout.splitlines()
            if line.startswith("FIXTURE ")
        }
        overlap = observed & fixtures
        assert not overlap, f"fixtures occur in multiple shards: {sorted(overlap)[:3]}"
        observed.update(fixtures)

    assert observed == expected, (
        f"shards lost {len(expected - observed)} fixtures and added "
        f"{len(observed - expected)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
