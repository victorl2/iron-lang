---
phase: 6
slug: milestone-gap-closure
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-26
---

# Phase 6 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity (C unit tests) + shell-based integration tests |
| **Config file** | `CMakeLists.txt` (Unity via FetchContent) |
| **Quick run command** | `cd build && ctest --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure && bash ../tests/integration/run_integration.sh ../build/iron` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --output-on-failure`
- **After every plan wave:** Run full suite + integration tests
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 06-01-01 | 01 | 1 | RT-07 | integration | `bash run_integration.sh` | Will create | pending |
| 06-01-02 | 01 | 1 | STD-03 | integration | `bash run_integration.sh` | Will create | pending |
| 06-01-03 | 01 | 1 | CLI-03 | integration | `iron check test_file.iron` | Will create | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. No new test framework needed.

---

## Manual-Only Verifications

All phase behaviors have automated verification.

---

## Validation Sign-Off

- [ ] All tasks have automated verify
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
