---
phase: 2
slug: semantics-and-codegen
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2025-03-25
---

# Phase 2 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity v2.6.1 (existing from Phase 1) + .iron integration tests |
| **Config file** | CMakeLists.txt (existing) |
| **Quick run command** | `cmake --build build && ctest --test-dir build --output-on-failure` |
| **Full suite command** | `cmake --build build && ctest --test-dir build --output-on-failure -j4` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ctest --test-dir build --output-on-failure`
- **After every plan wave:** Run full suite command
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 02-01-01 | 01 | 1 | SEM-01, SEM-02 | unit | `ctest -R test_analyzer` | ❌ W0 | ⬜ pending |
| 02-01-02 | 01 | 1 | SEM-03, SEM-04, SEM-05 | unit | `ctest -R test_analyzer` | ❌ W0 | ⬜ pending |
| 02-02-01 | 02 | 2 | SEM-06, SEM-07, SEM-08 | unit | `ctest -R test_analyzer` | ❌ W0 | ⬜ pending |
| 02-02-02 | 02 | 2 | SEM-09, SEM-10, SEM-11, SEM-12 | unit | `ctest -R test_analyzer` | ❌ W0 | ⬜ pending |
| 02-03-01 | 03 | 3 | GEN-01, GEN-02, GEN-03, GEN-06, GEN-07 | unit+integration | `ctest -R test_codegen` | ❌ W0 | ⬜ pending |
| 02-03-02 | 03 | 3 | GEN-04, GEN-05, GEN-08, GEN-09, GEN-10, GEN-11 | unit+integration | `ctest -R test_codegen` | ❌ W0 | ⬜ pending |
| 02-04-01 | 04 | 4 | TEST-01, TEST-02 | integration | `ctest --output-on-failure` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `src/analyzer/` directory created with header stubs
- [ ] `src/codegen/` directory created with header stubs
- [ ] `tests/test_analyzer.c` — Unity test stubs for SEM-01..12
- [ ] `tests/test_codegen.c` — Unity test stubs for GEN-01..11
- [ ] `tests/integration/` — .iron + .expected file pairs for end-to-end tests

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Generated C readability | GEN-07 | Subjective formatting quality | Inspect generated .c output with --verbose flag |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
