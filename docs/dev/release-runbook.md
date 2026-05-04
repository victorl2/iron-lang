# Release Runbook

> Phase 7 Plan 07-08 (HARD-24 + D-16). Step-by-step procedure for cutting
> an iron-lang release. The tag cut itself is human-gated; everything
> else is automated via `.github/workflows/release.yml`.

## v1.2.0-alpha.7 — first Core-Value-gated release

This is the first release cut after the full LSP + editors stack lands.
It is tagged from a `main` SHA where all 8 required status checks
(see `docs/dev/ci-gates.md`) are green, including the new `parity`
blocking gate.

## Prerequisites (one-time per maintainer)

- [ ] Apple Developer Program account + 5 GitHub secrets configured
      per `docs/dev/apple-notarization-setup.md`:
      `APPLE_DEV_ID_APP_CERT`, `APPLE_DEV_ID_APP_CERT_PASSWORD`,
      `APPLE_DEV_ID_APP_IDENTITY`, `APPLE_ID_USERNAME`,
      `APPLE_ID_APP_PASSWORD`, `APPLE_TEAM_ID`.
- [ ] VSCode Marketplace + Zed Registry publisher namespaces
      claimed per `docs/dev/publisher-namespace-checklist.md`.
- [ ] Branch protection configured on `main` per
      `docs/dev/ci-gates.md` — all 8 required status checks wired,
      "Do not allow bypassing the above settings" checked.
- [ ] Local clone has push access to `origin`. Release tags push
      directly to `origin` — do not push tags from a fork.

## Cutting the release (v1.2.0-alpha.7)

### 1. Verify `main` is green

On the GitHub PR page for `main`, the latest commit must show all 8
required checks green:

1. `build-and-test (ubuntu-latest)`
2. `build-and-test (macos-latest)`
3. `vscode-e2e`
4. `neovim-e2e`
5. `zed-package-validate`
6. `parity`
7. `tsan`
8. `slos`

Also verify locally:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIRON_BUILD_LSP=ON
cmake --build build -j4
ctest --test-dir build -L phase-m6-invariant --output-on-failure
ctest --test-dir build -R 'test_parity_ironc_lsp(_fmt)?$' --output-on-failure
bash tests/lsp/invariant/test_phase7_audit.sh
```

All must pass.

### 2. Bump version (done by Plan 07-08 Task 2)

`CMakeLists.txt`:

```
set(IRON_VERSION_FULL "1.2.0-alpha.7")
```

The bump propagates automatically to `iron`, `ironc`, and `ironls`
via the 07-07 plumbing (`IRON_VERSION_STRING` compile-time define +
`ilsp_server_version()` accessor). Verify after rebuild:

```
./build/iron --version
./build/ironc --version
./build/ironls --version
# All three must print: 1.2.0-alpha.7
```

The `test_version_stamp_coherence` ctest invariant enforces this on
every PR; if it passes locally, the three outputs match by construction.

### 3. Update `CHANGELOG.md`

Plan 07-08 Task 2 added the `## v1.2.0-alpha.7` section covering Phase
1-7 highlights. For subsequent releases, append a new section. The
release-notes generation in `.github/workflows/release.yml` reads the
matching CHANGELOG section when building the GitHub Release body.

### 4. Merge the version-bump PR

- Requires: all 8 required checks green.
- Squash-merge into `main`.

### 5. Cut the tag

From a clean local checkout of `main`:

```
git checkout main
git pull --ff-only
git tag -a v1.2.0-alpha.7 -m "Release v1.2.0-alpha.7"
git push origin v1.2.0-alpha.7
```

**Do not force-push tags.** Once published, a tag is immutable —
see rollback section.

### 6. Monitor `release.yml`

The tag push triggers `.github/workflows/release.yml`. Expected duration
is ~30-45 minutes end-to-end, dominated by:

- macOS notarization wait: 5-20 minutes per `ironls` tarball while
  Apple's staple step polls the notarization service.
- Cross-compile matrix: Linux x86_64, Linux aarch64, macOS x86_64,
  macOS arm64.

Watch the run under **Actions → release** for the v1.2.0-alpha.7 tag.

### 7. Verify published assets

On the [GitHub Releases page](https://github.com/iron-lang/iron-lang/releases)
for `v1.2.0-alpha.7`, the following assets must be present:

- 6 Linux tarballs:
  `iron-v1.2.0-alpha.7-linux-x86_64.tar.gz` + `.sha256`
  `iron-v1.2.0-alpha.7-linux-aarch64.tar.gz` + `.sha256`
  `ironc-v1.2.0-alpha.7-linux-x86_64.tar.gz` + `.sha256`
  `ironc-v1.2.0-alpha.7-linux-aarch64.tar.gz` + `.sha256`
  `ironls-v1.2.0-alpha.7-linux-x86_64.tar.gz` + `.sha256`
  `ironls-v1.2.0-alpha.7-linux-aarch64.tar.gz` + `.sha256`
- 6 macOS tarballs (same shape, `macos` in name). The two `ironls`
  macOS tarballs must be signed + notarized + stapled.
- 1 tree-sitter grammar: `iron.wasm`.
- Each `.tar.gz` has a sibling `.sha256` sidecar.

### 8. Verify notarization on a macOS machine

Download one of the macOS ironls tarballs to a Mac and run:

```
tar -xzf ironls-v1.2.0-alpha.7-macos-arm64.tar.gz
codesign --verify --verbose=2 ./ironls     # must report "valid on disk"
spctl --assess --type execute --verbose ./ironls   # must report "accepted"
stapler validate ./ironls                  # must report "stapled and validated"
```

If any of these fail, the notarization pipeline has a bug — do not
announce the release; open an incident.

### 9. Publish extensions manually

Per `docs/dev/publisher-namespace-checklist.md`, the VSCode Marketplace
and Zed Registry publishes are human-gated (CONTEXT D-11). Run the
commands from that document against the tagged source, not the local
`main` branch — reproducibility matters.

### 10. Announce

- Update the project README headline if v1.2.0-alpha.7 is the first
  external-user-facing tag (it is).
- Post to whichever channels the project uses (none yet at alpha.7;
  Discord/Mastodon/HN for v1.2.0 proper).

## Rollback (if a post-release bug is found)

- **Never** rewrite the `v1.2.0-alpha.7` tag. Extensions, package
  managers, and downstream users may already be pinned to the hash.
- Instead: land the fix on `main`, bump to `v1.2.0-alpha.7.1` via a
  new tag, and cut a patch release. Extensions with the pin
  `>= 1.2.0, < 2.0.0` accept the patch without user action.

## Post-v1 increment policy (per D-16)

- `1.2.0-alpha.7 → 1.2.0` (drop alpha suffix) when **all** of:
  - ≥ 3 external users have run ironls for ≥ 24 hours with no crash;
  - ≥ 2 weeks have elapsed since `1.2.0-alpha.7`;
  - No critical bug is open;
  - Zed Linux e2e is green (currently advisory; see CONTEXT D-07).
- Thereafter: semver. MAJOR on breaking LSP contract changes, MINOR
  on new request/notification surface, PATCH on fixes.

## Operating costs to track

- Apple Developer Program: $99/year (HARD-21 unblocks macOS signing).
- GitHub Actions minutes: 8-hour nightly soak × 2 runners + 30-minute
  PR short soak + parity/tsan/slos on every PR. Fits within the
  free-tier allowance for alpha-stage projects, but watch for creep
  as test matrix expands.
