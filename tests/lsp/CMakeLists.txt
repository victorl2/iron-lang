# LSP tests — Phase 1 ships the parity harness; full LSP suite arrives in
# Phase 2+ once `ironls` proper starts landing. Adding new subdirectories
# here does not require any edit to the root CMakeLists.txt (which only
# references this directory).

add_subdirectory(parity)
add_subdirectory(unit)
add_subdirectory(smoke)
# Phase 5 Plan 05-05 (FMT-06, D-07): quickfix fmt-clean CI gate.
add_subdirectory(fmt)
# Phase 7 Plan 07-01 (HARD-14, HARD-20): crash-dump + parent-death
# shell invariants. Pure CTest shell scripts (no compiled targets).
add_subdirectory(invariant)

# ── Phase 34 LSP-adaptation: Wave 0 fixture-corpus registration ─────────────
#
# Wave 0 reserves test slots for the hover / completion / quickfix surfaces
# whose drivers land in later Phase 34 waves. Each slot is marked WILL_FAIL
# TRUE so ctest reports the entry as PASSING while the driver is pending:
# the test stub prints a TODO and exits 1; the WILL_FAIL flag flips that
# into a pass. Once Wave 1/2 lands a real driver, the corresponding
# add_test command is replaced with a real invocation and the
# WILL_FAIL property is removed.
#
# Discovery: `ctest -L phase34-invariant -N` lists all reserved slots.
# Run:       `ctest -L phase34-invariant --output-on-failure`.
#
# The fixture .iron / .expected / .expected_edit files live under
# tests/lsp/fixtures/ and follow the format documented in
# .planning/phases/34-lsp-adaptation/34-01-PLAN.md Task 1.

# === Phase 34-02 hover tests ===
# Plan 34-02 retires the 7 WILL_FAIL hover stubs from Plan 34-01 and
# replaces them with the real driver under tests/lsp/hover/. The fixtures
# themselves still live under tests/lsp/fixtures/hover_*.{iron,expected};
# only the test wiring moved.
#
# add_subdirectory(hover) below registers test_hover_memory_model with
# labels "phase34-invariant;hover;lsp-unit", so `ctest -L phase34-invariant`
# now exercises the real fixture corpus end-to-end instead of TODO stubs.
add_subdirectory(hover)
# === end Phase 34-02 hover tests ===

# === Phase 34-04 quickfix tests ===
# Plan 34-04 retires the LSP-06..09 WILL_FAIL stubs (4 of 5 quickfix
# fixture slots) and wires the per-fixture drivers under
# tests/lsp/quickfix/. The 5th slot (quickfix_readonly_memory / LSP-10)
# flips in Task 2 of this same plan.
add_subdirectory(quickfix)
# === end Phase 34-04 quickfix tests ===

# === Phase 34-03 completion tests ===
# Plan 34-03 retires the 13 completion WILL_FAIL stubs from Plan 34-01
# (10 keyword fixtures + 3 defer-free snippet fixtures) and wires the
# real Unity drivers under tests/lsp/complete/. The fixtures themselves
# (tests/lsp/fixtures/complete_*.iron) remain on disk as reference
# documentation — the drivers mirror their scenarios inline to keep the
# test binaries self-contained (no runtime file I/O off the source tree).
#
# add_subdirectory(complete) below registers:
#   - test_complete_v4_keywords (Task 1 — 10 keyword arms)
#   - test_complete_defer_free_snippet (Task 2 — 3 snippet scenarios)
# both labeled "phase34-invariant;complete".
add_subdirectory(complete)
# === end Phase 34-03 completion tests ===

# Phase 34 Plan 34-04 Task 1 retired the WILL_FAIL stubs for
# quickfix_missing_val_var, quickfix_forgotten_free, quickfix_unused_var_alloc,
# and quickfix_amp_rc. Their drivers live in tests/lsp/quickfix/ as
# test_quickfix_lsp_{06,07,08,09}_* (labels phase34-invariant + quickfix).
# Only quickfix_readonly_memory (LSP-10) remains WILL_FAIL until Task 2
# of this same plan lands its handler.
set(PHASE34_QUICKFIX_FIXTURES
    quickfix_readonly_memory)
foreach(fix ${PHASE34_QUICKFIX_FIXTURES})
    add_test(NAME lsp_${fix}
        COMMAND sh -c "echo 'TODO Phase 34 Wave 2b: quickfix driver pending for ${fix}'; exit 1")
    set_tests_properties(lsp_${fix} PROPERTIES
        LABELS "phase34-invariant"
        WILL_FAIL TRUE)
endforeach()

# Publish_diag_820 — depends on IRON_ERR_READONLY_MEMORY=820 (Task 2 of
# Plan 34-01) AND on the compiler-side emit site landing in a follow-up
# plan. Registered WILL_FAIL until both pieces ship.
add_test(NAME lsp_publish_diag_820
    COMMAND sh -c "echo 'TODO Phase 34: publishDiagnostics harness for 820-range pending compiler-side emit site'; exit 1")
set_tests_properties(lsp_publish_diag_820 PROPERTIES
    LABELS "phase34-invariant"
    WILL_FAIL TRUE)
