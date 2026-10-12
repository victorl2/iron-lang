#!/usr/bin/env python3
"""A query cache compiled by one tree-sitter CLI is not reused by another."""

import os
from pathlib import Path
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[3]
CHECK = REPO / "tests/grammars/tree_sitter/check_queries.sh"


def main() -> int:
    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        log = tmp / "cache.log"
        fake = tmp / "tree-sitter"
        fake.write_text(
            "#!/usr/bin/env bash\n"
            "if [ \"${1:-}\" = --version ]; then\n"
            "  echo \"tree-sitter ${FAKE_TS_VERSION}\"\n"
            "else\n"
            "  printf \"%s\\n\" \"${TREE_SITTER_LIBDIR}\" >> \"${TREE_SITTER_CACHE_LOG}\"\n"
            "fi\n",
            encoding="utf-8",
        )
        fake.chmod(0o755)

        for version in ("0.25.10", "0.26.8"):
            env = dict(os.environ)
            env.update(
                TREE_SITTER=str(fake),
                TREE_SITTER_CACHE_LOG=str(log),
                FAKE_TS_VERSION=version,
            )
            env.pop("TREE_SITTER_LIBDIR", None)
            subprocess.run(["bash", str(CHECK)], cwd=REPO, env=env, check=True,
                           capture_output=True, text=True)

        paths = {line for line in log.read_text(encoding="utf-8").splitlines() if line}
        assert len(paths) == 2, f"CLI versions reused query cache paths: {sorted(paths)}"
        for version in ("0.25.10", "0.26.8"):
            assert any(path.endswith("/" + version) for path in paths), (
                f"no versioned query cache for tree-sitter {version}: {sorted(paths)}"
            )
    print("tree-sitter query caches are versioned by CLI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
