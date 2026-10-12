#!/usr/bin/env python3
"""Keep sanitizer-heavy tests above their safe timeout."""

import json
from pathlib import Path
import subprocess
import sys


FIXTURES = (
    "v4_7_drop_happy",
    "v4_7_boundary_drop_chain",
    "v4_7_boundary_copy_hook",
    "v4_7_boundary_nocopy",
    "v4_3.2_boundary_destructor_runs",
    "v4_7_panic_destructor_panic",
    "v4_7_boundary_partial_init_cleanup",
    "v4_4.3-unchecked-ptr_happy",
    "v4_4.3-unchecked-ptr_boundary_box_unwrap",
    "v4_4.3-unchecked-ptr_boundary_box_takes_ownership",
    "v4_4.3-unchecked-ptr_boundary_box_isnull",
    "v4_4.3-unchecked-ptr_panic_box_unwrap_null",
    "v4_8.1-composition-default_happy",
    "v4_7.5-stdlib_list_drop_filehandle",
    "v4_7.5-stdlib_rawptr_cast",
    "lsp_phase24_smoke",
    "lsp_phase25_smoke",
    "lsp_phase28_smoke",
)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_timeout_contract.py <ctest> <build-dir>", file=sys.stderr)
        return 2
    ctest, build_dir = sys.argv[1:]
    result = subprocess.run(
        [ctest, "--test-dir", str(Path(build_dir)), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = {test["name"]: test for test in json.loads(result.stdout)["tests"]}
    failures = []
    for name in FIXTURES:
        test = tests.get(name)
        if test is None:
            failures.append(f"{name} is not registered")
            continue
        properties = {item["name"]: item["value"] for item in test["properties"]}
        timeout = properties.get("TIMEOUT")
        if not isinstance(timeout, (int, float)) or timeout < 120:
            failures.append(f"{name} TIMEOUT is {timeout!r}, expected at least 120")
    lsp_fuzz = tests.get("lsp_fuzz_short")
    if lsp_fuzz is None:
        failures.append("lsp_fuzz_short is not registered")
    else:
        command = lsp_fuzz.get("command", [])
        try:
            deadline = float(command[command.index("--deadline") + 1])
        except (ValueError, IndexError):
            failures.append("lsp_fuzz_short has no valid --deadline argument")
        else:
            if deadline < 60:
                failures.append(
                    f"lsp_fuzz_short deadline is {deadline!r}, expected at least 60"
                )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("sanitizer-heavy tests have at least 120 seconds")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
