#!/usr/bin/env python3
"""Exercise the CTest retry driver with a test that passes on attempt two."""

import os
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
            """import pathlib, subprocess, sys
marker = pathlib.Path(sys.argv[1])
if marker.exists():
    raise SystemExit(0)
marker.write_text("first attempt failed", encoding="utf-8")
source = pathlib.Path(sys.argv[2])
build = pathlib.Path(sys.argv[3])
python = sys.argv[4]
(source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(ctest_retry NONE)
enable_testing()
add_test(NAME inserted COMMAND {python} {source / "inserted.py"})
add_test(NAME transient COMMAND {python} {source / "transient.py"} {marker} {source} {build} {python})
''', encoding="utf-8")
subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=True)
raise SystemExit(1)
""",
            encoding="utf-8",
        )
        (source / "inserted.py").write_text(
            f"from pathlib import Path\nPath({str(root / 'wrong-test-ran')!r}).touch()\n",
            encoding="utf-8",
        )
        (source / "CMakeLists.txt").write_text(
            f"""cmake_minimum_required(VERSION 3.25)
project(ctest_retry NONE)
enable_testing()
add_test(NAME transient COMMAND {sys.executable} {source / 'transient.py'} {build / 'marker'} {source} {build} {sys.executable})
""",
            encoding="utf-8",
        )
        subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=True)
        run = subprocess.run(
            [
                str(repo / "scripts/ci/ctest_with_retry.sh"),
                str(build),
                "2",
                os.path.relpath(results, repo),
            ],
            check=False,
            capture_output=True,
            text=True,
            cwd=repo,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        assert results.with_name("fixture-first.xml").is_file()
        assert results.with_name("fixture-retry.xml").is_file()
        retried = results.with_name("fixture-retried.txt").read_text(encoding="utf-8")
        assert retried == "transient\n", repr(retried)
        assert not (root / "wrong-test-ran").exists(), "retry ran the inserted test"
        outcome = results.with_name("fixture-ctest.result").read_text(encoding="utf-8")
        assert "status=pass\n" in outcome
        assert "first_exit_code=8\n" in outcome
        assert "retry_exit_code=0\n" in outcome
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
