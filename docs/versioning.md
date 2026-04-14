# Versioning

Phase 70 (VER-01). Declares the canonical release-tag scheme for Iron, the
rationale for the choice, the drift incident that motivated formalizing it,
and the verification path the CI workflow uses to enforce it.

## Canonical Scheme

Iron release tags follow `vMAJOR.MINOR.PATCH-alpha`, where `MAJOR` starts at
`1` and the `-alpha` suffix is dropped only once the first stable `v1.x.y`
ships. The exact regex enforced at CI time is:

```
^v[1-9][0-9]*\.[0-9]+\.[0-9]+(-alpha)?$
```

Public release tags are the source of truth. Planning-doc milestone names
and CMakeLists.txt's `IRON_VERSION_FULL` mirror the latest published tag,
not the other way around.

## Rationale

Public release tags are immutable once published. Renaming `v1.0.0-alpha`
or any later public tag would break every downstream consumer that pulls
release artifacts by tag — release-asset URLs, `git clone --branch`
checkouts, third-party mirrors, and any documentation that links to the
tag's GitHub page. Internal planning-doc names have no such constraint, so
the internal scheme conforms to the public one, never the reverse.

Adopting the older internal `v0.x.y-alpha` scheme was rejected because it
would require renaming the already-published public tags `v1.0.0-alpha`,
`v1.1.0-alpha`, and `v1.2.0-alpha`, which GitHub does not support without
breaking tag-referencing consumers.

## Drift History

