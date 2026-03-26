---
phase: 3
slug: runtime-stdlib-and-cli
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2025-03-26
---

# Phase 3 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity v2.6.1 (existing) + .iron integration tests + clang compilation gate |
| **Config file** | CMakeLists.txt (existing) |
| **Quick run command** | `cmake --build build && ctest --test-dir build --output-on-failure` |
| **Full suite command** | `cmake --build build && ctest --test-dir build --output-on-failure -j4` |
| **Estimated runtime** | ~20 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ctest --test-dir build --output-on-failure`
- **After every plan wave:** Run full suite command
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 20 seconds

---

## Wave 0 Requirements

- [ ] `src/runtime/` directory with header stubs
- [ ] `src/stdlib/` directory with header stubs
- [ ] `src/cli/` directory with main.c stub
- [ ] `tests/test_runtime.c` — Unity test stubs for RT-01..08
- [ ] `tests/test_stdlib.c` — Unity test stubs for STD-01..04
- [ ] Integration tests that compile .iron to binary and run

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| CLI colored output | CLI-07 | Requires terminal inspection | Run `iron build` with errors, verify ANSI colors in terminal |
| GitHub Actions CI | TEST-04 | Requires push to remote | Push branch, verify Actions workflow runs green |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 20s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
