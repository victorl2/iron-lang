---
phase: 02-cli-toml-scaffold
plan: "02"
subsystem: cli/toml
tags: [toml, parser, web-config, levenshtein, assets]
dependency_graph:
  requires:
    - "02-01"  # IronWebConfig struct + IronProject.web field landed in Plan 01
  provides:
    - "src/cli/toml.c section==3 branch"
    - "Levenshtein misspelled-section detection"
    - "assets normalization to char**+count"
    - "iron_toml_free web-field release"
  affects:
    - "02-06"  # test plan reads from proj->web — all fields must be populated
    - "02-03"  # main.c dispatch reads IronBuildOpts; toml.c provides proj->web
    - "02-04"  # build_web.c consumes proj->web fields
tech_stack:
  added: []
  patterns:
    - "Single-row rolling DP Levenshtein (stack-only, 64-char cap)"
    - "TOML array normalization: both string and array forms map to char**+int"
    - "Section integer: 0=none, 1=package/project, 2=dependencies, 3=web"
key_files:
  created: []
  modified:
    - src/cli/toml.c
decisions:
  - "Levenshtein cap at 64 chars returns INT_MAX — unreasonable for section names, avoids variable-length stack array"
  - "parse_toml_string_array() kept static, not exposed in header — single consumer (assets branch)"
  - "go-to label (goto fail) used in parse_toml_string_array for clean partial-alloc cleanup — matches C idiom for multi-alloc error paths"
  - "val_str trimming before array-form check done inline rather than calling trim() — trim() is mutable, val_str is used later by the scalar branch"
metrics:
  duration: "2m 6s"
  completed: "2026-04-11"
  tasks_completed: 3
  tasks_planned: 3
  files_modified: 1
---

# Phase 02 Plan 02: TOML [web] Section Parsing Summary

**One-liner:** Extended `src/cli/toml.c` with a `section==3` [web] parser branch, Levenshtein misspelled-section typo detection, 6-field parsing including dual-form assets normalization, and `iron_toml_free` web-field release — satisfying all 8 WEB-MANIFEST requirements.

## Tasks Completed

| # | Name | Commit | Files |
|---|------|--------|-------|
| 1 | Add Levenshtein helper + known-sections array + section==3 dispatch | 5beac2e | src/cli/toml.c |
| 2 | Add section==3 [web] parser branch with 6 fields and assets normalization | 8133272 | src/cli/toml.c |
| 3 | Extend iron_toml_free to release IronWebConfig heap fields | 87ae4ec | src/cli/toml.c |

## What Was Built

**Levenshtein misspelled-section detection (Task 1):**
- Static `levenshtein()` helper: single-row rolling DP, stack arrays `v0[66]/v1[66]`, 64-char input cap returning `INT_MAX` for unreasonably long names
- `KNOWN_SECTIONS[]` array: `{"package", "project", "dependencies", "web", NULL}`
- Section-header dispatch updated: `[web]` sets `section = 3`; unknown sections within Levenshtein distance ≤2 emit `warning: unknown section [X] — did you mean [Y]? (ignored)` to stderr; distance >2 is silently ignored
- `#include <limits.h>` added for `INT_MAX`

**[web] parser branch (Task 2):**
- Static `parse_toml_string_array()` helper: parses `["a.png", "b.png"]` form into a growing realloc'd `char**`; `goto fail` for clean partial-alloc cleanup
- `section == 3` branch after existing `section == 2` block parsing all 6 known fields:
  - `title`, `shell`: free-before-assign string fields via `extract_value()`
  - `initial_memory`, `stack_size`, `pthread_pool_size`: `strtol`-parsed with positive-integer guard; warn and ignore on invalid values
  - `assets`: last-write-wins; array form dispatches to `parse_toml_string_array()`; scalar string form normalized to one-element `char*[1]` array; both produce `char **assets + int asset_count`
- Unknown `[web].*` keys emit `warning: unknown [web] key '<name>' (ignored)` and continue without failing

**iron_toml_free extension (Task 3):**
- Added `free(proj->web.title)`, `free(proj->web.shell)`, element-wise `free(proj->web.assets[wi])` loop, then `free(proj->web.assets)` — placed before the existing `free(proj)` call
- Integer fields (`initial_memory`, `stack_size`, `pthread_pool_size`, `asset_count`) are value-embedded on `IronWebConfig`; no free needed

## Requirements Addressed

| Requirement | Status | Notes |
|-------------|--------|-------|
| WEB-MANIFEST-01 | Done | [web] parses into IronProject.web via proj->web writes |
| WEB-MANIFEST-02 | Done | assets accepts string or array, normalized to char**+count |
| WEB-MANIFEST-03 | Done | title parsed as malloc'd string |
| WEB-MANIFEST-04 | Done | shell parsed as malloc'd string |
| WEB-MANIFEST-05 | Done | initial_memory parsed as positive int |
| WEB-MANIFEST-06 | Done | stack_size parsed as positive int |
| WEB-MANIFEST-07 | Done | pthread_pool_size parsed as positive int |
| WEB-MANIFEST-08 | Done | unknown [web] keys warn, do not fail parsing |
| Pitfall 10 | Done | Levenshtein ≤2 misspelled-section detection |

## Verification

- `grep -c "section ==" src/cli/toml.c` → 3 (section==1, 2, 3 all present)
- `! grep -Fq "4.0.23" src/cli/toml.c` → passes (no pin hardcoded)
- `cmake --build build` → succeeds (all targets built cleanly)
- All per-task acceptance criteria checked via grep before each commit

## Deviations from Plan

None — plan executed exactly as written.

## Self-Check: PASSED

- `/Users/victor/code/worker-3/iron-lang/src/cli/toml.c` exists and contains all required patterns
- Commits 5beac2e, 8133272, 87ae4ec all present in git log
- cmake --build succeeded with 0 errors
