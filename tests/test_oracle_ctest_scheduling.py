#!/usr/bin/env python3
"""Keep exhaustive CTest oracles from oversubscribing small CI runners."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def properties(test: dict) -> dict:
    return {entry["name"]: entry["value"] for entry in test["properties"]}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_oracle_ctest_scheduling.py <build-dir>", file=sys.stderr)
        return 2

    result = subprocess.run(
        ["ctest", "--test-dir", sys.argv[1], "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = {test["name"]: test for test in json.loads(result.stdout)["tests"]}
    failed = False
    acceptance = properties(tests["test_v4_acceptance"])
    if acceptance.get("PROCESSORS", 0) < 4:
        print("test_v4_acceptance: PROCESSORS must be at least 4", file=sys.stderr)
        failed = True
    if acceptance.get("TIMEOUT", 0) < 7200:
        print("test_v4_acceptance: TIMEOUT must be at least 7200 seconds", file=sys.stderr)
        failed = True
    for name in ("test_opt_parity", "test_leak_check"):
        props = properties(tests[name])
        if props.get("PROCESSORS", 0) < 4:
            print(f"{name}: PROCESSORS must be at least 4", file=sys.stderr)
            failed = True
        if props.get("TIMEOUT", 0) < 1800:
            print(f"{name}: TIMEOUT must be at least 1800 seconds", file=sys.stderr)
            failed = True
        if props.get("RESOURCE_LOCK") != ["iron_exhaustive_corpus"]:
            print(f"{name}: must lock iron_exhaustive_corpus", file=sys.stderr)
            failed = True
        if props.get("TIMEOUT", 0) < 7200:
            print(f"{name}: TIMEOUT must be at least 7200 seconds", file=sys.stderr)
            failed = True
    parallel_fuzz = (
        "prog_fuzz",
        "prog_fuzz_types",
        "prog_fuzz_conc",
        "prog_fuzz_arena",
        "prog_fuzz_sync",
        "prog_fuzz_nocopy",
        "prog_fuzz_stdlib",
        "prog_fuzz_edges_num",
        "prog_fuzz_edges_types",
        "prog_fuzz_edges_mem",
        "cg_fuzz_modes",
        "cg_fuzz_meta",
        "cg_fuzz_mutate",
        "cg_fuzz_shapes",
    )
    for name in parallel_fuzz:
        props = properties(tests[name])
        if props.get("PROCESSORS", 0) < 4:
            print(f"{name}: PROCESSORS must be at least 4", file=sys.stderr)
            failed = True

    soa_fuzz = properties(tests["prog_fuzz_soa"])
    if soa_fuzz.get("PROCESSORS", 0) < 2:
        print("prog_fuzz_soa: PROCESSORS must be at least 2", file=sys.stderr)
        failed = True
    if soa_fuzz.get("TIMEOUT", 0) < 1800:
        print("prog_fuzz_soa: TIMEOUT must be at least 1800 seconds", file=sys.stderr)
        failed = True

    four_worker_tests = (
        "mod_fuzz_pkg",
        "mod_fuzz_incr",
        "mod_fuzz_tests",
        "cli_fuzz",
        "test_http_iron_concurrency",
        "test_websocket_iron_roundtrip",
        "test_file_operations_iron_roundtrip",
        "test_udp_iron_payload_roundtrip",
        "lsp_phase22_smoke",
    )
    for name in four_worker_tests:
        if properties(tests[name]).get("PROCESSORS", 0) < 4:
            print(f"{name}: PROCESSORS must be at least 4", file=sys.stderr)
            failed = True

    three_worker_tests = (
        "lsp_fuzz_short",
        "fmt_fuzz_short",
    )
    for name in three_worker_tests:
        if properties(tests[name]).get("PROCESSORS", 0) < 3:
            print(f"{name}: PROCESSORS must be at least 3", file=sys.stderr)
            failed = True

    lsp_fuzz_command = tests["lsp_fuzz_short"]["command"]
    try:
        deadline = float(lsp_fuzz_command[lsp_fuzz_command.index("--deadline") + 1])
    except (ValueError, IndexError):
        deadline = 0
    if deadline < 120:
        print("lsp_fuzz_short: request deadline must be at least 120 seconds",
              file=sys.stderr)
        failed = True

    two_worker_tests = (
        "json_fuzz",
        "test_manual",
        "test_algorithms",
        "test_composite",
        "test_compile_fail",
        "test_v4_acceptance_fail",
        "test_v4_migrated_from_v3",
        "debug_stepping_smoke",
        "debug_info_smoke",
        "debug_values_smoke",
        "debug_temporaries_smoke",
        "debug_dap_smoke",
        "debug_dap_eval_smoke",
        "debug_dap_temporaries",
        "debug_panic_smoke",
    )
    for name in two_worker_tests:
        if properties(tests[name]).get("PROCESSORS", 0) < 2:
            print(f"{name}: PROCESSORS must be at least 2", file=sys.stderr)
            failed = True

    for name, test in tests.items():
        props = properties(test)
        if "aggregate-parity" in props.get("LABELS", []) and props.get("TIMEOUT", 0) < 180:
            print(f"{name}: aggregate-parity TIMEOUT must be at least 180 seconds",
                  file=sys.stderr)
            failed = True
        if name.startswith("v4_") and "integration" in props.get("LABELS", []) and \
                props.get("TIMEOUT", 0) < 180:
            print(f"{name}: v4 integration TIMEOUT must be at least 180 seconds",
                  file=sys.stderr)
            failed = True

    repo = Path(__file__).resolve().parents[1]
    parity = (repo / "tests/oracles/opt_parity.sh").read_text(encoding="utf-8")
    parity_timeout = re.search(r"TIMEOUT=\$\{PARITY_TIMEOUT:-(\d+)\}", parity)
    if not parity_timeout or int(parity_timeout.group(1)) < 30:
        print("opt_parity.sh: default child timeout must be at least 30 seconds",
              file=sys.stderr)
        failed = True

    workflow = (repo / ".github/workflows/v4-acceptance.yml").read_text(encoding="utf-8")
    workflow_timeout = re.search(r"^\s*timeout-minutes:\s*(\d+)\s*$", workflow,
                                 re.MULTILINE)
    if not workflow_timeout or int(workflow_timeout.group(1)) < 60:
        print("v4-acceptance workflow: timeout must be at least 60 minutes",
              file=sys.stderr)
        failed = True

    runner = Path(__file__).with_name("run_tests.sh")
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as cpu_max:
        cpu_max.write("400000 100000\n")
        cpu_max.flush()
        env = dict(os.environ, IRON_TEST_PRINT_JOBS="1",
                   IRON_TEST_CPU_MAX_FILE=cpu_max.name)
        env.pop("IRON_TEST_JOBS", None)
        jobs = subprocess.run(["bash", str(runner)], check=True, capture_output=True,
                              text=True, env=env).stdout.strip()
        if jobs != "2":
            print(f"run_tests.sh: 4-CPU cgroup quota selected {jobs} workers, want 2",
                  file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
