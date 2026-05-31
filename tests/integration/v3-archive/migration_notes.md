# Migration notes — v3 → v4 corpus

Phase 35 MIG-09 hand-migration log. For each v3-archive fixture that
was NOT migrated essence-wise to `tests/integration/v4/migrated-from-v3/`,
this file records why.

Reasons fall into 5 buckets:

- **REMOVED**: tested a v3-only language feature (e.g., `mut` keyword in
  receiver position) that has no v4 analog
- **INTERNAL_IR**: tested compiler IR (HIR/LIR) behavior, not surface
  semantics; v4 has its own dedicated HIR/LIR unit tests
- **OPTIMIZER**: tested optimizer pass behavior (SoA fusion, dead-field
  compression, mono fusion); v4 optimizer has its own pass-level tests
  under tests/lir/
- **DEBUG_ONLY**: bug repro fixture from a v3 era; equivalent v4 behavior
  is covered by other fixtures or v4-spec semantics make the bug
  impossible
- **DUPLICATE**: covered by an existing tests/integration/v4/ fixture

For migrated fixtures, see `tests/integration/v4/migrated-from-v3/<category>/`.

## Closures (capture_*, lambda_*)

All 20 `capture_*` fixtures migrated as-is — they were already written
in v4-compatible syntax (val/var discipline correct; no `mut` keyword;
no `box`/`rc`/`weak` constructs; closure syntax unchanged v3→v4). Verified
end-to-end via v4 ironc on silvaserver podman.

Migrated: capture_01_immutable, capture_02_mutate, capture_03_multi_type,
capture_04_loop_snapshot, capture_05_make_adder, capture_06_object_capture,
capture_06_return_closure, capture_07_callback_arg,
capture_09_lambda_capture_lambda, capture_10_make_counter,
capture_11_shared_mutable, capture_12_capture_in_branch,
capture_13_capture_in_match, capture_14_filter_with_capture,
capture_15_spawn_capture, capture_16_pfor_capture,
capture_17_closure_object_field, capture_18_compose,
capture_19_recursive_lambda, capture_20_game_state.

(entries below added by subsequent tasks)

## ADTs + Match (adt_*, match_*, enum_*)

All 11 `adt_*` + 1 `match_expressions` fixtures migrated as-is —
ADT/match semantics are preserved v3→v4 (no spec change to match arms or
pattern-binding). Verified end-to-end via v4 ironc on silvaserver podman.

Migrated: adt_else_arm, adt_enum_construct, adt_enum_decl, adt_match_syntax,
adt_mixed_payload, adt_nested_pattern, adt_pattern_binding, adt_recursive_expr,
adt_recursive_generic, adt_recursive_list, adt_wildcard_pattern,
match_expressions.

REMOVED (kept disabled in v3, no v4 analog):
- adt_enum_method.iron.disabled — REMOVED. ADT enum-method dispatch was
  disabled pre-v4 (already `.disabled` in v3-archive); v4 receiver-method
  semantics covered by v4 corpus under `tests/integration/v4/7.5-stdlib/`
  (mutex_guard, channel_bounded, filehandle_drop receiver-form patterns).
- adt_plain_enum_method.iron.disabled — REMOVED. Same rationale.
- enum_construct_reinterpret.iron.disabled — REMOVED. Tested v3-specific
  enum reinterpret-cast machinery; v4 forbids the operation via
  drop/copy/nocopy resource discipline.

## Collections (coll_*, collection_*, array_*, push_*, split_*)
(entries added by Task 3)

## Control flow (control_*, defer_*, early_*, edge_*, args_*)
(entries added by Task 4)

## Expressions (bitwise_*, binary_*, int_*, str_*, tuple_*, expr_*, blind_cast_*, layout_*)
(entries added by Task 5)

## HIR coverage (hir_*, mono_*, fusion_*, compose_*)
(entries added by Task 6)

## v3-specific (v3_*, bug_audit_mut_*, etc.)
(entries added by Task 6 — all marked REMOVED; v3 syntax is gone)

## Single-occurrence prefixes
(entries added by Task 5/6 misc bucket)
