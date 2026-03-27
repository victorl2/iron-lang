---
phase: 8
slug: ast-to-ir-lowering
status: draft
nyquist_compliant: true
wave_0_complete: true
created: 2026-03-27
---

# Phase 8 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity (C unit tests) + ctest |
| **Config file** | CMakeLists.txt (test targets) |
| **Quick run command** | `ctest --test-dir build -R test_ir` |
| **Full suite command** | `ctest --test-dir build` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `ctest --test-dir build -R test_ir`
- **After every plan wave:** Run `ctest --test-dir build`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 08-01-00 | 01 | 1 | (Wave 0) | scaffold | `ctest --test-dir build -R test_ir_lower` | W0 creates | pending |
| 08-01-01 | 01 | 1 | INSTR-01..04 | unit | `ctest --test-dir build -R test_ir_lower` | via W0 | pending |
| 08-01-02 | 01 | 1 | INSTR-05..13 | unit | `ctest --test-dir build -R test_ir_lower` | via W0 | pending |
| 08-01-03 | 01 | 1 | INSTR-01..13 | unit | `ctest --test-dir build -R test_ir_lower` | via W0 | pending |
| 08-02-01 | 02 | 2 | CTRL-01..04 | unit | `ctest --test-dir build -R test_ir_lower` | via W0 | pending |
| 08-02-02 | 02 | 2 | MEM-01..04 | unit+snapshot | `ctest --test-dir build -R test_ir_lower` | via W0 | pending |
| 08-03-01 | 03 | 3 | CONC-01..04 | unit | `ctest --test-dir build -R test_ir_lower` | via W0 | pending |
| 08-03-02 | 03 | 3 | MOD-01..04 | unit+snapshot | `ctest --test-dir build -R test_ir` | via W0 | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

- [x] `tests/ir/test_ir_lower.c` — created by Plan 08-01 Task 0 (scaffold with placeholder test + snapshot helper)
- [x] `tests/ir/snapshots/` — created by Plan 08-01 Task 0 (directory with placeholder .expected files)
- [x] CMakeLists.txt updated with test_ir_lower target — done in Plan 08-01 Task 0

*Wave 0 is satisfied by Plan 08-01 Task 0, which runs before any implementation tasks.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Draw keyword removal | MOD-04 | Parser change affects existing .iron files | Verify `draw { }` produces parse error; `raylib.draw(|| { })` compiles |

---

## Validation Sign-Off

- [x] All tasks have `<automated>` verify or Wave 0 dependencies
- [x] Sampling continuity: no 3 consecutive tasks without automated verify
- [x] Wave 0 covers all MISSING references
- [x] No watch-mode flags
- [x] Feedback latency < 5s
- [x] `nyquist_compliant: true` set in frontmatter

**Approval:** approved (post-revision)
