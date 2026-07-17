# v3-archive — Archived v3-era integration corpus

This directory contains the 388 root-level `.iron` + `.expected` fixtures
(plus 8 `.disabled` siblings) that lived at `tests/integration/*.iron` prior
to the Iron v4 migration (Phase 35 MIG-08, 2026-05-31). Every file was moved
via `git mv`, so `git log --follow tests/integration/v3-archive/<fixture>.iron`
reveals the pre-archive history (R100 rename detection succeeds).

## Why archived?

The v4 memory model (val/var discipline, heap/rc/weak rc policies,
`*T` / `*var T` pointers, `[T; <=N]` bounded vectors, drop/copy/nocopy
resource types, `defer`, arena allocation) is a clean syntactic and semantic
break from v3. Per the milestone decision (see `.planning/ROADMAP.md` Phase
35 and `.planning/phases/35-grammar-extension-catchup-corpus-migration/35-CONTEXT.md`),
there is NO `ironc migrate` tool this milestone. Fixtures from v3 that have
a v4 analog will be hand-migrated into `tests/integration/v4/` (Wave 2 of
Phase 35); fixtures without a v4 analog stay only here.

## Opt-in invocation

The archived corpus does NOT run on default `ctest` invocations. The CTest
entry `test_integration_v3_archive` (label `v3-archive;archeology`) is only
registered when the cache variable `IRON_RUN_ARCHIVED_V3_CORPUS` is `ON`:

```bash
# Default build: archive excluded
cmake -S . -B build -G Ninja
ctest --test-dir build -N | grep v3_archive   # returns nothing

# Opt-in build: archive included
cmake -S . -B build-archive -G Ninja -DIRON_RUN_ARCHIVED_V3_CORPUS=ON
cmake --build build-archive
ctest --test-dir build-archive -R test_integration_v3_archive --output-on-failure
# or, equivalently:
ctest --test-dir build-archive -L v3-archive --output-on-failure
```

v3 fixtures are **NOT expected to pass** under the v4 compiler — this run
is for archeology / diff purposes only. The harness will report large
`FAIL` counts; that is the point.

## Why preserved instead of deleted?

Git history alone would suffice to recover the files, but having them
in-tree under an opt-in gate makes it cheap to run regression diffs when
v4 behavior on a v3-style construct is unclear, and avoids the friction
of `git show <pre-archive-sha>:tests/integration/<fixture>.iron`
incantations during Phase 35 Wave 2 hand-migration.

## Phase 15 acceptance corpus

The Phase 15 TDD acceptance corpus (TDD-01..TDD-08) is NOT in this archive
— it was authored directly under `tests/integration/v4/` in feature-area
subdirectories (`3.1-stack/`, `3.2-heap/`, ..., `8.7-composition-mixing/`).
See `tests/integration/v4/v15-acceptance/README.md` for the full mapping.
The Phase 15 corpus runs as part of the v4 default ctest, not via the
opt-in archive gate.
