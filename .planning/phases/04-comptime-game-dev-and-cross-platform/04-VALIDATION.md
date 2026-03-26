---
phase: 4
slug: comptime-game-dev-and-cross-platform
status: active
nyquist_compliant: true
wave_0_complete: true
created: 2025-03-26
---

# Phase 4 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity v2.6.1 (existing) + .iron integration tests + iron CLI end-to-end |
| **Config file** | CMakeLists.txt (existing) |
| **Quick run command** | `cmake --build build && ctest --test-dir build --output-on-failure` |
| **Full suite command** | `cmake --build build && ctest --test-dir build --output-on-failure -j4` |
| **Estimated runtime** | ~25 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ctest --test-dir build --output-on-failure`
- **After every plan wave:** Run full suite command
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 25 seconds

---

## Wave 0 Requirements

N/A — plans create all artifacts directly. Each plan's first task creates the necessary files from scratch.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Raylib window opens | GAME-01 | Requires display/GPU | Run example game.iron, verify window opens with graphics |
| Windows CI green | GAME-04/RT-08 | Requires Windows runner | Push to trigger CI, verify windows-latest passes |

---

## Validation Sign-Off

- [x] All tasks have `<automated>` verify or Wave 0 dependencies
- [x] Sampling continuity: no 3 consecutive tasks without automated verify
- [x] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 25s
- [x] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
