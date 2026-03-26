---
phase: 5
slug: codegen-fixes-stdlib-wiring
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-26
---

# Phase 5 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity (C unit tests) + shell-based integration tests |
| **Config file** | `CMakeLists.txt` (Unity via FetchContent) |
| **Quick run command** | `cd build && ctest --output-on-failure -R "test_codegen\|test_interp\|test_parallel"` |
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
| 05-01-01 | 01 | 1 | GEN-01 | unit | `ctest -R test_codegen` | ❌ W0 | ⬜ pending |
| 05-01-02 | 01 | 1 | GEN-01 | integration | `bash run_integration.sh` | ❌ W0 | ⬜ pending |
| 05-02-01 | 02 | 1 | GEN-11 | unit | `ctest -R test_parallel` | ❌ W0 | ⬜ pending |
| 05-02-02 | 02 | 1 | GEN-11 | integration | `bash run_integration.sh` | ❌ W0 | ⬜ pending |
| 05-03-01 | 03 | 2 | STD-01 | integration | `bash run_integration.sh` | ❌ W0 | ⬜ pending |
| 05-03-02 | 03 | 2 | STD-02 | integration | `bash run_integration.sh` | ❌ W0 | ⬜ pending |
| 05-03-03 | 03 | 2 | STD-03 | integration | `bash run_integration.sh` | ❌ W0 | ⬜ pending |
| 05-03-04 | 03 | 2 | STD-04 | integration | `bash run_integration.sh` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tests/unit/test_interp_codegen.c` — C unit tests for string interpolation emission
- [ ] `tests/unit/test_parallel_codegen.c` — C unit tests for parallel-for chunk generation
- [ ] `tests/integration/interp_string/` — .iron + .expected for interpolation
- [ ] `tests/integration/parallel_for/` — .iron + .expected for parallel-for
- [ ] `tests/integration/stdlib_math/` — .iron + .expected for Math module
- [ ] `tests/integration/stdlib_io/` — .iron + .expected for IO module
- [ ] `tests/integration/stdlib_time/` — .iron + .expected for Time module
- [ ] `tests/integration/stdlib_log/` — .iron + .expected for Log module

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Parallel-for distributes work across threads | GEN-11 | Thread scheduling is non-deterministic | Run parallel_for test multiple times; verify aggregate result is always correct |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