Commit `e8b9f7a` (feat: static interface dispatch + collection methods +
layout optimizations + compiler hardening, #11, 2026-04-07) published the
public release `v1.0.0-alpha` directly after the internal milestone
`v0.0.8-alpha`, skipping the intermediate `v0.0.9-alpha` / `v0.1.0-alpha` /
... names entirely. The jump landed without a corresponding planning-doc
rename, so the two schemes diverged at that commit.

The `.planning/` planning documents continued numbering under the internal
scheme (`v0.1.0-alpha`, `v0.1.1-alpha`, `v0.1.2-alpha`, `v0.1.3-alpha`,
`v0.1.4-alpha`) while public releases continued under the new scheme
(`v1.0.0-alpha`, `v1.1.0-alpha`, `v1.2.0-alpha`). By Phase 65 the two
schemes had drifted by a full major version.

The drift went unnoticed because no CI check enforced a canonical regex on
release tags, and no cross-doc link tied planning-doc milestone headings to
CMake `IRON_VERSION_FULL` or to the published release tags. Phase 70 closes
that gap.

## Rules for Future Releases

- **Tag format.** Release tags must match `^v[1-9][0-9]*\.[0-9]+\.[0-9]+(-alpha)?$`.
  CI rejects anything else (see Verification below).
- **Bump rules.** `MAJOR` for breaking changes, `MINOR` for new features,
  `PATCH` for bug fixes (semver). The `-alpha` suffix stays until the first
  stable `v1.x.y` ships; after that, both `vX.Y.Z` and `vX.Y.Z-alpha` remain
  valid tag shapes per the canonical regex.
- **Planning-doc milestone headings.** `.planning/PROJECT.md`,
  `.planning/ROADMAP.md`, and `.planning/REQUIREMENTS.md` must use the same
  scheme for forward-looking references. Historical prose in `last_activity`
  fields and archived `REQUIREMENTS-vX.Y.Z.md` files is NOT retroactively
  rewritten — the archived filename IS the historical name at time of
  archival.
- **CMakeLists.txt sync.** `project(iron VERSION X.Y.Z ...)` and
  `set(IRON_VERSION_FULL "X.Y.Z-alpha")` must match the latest published
  release tag. The `.github/workflows/changelog-on-release.yml` workflow
  (triggered on `release: published`) keeps these in sync automatically via
  `scripts/update_changelog_from_release.py`.
- **Parallel API variant convention.** When a runtime function needs to
  expose both an abort-on-failure legacy signature (for codegen-emitted
  user code and one-shot CLI tools) AND a typed-error variant (for long-
  running services and networked code), introduce the typed-error form as
  a parallel `*_or_error` function with a `Iron_*_OrError` result struct,
  not as a clean-break signature change. The legacy form delegates to the
  new form and aborts on any non-zero error so external callers continue
  to link byte-identically. Phase 71 (HARDEN-01) established this
  convention for the thread-pool creation and submission API — see
  [`docs/runtime-failure-contract.md`](runtime-failure-contract.md). Future
  runtime error-channel work (`iron_string`, `iron_collections`,
  `iron_net.c` caller migration) should follow the same pattern so user
  code can migrate at its own pace without a mega-refactor.

## Verification

### Rejected tag — dry-run

The canonical regex rejects any tag starting with `v0.`, which is exactly
the drift the rule exists to prevent. Run locally with no network and no
`gh` CLI:

```
$ python3 scripts/update_changelog_from_release.py --tag v0.5.0-alpha --dry-run
ERROR: release tag 'v0.5.0-alpha' does not match canonical scheme '^v[1-9][0-9]*\.[0-9]+\.[0-9]+(-alpha)?$'. See docs/versioning.md for the canonical version scheme. Reject this release or delete the tag and re-tag with the correct format.
(exit code 1)
```

The validation runs as the first action in `main()`, before `parse_tag()`
and before `fetch_release()`, so the rejection path does not depend on a
configured `gh` CLI or network connectivity. This is the locally-runnable
portion of the verification flow.

### Accepted tag — dry-run

```
$ python3 scripts/update_changelog_from_release.py --tag v1.4.0-alpha --dry-run
```

The validation passes and the script proceeds to `parse_tag` and
`fetch_release` as normal. The dry-run exits `0` on change or `2` on no-op
depending on whether `CHANGELOG.md` and `CMakeLists.txt` already have
entries for the tag. The happy-path dry-run requires `gh` CLI and network
access to fetch the release body, so it is only fully runnable in a
configured workflow environment — the rejected-tag example above is the
locally-runnable portion.

### Remediation if drift ever re-lands

- **Non-canonical tag reaches `main`.** Delete the tag and re-tag with the
  correct format, then re-dispatch the workflow:
  ```
  git tag -d <tag> && git push origin :refs/tags/<tag>
  git tag -a <correct-tag> -m "..." <sha> && git push origin <correct-tag>
  gh workflow run changelog-on-release.yml --ref main -f tag=<correct-tag>
  ```
- **`CMakeLists.txt` drifts from the latest tag.** Re-dispatch the sync
  workflow and merge the resulting PR, then re-dispatch `release.yml` so the
  release binaries are rebuilt with the correct version string:
  ```
  gh workflow run changelog-on-release.yml --ref main -f tag=<tag>
  gh workflow run release.yml --ref main -f tag=<tag>
  ```
- **Planning-doc milestone headings drift.** Update `.planning/PROJECT.md`,
  `.planning/ROADMAP.md`, and `.planning/REQUIREMENTS.md` on disk. These
  files are not committed per the Phase 67 repo convention — the fix is a
  one-line edit to each affected heading and survives in the local working
  tree.

## Internal-to-Public Milestone Mapping

A reader who encounters old planning docs or commit messages can use the
following table to translate internal milestone names to the public release
tag they correspond to.

| Internal milestone name | Public release tag | Notes |
|---|---|---|
| `v0.0.1-alpha` through `v0.0.8-alpha` | `v0.0.1-alpha` through `v0.0.8-alpha` | First eight internal milestones shipped under matching public tags; no drift yet. |
| (drift gap at commit `e8b9f7a`) | `v1.0.0-alpha` | Jump from `v0.0.8-alpha` to `v1.0.0-alpha` without intermediate `v0.0.9` / `v0.1.0` public tags. |
| `v0.1-alpha` (internal: Static Interface Dispatch) | `v1.0.0-alpha` | Same work, two different names. |
| `v0.1.1-alpha` (internal: Collection Methods, Full Captures, Layout) | rolled into `v1.1.0-alpha` | Later bundled. |
| `v0.1.2-alpha` (internal: Compiler Hardening) | `v1.1.0-alpha` | |
| `v0.1.3-alpha` (internal: Known Limitations Cleanup) | parallel worker, not yet cut | Still in flight on a separate worker. |
| `v0.2.0-alpha` (internal: Networking, paused) | `v1.2.0-alpha` (partial, Phase 59 only) | Phase 59 shipped INFRA-04..10 + NET-01..13 + URL-01..07 as `v1.2.0-alpha` via PR #17; Phases 60-64 still paused. |
| `v0.1.4-alpha` (internal: Compiler Correctness & Maintenance) | `v1.3.0-alpha` (when cut) | Current active milestone; next release under canonical scheme. |

Archived requirements files `.planning/REQUIREMENTS-v0.1.0.md`,
`.planning/REQUIREMENTS-v0.1.x.md`, and `.planning/REQUIREMENTS-v0.2.0.md`
retain their historical internal-scheme filenames on disk. Per the Phase 70
decision, archived filenames are NOT retroactively renamed — the filename
IS the historical name at time of archival.
