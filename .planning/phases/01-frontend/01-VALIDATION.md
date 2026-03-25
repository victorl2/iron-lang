---
phase: 1
slug: frontend
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2025-03-25
---

# Phase 1 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Unity v2.6.1 (C unit test framework via CMake FetchContent) |
| **Config file** | CMakeLists.txt (Wave 0 installs) |
| **Quick run command** | `cmake --build build && ctest --test-dir build --output-on-failure` |
| **Full suite command** | `cmake --build build && ctest --test-dir build --output-on-failure -j4` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ctest --test-dir build --output-on-failure`
- **After every plan wave:** Run full suite command
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 10 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 01-01-01 | 01 | 1 | LEX-01 | unit | `ctest -R test_lexer` | ❌ W0 | ⬜ pending |
| 01-01-02 | 01 | 1 | LEX-02 | unit | `ctest -R test_lexer` | ❌ W0 | ⬜ pending |
| 01-01-03 | 01 | 1 | LEX-03 | unit | `ctest -R test_lexer` | ❌ W0 | ⬜ pending |
| 01-01-04 | 01 | 1 | LEX-04 | unit | `ctest -R test_lexer` | ❌ W0 | ⬜ pending |
| 01-02-01 | 02 | 1 | PARSE-01 | unit | `ctest -R test_parser` | ❌ W0 | ⬜ pending |
| 01-02-02 | 02 | 1 | PARSE-02 | unit | `ctest -R test_parser` | ❌ W0 | ⬜ pending |
| 01-02-03 | 02 | 1 | PARSE-03 | unit | `ctest -R test_parser` | ❌ W0 | ⬜ pending |
| 01-02-04 | 02 | 1 | PARSE-04 | unit | `ctest -R test_parser` | ❌ W0 | ⬜ pending |
| 01-02-05 | 02 | 1 | PARSE-05 | unit | `ctest -R test_parser` | ❌ W0 | ⬜ pending |
| 01-03-01 | 03 | 1 | TEST-03 | integration | `ctest -R test_diagnostics` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `CMakeLists.txt` — project build config with Unity FetchContent, test targets
- [ ] `tests/test_lexer.c` — Unity test stubs for LEX-01..04
- [ ] `tests/test_parser.c` — Unity test stubs for PARSE-01..05
- [ ] `tests/test_diagnostics.c` — test stubs for TEST-03 (error code + line matching)
- [ ] Arena allocator (`src/util/arena.h`, `src/util/arena.c`) — shared infrastructure

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Pretty-printer output readability | PARSE-05 | Subjective formatting quality | Run pretty-printer on example .iron files, verify output is readable |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 10s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
