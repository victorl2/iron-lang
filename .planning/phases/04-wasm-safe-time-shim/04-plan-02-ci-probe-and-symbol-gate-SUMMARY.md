---
phase: 04-wasm-safe-time-shim
plan: 02
subsystem: ci-and-ctest
tags: [ci, ctest, web-portability, emcc, symbol-gate, pin-discipline]
dependency_graph:
  requires: [04-01]
  provides: [WEB-RUNTIME-06-compile-gate, iron_time_web_symbol_invariant, iron_time_web_pin_discipline]
  affects: [.github/workflows/web.yml, CMakeLists.txt]
tech_stack:
  added: []
  patterns: [emcc-portability-probe, ctest-grep-invariant, inverted-grep-pin-discipline]
key_files:
  created: []
  modified:
    - .github/workflows/web.yml
    - CMakeLists.txt
decisions:
  - "Renamed probe step title to reference both WEB-RUNTIME-07 and WEB-RUNTIME-06 (append, not rename) — preserves Phase 3 context while documenting Phase 4 addition"
  - "test_iron_time_web_symbols uses a single sh -c for-loop over all 9 symbols — one ctest entry, not 9 separate entries, matching Phase 3 style"
  - "test_emsdk_pin_discipline_iron_time_web labeled phase4-invariant (not phase2-invariant) to distinguish the Phase 4 contribution in ctest -L output"
  - "src/stdlib/iron_time.c deliberately NOT added to paths filter — zero-diff invariant means no one should be editing it; adding it would be ironic"
metrics:
  duration: 730s
  completed_date: "2026-04-11"
  tasks_completed: 2
  files_modified: 2
---

# Phase 4 Plan 02: CI Probe and Symbol Gate Summary

**One-liner:** Emcc portability probe extended with `iron_time_web.c` and two new ctest invariants guard symbol presence + emsdk pin discipline on the web time shim.

## What Was Built

Two targeted file edits wiring mechanical CI and local ctest gates over the `iron_time_web.c` file produced in Plan 01.

### `.github/workflows/web.yml` changes

- Renamed the Phase 3 portability probe step from `Probe iron_rc/builtins/collections portability under emcc (WEB-RUNTIME-07)` to `Probe runtime + stdlib files portability under emcc (WEB-RUNTIME-07, WEB-RUNTIME-06)`.
- Added `WEB-RUNTIME-06` echo lines describing the new file before the `emcc -c` block.
- Appended `emcc -c src/stdlib/iron_time_web.c -I src -Wall -Werror -o /tmp/iron_time_web.o` after the three existing Phase 3 `emcc -c` invocations.
- Added `test -s /tmp/iron_time_web.o` guard after the three existing `test -s` checks.
- Updated the final success echo to include `iron_time_web.c`.
- Added `src/stdlib/iron_time_web.c` to BOTH the `push:` and `pull_request:` `paths:` filters (exactly once each — 2 total path filter entries + 1 probe line = 3+ occurrences confirmed by `grep -c`).

### `CMakeLists.txt` changes

Two new `add_test` + `set_tests_properties` blocks inserted directly after the Phase 3 `test_runtime_files_portability_grep` block:

1. **`test_iron_time_web_symbols`** — `sh -c` for-loop greps `iron_time_web.c` for all 9 symbols declared in `iron_time.h` (`Iron_time_now`, `Iron_time_now_ms`, `Iron_time_now_ns`, `Iron_time_sleep`, `Iron_time_since`, `Iron_time_Timer`, `Iron_timer_done`, `Iron_timer_update`, `Iron_timer_reset`). Labeled `unit;web-portability`.

2. **`test_emsdk_pin_discipline_iron_time_web`** — inverted grep `! grep -Fq '4.0.23' iron_time_web.c` enforcing pin discipline on the new file. Labeled `phase4-invariant`.

## Verification Results

- `test_iron_time_web_symbols`: PASSED — all 9 symbols found in Plan 01 baseline
- `test_emsdk_pin_discipline_iron_time_web`: PASSED — no `4.0.23` reference in `iron_time_web.c`
- Full native suite: 65/65 tests passed (`ctest -E benchmark_smoke`)
  - `web-portability` label: 2 tests (Phase 3 + new)
  - `phase4-invariant` label: 1 test (new)
- YAML: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/web.yml'))"` exits 0
- Zero-diff invariant: `git diff --exit-code src/stdlib/iron_time.c src/stdlib/iron_time.h src/stdlib/iron_time_web.c` exits 0

## Commits

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Extend web.yml portability probe with iron_time_web.c + add paths filter | 40b5c43 | .github/workflows/web.yml |
| 2 | Add test_iron_time_web_symbols + test_emsdk_pin_discipline_iron_time_web | 0a3a824 | CMakeLists.txt |

## Deviations from Plan

None — plan executed exactly as written.

## Self-Check: PASSED

- `.github/workflows/web.yml` modified: confirmed (8 insertions)
- `CMakeLists.txt` modified: confirmed (35 insertions)
- Task 1 commit 40b5c43: present
- Task 2 commit 0a3a824: present
- `test_iron_time_web_symbols` passes on Plan 01 baseline: confirmed
- `test_emsdk_pin_discipline_iron_time_web` passes on Plan 01 baseline: confirmed
- 65/65 native tests passing: confirmed
- Zero-diff invariant on native source files: confirmed
- No CMake target compiles `iron_time_web.c`: confirmed
- YAML parses cleanly: confirmed
