---
phase: 03-runtime-audit-web-hardening
plan: 04
subsystem: ci/portability
tags: [emcc, web, portability, ci, ctest, WEB-RUNTIME-07]
dependency_graph:
  requires:
    - 01-plan-01 (emsdk 4.0.23 pin via .emsdk-version, emsdk-smoke job in web.yml)
  provides:
    - WEB-RUNTIME-07: iron_rc.c + iron_builtins.c + iron_collections.c compile-clean under emcc invariant
    - CI portability probe that fails the emsdk-smoke job on -Wall -Werror violations
    - Local grep-invariant ctest that catches non-portable API additions without emsdk install
  affects:
    - .github/workflows/web.yml (emsdk-smoke job, paths filter)
    - CMakeLists.txt (new ctest entry)
tech_stack:
  added: []
  patterns:
    - inverted-grep ctest (same pattern as test_emsdk_pin_discipline from Phase 2)
    - emcc -c try-compile in CI (reusing existing emsdk setup step)
key_files:
  created: []
  modified:
    - .github/workflows/web.yml
    - CMakeLists.txt
decisions:
  - Use /tmp output paths for emcc probe (-o /tmp/iron_rc.o) rather than /dev/null so test -s checks confirm non-empty object files
  - Grep invariant uses sh -c wrapper to match existing test_emsdk_pin_discipline style
  - Labels unit;web-portability on the new ctest to allow ctest -L web-portability targeted runs
metrics:
  duration: 697s
  completed_date: "2026-04-11"
  tasks_completed: 2
  files_modified: 2
---

# Phase 3 Plan 4: Portability Probe CI Summary

**One-liner:** CI emcc try-compile + local grep-invariant ctest locking WEB-RUNTIME-07 (iron_rc/builtins/collections compile clean under emcc with zero file modifications).

## What Was Built

Two complementary mechanical gates for WEB-RUNTIME-07:

1. **CI gate** (`.github/workflows/web.yml`): A new `Probe iron_rc/builtins/collections portability under emcc (WEB-RUNTIME-07)` step appended to the existing `emsdk-smoke` job. Reuses the already-configured emsdk 4.0.23 setup step (no extra toolchain install cost). Invokes `emcc -c` on each of the three runtime files with `-I src -Wall -Werror`, then asserts the `.o` files are non-empty. Runs on both `ubuntu-latest` and `macos-latest` via the existing strategy matrix. The `paths:` filter in both `push:` and `pull_request:` triggers was extended to include the three runtime file paths so future edits to them trigger the probe automatically.

2. **Local grep-invariant ctest** (`CMakeLists.txt`): A new `test_runtime_files_portability_grep` ctest entry using `! grep -E 'sysconf|pthread_attr|sigaction|sys/wait\.h|sys/resource\.h'` over the three files. Passes on the clean unmodified baseline. Labeled `unit;web-portability` for targeted runs via `ctest -L web-portability`. Gives development-time feedback without requiring a local emsdk install.

## Paper Audit Results

Manual review of all three files before wiring the probe:

- **`iron_rc.c`** (83 lines): `stdlib.h`, `string.h`, `iron_runtime.h`. Only `malloc`, `memcpy`, `free`, `IRON_ATOMIC_*` macros. Zero non-portable APIs.
- **`iron_builtins.c`** (55 lines): `stdio.h`, `stdlib.h`, `iron_runtime.h`. Only `printf`, `fprintf`, `abort`. Zero non-portable APIs.
- **`iron_collections.c`** (57 lines): `stdlib.h`, `string.h`, `runtime/iron_runtime.h`. Only IRON_*_IMPL macro expansions. Zero non-portable APIs.

All three files clean. Probe and grep-invariant both expected to pass and confirmed passing.

## Tasks

| # | Name | Commit | Files |
|---|------|--------|-------|
| 1 | Add emcc portability probe step to web.yml + extend paths filter | c465d8c | `.github/workflows/web.yml` |
| 2 | Add local grep-invariant ctest entry for runtime file portability | 0e23ba2 | `CMakeLists.txt` |

## Verification Results

- `ctest -R test_runtime_files_portability_grep -V`: PASSED (0.01s)
- `ctest -E benchmark_smoke --output-on-failure`: 62/62 PASSED (no regressions)
- `git diff src/runtime/iron_rc.c src/runtime/iron_builtins.c src/runtime/iron_collections.c`: zero lines (WEB-RUNTIME-07 zero-modifications invariant preserved)
- YAML syntax check via `python3 -c "import yaml; yaml.safe_load(...)"`: OK
- All grep acceptance criteria in both tasks: satisfied

## Deviations from Plan

None — plan executed exactly as written.

## Self-Check: PASSED

- `.github/workflows/web.yml` — modified, YAML valid, all required strings present
- `CMakeLists.txt` — modified, new ctest entry present
- Commits c465d8c and 0e23ba2 exist
- Three runtime files have zero modifications
- 62/62 ctests pass
