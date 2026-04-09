---
phase: 55-push-on-interface-arrays
verified: 2026-04-09T15:45:00Z
status: passed
resolution: 2026-04-09T15:50:00Z
resolution_note: >
  Both gaps resolved. Cosmetic gap (unticked checkboxes) fixed directly in ROADMAP.md.
  Substantive gap (empty-literal type inference for `var shapes: [Shape] = []`) identified
  as orthogonal to the push dispatch fix and isolated into new decimal Phase 55.1 with
  EMPTY-LIT-01/02 requirements. ROADMAP SC1 wording updated to reference Phase 55.1 for
  the empty-literal portion. Phase 55 itself delivers the full push/len/pop/get/set
  broad-scope fix as locked in CONTEXT.md.
score: 4/5 → 5/5 (SC1 scope clarified, empty-literal tracked separately)
gaps:
  - truth: "var shapes: [Shape] = []; shapes.push(Circle(5)) compiles without errors (ROADMAP SC1)"
    status: resolved
    reason: >
      Empty interface array literal `[]` fails type inference with
      E0202: type mismatch expected '[<interface>]', got '[<error>]'.
      The PLAN had an escape hatch ("if empty interface literal is
      supported; otherwise surface as blocker") but neither any SUMMARY,
      nor any commit message, nor any known-limitations list documents
      this as a surfaced blocker or deferred limitation.
      All tests work around it by starting with a 2-element literal.
    artifacts:
      - path: "tests/integration/push_interface_collection.iron"
        issue: "Uses [Circle(1), Square(2)] initial literal, not []. Workaround hides the gap."
    missing:
      - "Either: test the empty-literal path and verify it compiles (may require a separate fix)"
      - "Or: explicitly document `var shapes: [Shape] = []` as a known limitation in REQUIREMENTS.md
         or a dedicated KNOWN_LIMITATIONS doc, referencing it as out-of-scope for Phase 55"
      - "Update ROADMAP SC1 to reflect what was actually delivered, or add a tracking item for empty-literal support"
  - truth: "ROADMAP plan checkboxes for 55-02 and 55-03 show complete"
    status: resolved
    reason: >
      ROADMAP.md lines 1120-1121 still show `- [ ] 55-02-PLAN.md` and
      `- [ ] 55-03-PLAN.md`. Both plans have SUMMARY.md files confirming
      they are complete, so this is a cosmetic tracking inconsistency
      rather than a code gap. Low severity.
    artifacts:
      - path: ".planning/ROADMAP.md"
        issue: "Lines 1120-1121: plan checkboxes not ticked for 55-02 and 55-03"
    missing:
      - "Tick [ ] to [x] for 55-02 and 55-03 in ROADMAP.md lines 1120-1121"
---

# Phase 55: Push on Interface Arrays — Verification Report

**Phase Goal:** Root-cause and fix `.push()` on interface-typed split collections — programmatic
building of `[Shape]` via push loop must compile and run correctly. CONTEXT.md broadened scope
to also cover `.len()`, `.pop()`, `.get()`, `.set()` since they share the same emit_c.c
interception-block gap.

**Verified:** 2026-04-09T15:45:00Z
**Status:** gaps_found
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths (ROADMAP Success Criteria)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| SC1 | `var shapes: [Shape] = []; shapes.push(Circle(5))` compiles without errors | FAILED | Live test: E0202 type mismatch on empty interface literal `[]`. PLAN escape hatch not surfaced as blocker. |
| SC2 | Push loop building 100+ element split collection produces correct runtime output | VERIFIED | `push_interface_loop_100.iron` outputs `102` / `681750`, matches `.expected`. Runtime confirmed. |
| SC3 | Root cause documented in commit message (what broke, why, how fixed) | VERIFIED | Commit `118763c` — full root-cause in message body: HIR->LIR mangle + interception block miss, fix via new branch, both dispatch modes described. |
| SC4 | Regression test `push_interface_collection.iron` exists | VERIFIED | File present at `tests/integration/push_interface_collection.iron`, exercises original broken path (shapes + push(Circle(3))). |
| SC5 | Adjacent tests: multi-type, after-op, already-populated | VERIFIED | `push_interface_multi_type.iron`, `push_interface_after_op.iron`, `push_interface_prepopulated.iron` — all present and substantive. |

**Score:** 4/5 success criteria verified

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/lir/emit_c.c` | `_push` suffix detection (line 1528) | VERIFIED | `else if (strcmp(suffix, "_push") == 0) coll_method = "push";` at line 1528 |
| `src/lir/emit_c.c` | `_len` suffix detection (line 1529) | VERIFIED | `else if (strcmp(suffix, "_len") == 0) coll_method = "len";` at line 1529 |
| `src/lir/emit_c.c` | `_pop` suffix detection (line 1530) | VERIFIED | `else if (strcmp(suffix, "_pop") == 0) coll_method = "pop";` at line 1530 |
| `src/lir/emit_c.c` | `_get` suffix detection (line 1531) | VERIFIED | `else if (strcmp(suffix, "_get") == 0) coll_method = "get";` at line 1531 |
| `src/lir/emit_c.c` | `_set` suffix detection (line 1532) | VERIFIED | `else if (strcmp(suffix, "_set") == 0) coll_method = "set";` at line 1532 |
| `src/lir/emit_c.c` | `push` dispatch branch | VERIFIED | Substantive at line 1846 — Mode (a) concrete via `emit_get_value_type`, Mode (b) interface-typed via tag switch |
| `src/lir/emit_c.c` | `len` dispatch branch | VERIFIED | Substantive at line 1919 — direct `._total_count` field read |
| `src/lir/emit_c.c` | `pop` dispatch branch | VERIFIED | Substantive at line 1941 — `_order[_total_count-1]` tag switch, SoA fallback, decrement counts |
| `src/lir/emit_c.c` | `get` dispatch branch | VERIFIED | Substantive at line 2058 — `_order[i]` tag switch, SoA fallback, pure read |
| `src/lir/emit_c.c` | `set` dispatch branch | VERIFIED | Substantive at line 2166 — same-type in-place overwrite with runtime tag guard, documented no-op for diff-type/interface/SoA |
| `tests/integration/push_interface_collection.iron` | Main regression test | VERIFIED | Exercises original broken path; generates `Iron_SplitList_Iron_Shape_push_Circle` in C |
| `tests/integration/push_interface_collection.expected` | Expected output | VERIFIED | Present, non-empty |
| `tests/integration/push_interface_loop_100.iron` | 100+ element stress test | VERIFIED | 100-iteration while loop, 102 elements, total area 681750 |
| `tests/integration/push_interface_multi_type.iron` | Adjacent: multiple interface types | VERIFIED | Alternating Circle/Square pushes, sum-based assertion |
| `tests/integration/push_interface_after_op.iron` | Adjacent: push after filter() | VERIFIED | filter then push, validates composition |
| `tests/integration/push_interface_prepopulated.iron` | Adjacent: push into populated | VERIFIED | 4 initial + 3 pushed, count + total verified |
| `tests/integration/push_interface_typed_var.iron` | Interface-typed arg (Mode b) | VERIFIED | `pick_shape(flag) -> Shape` forces tag-switch dispatch path |
| `tests/integration/push_interface_len_pop.iron` | len + pop combined test | VERIFIED | len=3, pop area=27, len=2 |
| `tests/integration/push_interface_len_empty.iron` | len on empty (after pop) | VERIFIED | Pop to zero, len=0 |
| `tests/integration/push_interface_pop_order.iron` | LIFO order test | VERIFIED | Pop 4 shapes in reverse-push order |
| `tests/integration/push_interface_get.iron` | Get by index | VERIFIED | get(0,1,2) areas: 3, 4, 27 |
| `tests/integration/push_interface_set_same_type.iron` | Set same-type overwrite | VERIFIED | set then get-back, 75, 100, 27 |
| `tests/integration/push_interface_get_after_push.iron` | Get after push | VERIFIED | push then get, 3, 4, 27, 16 |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `emit_c.c` suffix switch (line 1528) | `push` dispatch branch (line 1846) | `coll_method = "push"` assignment | VERIFIED | Line 1528 sets `coll_method`; line 1846 checks `strcmp(coll_method, "push")` |
| `emit_c.c` suffix switch (line 1529) | `len` dispatch branch (line 1919) | `coll_method = "len"` | VERIFIED | Same pattern |
| `emit_c.c` suffix switch (line 1530) | `pop` dispatch branch (line 1941) | `coll_method = "pop"` | VERIFIED | Same pattern |
| `emit_c.c` suffix switch (line 1531) | `get` dispatch branch (line 2058) | `coll_method = "get"` | VERIFIED | Same pattern |
| `emit_c.c` suffix switch (line 1532) | `set` dispatch branch (line 2166) | `coll_method = "set"` | VERIFIED | Same pattern |
| `push` branch (Mode a) | `Iron_SplitList_Iron_Shape_push_Circle/Square` | `emit_get_value_type` + format string `Iron_SplitList_%s_push_%s` | VERIFIED | Generated C for `push_interface_collection.iron` contains `Iron_SplitList_Iron_Shape_push_Circle` at line 224-226; zero occurrences of `Iron_List_Iron_Shape_push` |
| `push` branch (Mode b) | per-type push via tag switch | `switch (_sp_push_val.tag)` | VERIFIED | Commit `118763c` diff shows tag-switch emission; `push_interface_typed_var` test passes |
| `pop` branch | `_order[]` + per-type sub-array | `_order[_sp_pop_i].tag` switch | VERIFIED | `55-02-SUMMARY.md` shows generated C: `switch (_v8._order[_sp_pop_i].tag)` + per-type case |
| `len` branch | `._total_count` field | direct field access | VERIFIED | `55-02-SUMMARY.md` shows `int64_t _v11 = _v8._total_count;` in generated C |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| PUSH-01 | 55-01, 55-02, 55-03 | `.push()` on interface-typed split collections works without codegen errors | VERIFIED (partially) | push/len/pop/get/set all have working dispatch branches. Empty-literal `[Shape]` = [] path not fixed (see SC1 gap). Push loop from pre-populated collection works. |
| PUSH-02 | 55-01 | Regression test exercises programmatic split collection building via push loop | VERIFIED | `push_interface_collection.iron` + `push_interface_loop_100.iron` both exist and pass. |

Both requirements marked `[x] Complete` in `REQUIREMENTS.md` lines 163-164.
The PUSH-01 partial note: the requirement text ("push loop compiles and runs correctly") is satisfied; the empty-literal path (ROADMAP SC1) is a stricter criterion not captured by PUSH-01's wording.

---

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `.planning/ROADMAP.md` | 1120-1121 | `- [ ] 55-02-PLAN.md` and `- [ ] 55-03-PLAN.md` | Info | Cosmetic: both plans have SUMMARY.md confirming completion, but ROADMAP checkboxes not ticked |

No code anti-patterns found in `src/lir/emit_c.c`. No TODO/FIXME/placeholder in any of the 12 new test files. All dispatch branches are substantive (no empty return, no stub). All known limitations (SoA fallback, set different-type no-op) documented in source via C comments and in SUMMARY.md files.

---

### Gaps Summary

**Gap 1 (blocking — ROADMAP SC1):** The exact ROADMAP success criterion 1 — `var shapes: [Shape] = []; shapes.push(Circle(5))` — fails at type inference with `E0202: type mismatch expected '[<interface>]', got '[<error>]'`. The PLAN included an escape hatch ("otherwise surface as blocker") but the SUMMARY never surfaced this as a blocker, and the SUMMARY incorrectly maps ROADMAP SC#1 to "regression test exists" (which is actually SC#4). Every Phase 55 test works around this by starting with a 2-element literal containing both implementors.

This gap is scoped to type inference for empty typed array literals — not the push dispatch itself. The push dispatch fix is complete. But the ROADMAP SC1 wording requires this specific pattern to compile without errors.

Resolution options (in order of effort):
1. Document the empty-literal limitation in REQUIREMENTS.md or a KNOWN_LIMITATIONS section as out-of-scope for Phase 55, update ROADMAP SC1 wording to match what was delivered.
2. Fix the empty interface literal type inference (likely involves `hir/type_check.c` or `hir/hir_to_lir.c` empty-array inference path), add a test, mark SC1 verified.

**Gap 2 (cosmetic — ROADMAP tracking):** `55-02-PLAN.md` and `55-03-PLAN.md` plan entries in ROADMAP.md show `[ ]` (unchecked). Both plans have SUMMARY.md files confirming completion and all their test files pass. This is a docs-only inconsistency.

---

### Human Verification Required

None — all behavioral checks were automatable. The generated C was spot-checked directly; the integration suite was run live (299 passing, 0 failing confirmed).

---

_Verified: 2026-04-09T15:45:00Z_
_Verifier: Claude (gsd-verifier)_
