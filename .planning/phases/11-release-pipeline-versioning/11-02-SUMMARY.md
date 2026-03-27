---
phase: 11-release-pipeline-versioning
plan: 02
subsystem: infra
tags: [github-actions, release, ci-cd, install-script, cmake, cross-platform]

# Dependency graph
requires:
  - phase: 11-release-pipeline-versioning-01
    provides: "CMakeLists.txt with project(VERSION 0.1.1), git hash baking, iron --version output"
provides:
  - "GitHub Actions release workflow building iron for 4 platform targets"
  - "curl-pipe-sh install script installing iron to ~/.iron/bin"
affects: [release-process, user-onboarding]

# Tech tracking
tech-stack:
  added: [softprops/action-gh-release@v2]
  patterns:
    - "Test gate: test job must pass before build matrix runs"
    - "continue-on-error: true on Windows build job (stretch goal)"
    - "Archive naming: iron-{version}-{os}-{arch}.{ext} with v-prefix stripped"
    - "Install dir: ~/.iron/bin with idempotent PATH update to ~/.bashrc and ~/.zshrc"

key-files:
  created:
    - .github/workflows/release.yml
    - scripts/install.sh
  modified: []

key-decisions:
  - "Release triggered on types: [created] — fires on both draft and published releases"
  - "macOS arm64 uses macos-latest (Apple Silicon runners), macOS x86_64 uses macos-13 (Intel)"
  - "softprops/action-gh-release@v2 for asset upload — standard, well-maintained action"
  - "install.sh uses /bin/sh not bash for maximum curl-pipe-sh portability"
  - "PATH update is idempotent — only writes if .iron/bin not already in profile"

patterns-established:
  - "Release workflow: test gate -> build matrix -> upload-install-script as independent jobs"
  - "Unix packaging: staging/ dir, copy binary + LICENSE, tar -czf from staging"
  - "Windows packaging: PowerShell Compress-Archive in pwsh shell step"

requirements-completed: [REL-01, REL-02]

# Metrics
duration: 2min
completed: 2026-03-27
---

# Phase 11 Plan 02: Release Pipeline Summary

**GitHub Actions release workflow builds iron for 4 platform targets (linux-x86_64, macos-arm64, macos-x86_64, windows-x86_64) behind a test gate, and curl-pipe-sh install.sh installs to ~/.iron/bin**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-27T22:38:49Z
- **Completed:** 2026-03-27T22:40:21Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Release workflow with test gate (full unit + integration suite on Linux before any build starts)
- Cross-platform build matrix: ubuntu-latest, macos-latest (arm64), macos-13 (x86_64), windows-latest
- Windows is best-effort with continue-on-error, Unix uses clang + Ninja + CMAKE_BUILD_TYPE=Release
- Archives named `iron-{version}-{target}.tar.gz` (Unix) or `.zip` (Windows) uploaded via softprops/action-gh-release@v2
- Portable /bin/sh install script: detects OS/arch, fetches latest GitHub release, installs to ~/.iron/bin

## Task Commits

Each task was committed atomically:

1. **Task 1: Create GitHub Actions release workflow** - `d7dee7f` (feat)
2. **Task 2: Create install.sh script for curl-pipe-sh installation** - `be73bbd` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified
- `.github/workflows/release.yml` - Release CI: test gate + 4-platform build matrix + install script upload
- `scripts/install.sh` - curl-pipe-sh installer detecting OS/arch, downloading from GitHub releases

## Decisions Made
- Triggered on `types: [created]` so draft releases also trigger the workflow (matches user decision captured in plan)
- Used `softprops/action-gh-release@v2` as the standard action for uploading release assets
- `install.sh` uses `/bin/sh` (not bash) for widest shell compatibility in curl-pipe-sh pattern
- PATH update checks `.iron/bin` presence before writing — idempotent, safe to re-run

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required. The release workflow will activate automatically when a GitHub release is created in the `victorl2/iron-lang` repository.

## Next Phase Readiness

- Phase 11 complete: version baking (11-01) and release pipeline (11-02) are both done
- To publish a release: create a GitHub release (draft or published) with a tag like `v0.1.1`
- Users can install via: `curl -sSfL https://github.com/victorl2/iron-lang/releases/latest/download/install.sh | sh`

---
*Phase: 11-release-pipeline-versioning*
*Completed: 2026-03-27*
