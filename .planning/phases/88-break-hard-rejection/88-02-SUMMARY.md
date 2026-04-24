---
phase: 88-break-hard-rejection
plan: "02"
subsystem: tests
tags: [compile_fail, fixtures, v3, strict-mode, TEST-14, BREAK-03, BREAK-04]

# Dependency graph
requires:
  - phase: 88-01
    provides: E0260..E0264 error codes, --strict-v3 CLI flag, v3_strict_mode gate
provides:
  - compile_fail fixture pair for TEST-14 (v3_receiver_method_removed)
  - compile_fail fixture pair for BREAK-03 (v3_inline_default_removed)
  - compile_fail fixture pair for BREAK-04 (v3_mut_keyword_removed)
affects: [88-03]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Compile_fail fixture shape: .iron source + .expected substring locked via grep -qF"
    - "Gate-ON: ironc check --strict-v3 triggers BREAK errors; Gate-OFF: same files parse clean"
    - "TEST-14 fixture uses read-only receiver body to avoid pre-existing E0234 in gate-OFF mode"

key-files:
  created:
    - tests/compile_fail/v3_receiver_method_removed.iron
    - tests/compile_fail/v3_receiver_method_removed.expected
    - tests/compile_fail/v3_mut_keyword_removed.iron
    - tests/compile_fail/v3_mut_keyword_removed.expected
    - tests/compile_fail/v3_inline_default_removed.iron
    - tests/compile_fail/v3_inline_default_removed.expected
  modified: []

key-decisions:
  - "TEST-14 receiver body uses read-only return (no field mutation) to prevent E0234 firing in gate-OFF path"
  - "BREAK-03 fixture includes a valid v3 object (Config) alongside the invalid one (Legacy) to confirm gate only fires on the = expr field"

patterns-established:
  - "Locked substring for migration hint: 'ironc migrate --from v2 --to v3' (E0260)"
  - "Locked substring for mut removal: \"'mut' keyword removed\" (E0263)"
  - "Locked substring for inline defaults: 'inline field defaults' (E0262)"

requirements-completed: [TEST-14, BREAK-05]

# Metrics
duration: 8min
completed: 2026-04-23
---

# Phase 88 Plan 02: Compile_fail Fixtures for TEST-14, BREAK-03, BREAK-04 Summary

**Six fixture files locking E0260/E0262/E0263 diagnostic text; gate-OFF path holds; all 9 pre-existing compile_fail fixtures and 106/41/171 unit tests green**

## Performance

- **Duration:** 8 min
- **Started:** 2026-04-23T22:35:13Z
- **Completed:** 2026-04-23T22:43:00Z
- **Tasks:** 2 (+ 1 auto-fix for gate-OFF E0234 collision)
- **Files modified:** 6 created

## Accomplishments

- Created `v3_receiver_method_removed.iron/.expected` (TEST-14): locks E0260 migration hint substring
- Created `v3_mut_keyword_removed.iron/.expected` (BREAK-04): locks E0263 "'mut' keyword removed" substring
- Created `v3_inline_default_removed.iron/.expected` (BREAK-03): locks E0262 "inline field defaults" substring
- Gate-ON verified on server: all three COMPILE_FAIL_OK
- Gate-OFF verified on server: GATE_OFF_OK for receiver-method fixture
- All 9 pre-existing compile_fail fixtures pass (gate OFF, zero regressions)
- Unit tests: test_lexer PASSED, test_parser PASSED, test_typecheck PASSED (41/106/171)
- Integration count: 395 (unchanged)

## Task Commits

1. **Task 1: Six fixture files** - `3dfc33c` (test)
2. **Auto-fix: gate-OFF body** - `6e3302a` (fix)

Task 2 (regression verification) required no file changes; results documented here.

## Verification Evidence

### Gate-ON (--strict-v3 active)
```
COMPILE_FAIL_OK: v3_receiver_method_removed
COMPILE_FAIL_OK: v3_mut_keyword_removed
COMPILE_FAIL_OK: v3_inline_default_removed
```

### Gate-OFF (no --strict-v3)
```
GATE_OFF_OK: v3_receiver_method_removed
```

### Unit tests (server, ctest)
```
test_lexer   PASSED
test_parser  PASSED
test_typecheck PASSED
100% tests passed, 0 tests failed out of 3
```

### Pre-existing compile_fail (gate OFF)
```
COMPILE_FAIL_OK: v3_readonly_write_self
COMPILE_FAIL_OK: v3_pure_io
COMPILE_FAIL_OK: v3_init_unassigned_field
COMPILE_FAIL_OK: v3_patch_conflict
COMPILE_FAIL_OK: v3_patch_adds_field
COMPILE_FAIL_OK: v3_iface_tier_mismatch
COMPILE_FAIL_OK: v3_iface_init_rejected
COMPILE_FAIL_OK: v3_iface_missing_method
COMPILE_FAIL_OK: v3_self_outside_method
```

### Integration count
```
395
```

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Gate-OFF path triggered E0234 (immutable receiver mutation)**
- **Found during:** Task 1 gate-OFF verification
- **Issue:** Original plan fixture body had `p.health = p.health - n` which triggers E0234 (mut field on val receiver) even without --strict-v3; GATE_OFF_FAIL was printed
- **Fix:** Replaced the receiver method body with a read-only return expression `return p.health` and renamed the method to `current_health()` to avoid any mutation in gate-OFF mode
- **Files modified:** tests/compile_fail/v3_receiver_method_removed.iron
- **Commit:** 6e3302a

---

**Total deviations:** 1 auto-fixed (gate-OFF collision with pre-existing type error)
**Impact on plan:** No scope creep; gate-ON behavior unchanged, gate-OFF now correctly clean.

## Notes

- `test_parser_errors` binary was absent during ctest run (pre-existing unbuilt target, unrelated to Phase 88). The three canonical unit test binaries (test_lexer, test_parser, test_typecheck) all passed.

---
*Phase: 88-break-hard-rejection*
*Completed: 2026-04-23*
