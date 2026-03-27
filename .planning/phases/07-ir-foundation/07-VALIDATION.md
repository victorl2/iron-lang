---
phase: 7
slug: ir-foundation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-27
---

# Phase 7 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity (C unit test framework, already in project) |
| **Config file** | CMakeLists.txt (test targets defined here) |
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
| 07-01-01 | 01 | 1 | IRCORE-01 | unit | `ctest --test-dir build -R test_ir_structs` | ❌ W0 | ⬜ pending |
| 07-01-02 | 01 | 1 | IRCORE-02 | unit | `ctest --test-dir build -R test_ir_structs` | ❌ W0 | ⬜ pending |
| 07-01-03 | 01 | 1 | IRCORE-03 | unit | `ctest --test-dir build -R test_ir_structs` | ❌ W0 | ⬜ pending |
| 07-01-04 | 01 | 1 | IRCORE-04 | unit | `ctest --test-dir build -R test_ir_structs` | ❌ W0 | ⬜ pending |
| 07-02-01 | 02 | 1 | TOOL-01 | unit | `ctest --test-dir build -R test_ir_print` | ❌ W0 | ⬜ pending |
| 07-02-02 | 02 | 1 | TOOL-02 | unit | `ctest --test-dir build -R test_ir_verify` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tests/ir/test_ir_structs.c` — stubs for IRCORE-01..04 (module/func/block/instr creation, value IDs, type reuse, span fields)
- [ ] `tests/ir/test_ir_print.c` — stubs for TOOL-01 (printer output verification)
- [ ] `tests/ir/test_ir_verify.c` — stubs for TOOL-02 (verifier pass/fail cases)
- [ ] CMakeLists.txt updated with test_ir_structs, test_ir_print, test_ir_verify targets

*Existing Unity framework covers all phase requirements.*

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
