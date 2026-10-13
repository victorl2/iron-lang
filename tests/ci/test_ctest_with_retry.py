#!/usr/bin/env python3
"""Exercise the CTest retry driver with a test that passes on attempt two."""

import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_ctest_with_retry.py REPO", file=sys.stderr)
        return 2

    repo = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="iron-ctest-retry-") as tmp:
        root = Path(tmp)
        source = root / "source"
        build = root / "build"
        results = root / "results" / "fixture"
        source.mkdir()
        (source / "transient.py").write_text(
            """import pathlib, sys
marker = pathlib.Path(sys.argv[1])
if marker.exists():
    raise SystemExit(0)
marker.write_text("first attempt failed", encoding="utf-8")
raise SystemExit(1)
""",
            encoding="utf-8",
        )
        (source / "CMakeLists.txt").write_text(
            f"""cmake_minimum_required(VERSION 3.25)
project(ctest_retry NONE)
enable_testing()
add_test(NAME transient COMMAND {sys.executable} {source / 'transient.py'} {build / 'marker'})
""",
            encoding="utf-8",
        )
        subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=True)
        run = subprocess.run(
            [
                str(repo / "scripts/ci/ctest_with_retry.sh"),
                str(build),
                "2",
                str(results),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        assert results.with_name("fixture-first.xml").is_file()
        assert results.with_name("fixture-retry.xml").is_file()
        retried = results.with_name("fixture-retried.txt").read_text(encoding="utf-8")
        assert retried == "transient\n", repr(retried)
        outcome = results.with_name("fixture-ctest.result").read_text(encoding="utf-8")
        assert "status=pass\n" in outcome
        assert "first_exit_code=8\n" in outcome
        assert "retry_exit_code=0\n" in outcome
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
