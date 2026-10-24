#!/usr/bin/env python3
"""Compile parity partners together, but never run them together."""

import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path


def executable(path: Path, body: str) -> None:
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_opt_parity_scheduling.py SOURCE_DIR", file=sys.stderr)
        return 2

    source_dir = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="iron-parity-scheduling-") as temporary:
        root = Path(temporary)
        state = root / "state"
        fixtures = root / "fixtures"
        state.mkdir()
        fixtures.mkdir()
        (fixtures / "fixed_resource.iron").write_text("func main() {}\n", encoding="utf-8")

        compiler = root / "fake-ironc"
        executable(
            compiler,
            """#!/usr/bin/env bash
set -eu
state=$IRON_PARITY_TEST_STATE
if mkdir "$state/build-lock" 2>/dev/null; then
    sleep 0.2
    rmdir "$state/build-lock"
else
    touch "$state/build-overlap"
    sleep 0.2
fi
source=${@: -1}
name=$(basename "$source" .iron)
cat > "$name" <<'PROGRAM'
#!/usr/bin/env bash
set -eu
state=$IRON_PARITY_TEST_STATE
if mkdir "$state/run-lock" 2>/dev/null; then
    sleep 0.2
    echo ok
    rmdir "$state/run-lock"
else
    touch "$state/run-overlap"
    echo collision
fi
PROGRAM
chmod +x "$name"
""",
        )

        environment = os.environ.copy()
        environment.update(
            IRON_PARITY_TEST_STATE=str(state),
            PARITY_JOBS="2",
            PARITY_TIMEOUT="5",
        )
        result = subprocess.run(
            [str(source_dir / "tests/oracles/opt_parity.sh"), str(compiler), str(fixtures)],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert (state / "build-overlap").exists(), "compile partners did not overlap"
        assert not (state / "run-overlap").exists(), "runtime partners overlapped"
        assert "1 agree, 0 disagree" in result.stdout
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
