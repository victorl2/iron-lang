#!/usr/bin/env python3
"""Lock the recoverability and two-pass guarantees of local CI."""

import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_local_ci_contract.py REPO", file=sys.stderr)
        return 2

    repo = Path(sys.argv[1])
    scripts = [
        repo / "scripts/ci/local_ci.sh",
        repo / "scripts/ci/remote_linux_ci.sh",
        repo / "scripts/ci/run_linux_leg.sh",
        repo / "scripts/ci/ctest_with_retry.sh",
    ]
    subprocess.run(["bash", "-n", *map(str, scripts)], check=True)

    local = scripts[0].read_text(encoding="utf-8")
    remote = scripts[1].read_text(encoding="utf-8")
    retry = scripts[3].read_text(encoding="utf-8")

    assert "status|wait" in local
    assert "/proc/loadavg" in local and "nproc" in local
    assert "CI_JOBS_ASAN" in local
    assert "CI_JOBS_RELEASE" in local
    assert "CI_JOBS_LEAKS" in local
    assert "podman run -d --name \"$prefix-$leg\"" in remote
    assert "CREATE TABLE iron_ci_ready" in remote
    assert "DROP TABLE iron_ci_ready" in remote
    assert "--output-junit" in retry
    assert "--rerun-failed" in retry

    forbidden = ("podman system prune", "podman container prune", "podman rm -af")
    for command in forbidden:
        assert command not in remote, f"run-unscoped cleanup is forbidden: {command}"
    for line in remote.splitlines():
        if "podman rm -f" in line:
            assert "$prefix-" in line, f"cleanup is not run-scoped: {line}"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
