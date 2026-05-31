# v15-acceptance — Phase 15 TDD Acceptance Corpus marker

The Phase 15 TDD acceptance corpus (TDD-01..TDD-08) was authored directly
into the `tests/integration/v4/` tree under feature-area subdirectories
named after the v4 spec sections (§3-§8). There is no separate
`v15-acceptance/` source directory to symlink — the corpus is co-located
with the rest of the v4 integration fixtures.

## Phase 15 corpus → v4 subdir map

| Spec section            | Subdirectory                                              |
| ----------------------- | --------------------------------------------------------- |
| §3.1 stack              | `tests/integration/v4/3.1-stack/`                         |
| §3.2 heap               | `tests/integration/v4/3.2-heap/`                          |
| §3.3 rc                 | `tests/integration/v4/3.3-rc/`                            |
| §3.4 weak rc            | `tests/integration/v4/3.4-weak-rc/`                       |
| §3.6 non-transitive     | `tests/integration/v4/3.6-non-transitive/`                |
| §3.7 arena              | `tests/integration/v4/3.7-arena/`                         |
| §4.2 checked-ptr        | `tests/integration/v4/4.2-checked-ptr/`                   |
| §4.3 unchecked-ptr      | `tests/integration/v4/4.3-unchecked-ptr/`                 |
| §4.4 readonly           | `tests/integration/v4/4.4-readonly/`, `4.4-regime-isolation/` |
| §4.5 bounded-vector     | `tests/integration/v4/4.5-bounded-vector/`                |
| §4.8 rc-policy          | `tests/integration/v4/4.8-rc-policy/`                     |
| §4.9 weak-rc-policy     | `tests/integration/v4/4.9-weak-rc-policy/`                |
| §4.10 rc-elision        | `tests/integration/v4/4.10-rc-elision/`                   |
| §4.11 ptr-check-elision | `tests/integration/v4/4.11-ptr-check-elision/`            |
| §4.12 debug-leak        | `tests/integration/v4/4.12-debug-leak/`                   |
| §4.13 defer             | `tests/integration/v4/4.13-defer/`                        |
| §5 mutability           | `tests/integration/v4/5-mutability/`                      |
| §6 readonly             | `tests/integration/v4/6-readonly/`                        |
| §7 drop-copy            | `tests/integration/v4/7-drop-copy/`                       |
| §7.5 stdlib             | `tests/integration/v4/7.5-stdlib/`                        |
| §8.1-8.7 composition    | `tests/integration/v4/8.1-composition-default/`, `8.2-composition-heap/`, `8.3-composition-rc/`, `8.4-composition-weak-rc/`, `8.5-composition-box/`, `8.6-composition-arena/`, `8.7-composition-mixing/` |
| §10 tooling             | `tests/integration/v4/10-tooling/`                        |

## How the Phase 15 corpus runs

The Phase 15 corpus is ALREADY incorporated into the v4 default CTest run
(see `tests/integration/v4/CMakeLists.txt` → `test_v4_acceptance` with
label `v4-acceptance`). It runs on every default `ctest` invocation; no
opt-in flag is needed.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build -L v4-acceptance --output-on-failure
```

## Why this marker file exists

This README satisfies the MIG-10 acceptance criterion ("Phase 15 acceptance
corpus wired into `tests/integration/v4/v15-acceptance/`") as a
documentation-only marker — no symlink is needed because the corpus is
co-located with the rest of the v4 integration fixtures and would create
a circular/redundant link. Wave 2 of Phase 35 will hand-migrate v3 fixtures
into these same subdirs, extending (not replacing) the Phase 15 baseline.
