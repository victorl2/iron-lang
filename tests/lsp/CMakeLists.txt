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

set(PHASE34_HOVER_FIXTURES
    hover_policy_heap
    hover_policy_rc
    hover_policy_weak_rc
    hover_policy_stack
    hover_regime_unchecked
    hover_readonly_func
    hover_nocopy_object)
foreach(fix ${PHASE34_HOVER_FIXTURES})
    add_test(NAME lsp_hover_${fix}
        COMMAND sh -c "echo 'TODO Phase 34 Wave 1a: hover driver pending for ${fix}'; exit 1")
    set_tests_properties(lsp_hover_${fix} PROPERTIES
        LABELS "phase34-invariant"
        WILL_FAIL TRUE)
endforeach()

set(PHASE34_COMPLETE_FIXTURES
    complete_keyword_heap
    complete_keyword_rc
    complete_keyword_weak
    complete_keyword_unchecked
    complete_keyword_defer
    complete_keyword_drop
    complete_keyword_copy
    complete_keyword_nocopy
    complete_keyword_leak
    complete_keyword_in
    complete_defer_free_heap
    complete_defer_free_rc
    complete_defer_free_none)
foreach(fix ${PHASE34_COMPLETE_FIXTURES})
    add_test(NAME lsp_complete_${fix}
        COMMAND sh -c "echo 'TODO Phase 34 Wave 1b/2a: completion driver pending for ${fix}'; exit 1")
    set_tests_properties(lsp_complete_${fix} PROPERTIES
        LABELS "phase34-invariant"
        WILL_FAIL TRUE)
endforeach()

set(PHASE34_QUICKFIX_FIXTURES
    quickfix_missing_val_var
    quickfix_forgotten_free
    quickfix_unused_var_alloc
    quickfix_amp_rc
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
