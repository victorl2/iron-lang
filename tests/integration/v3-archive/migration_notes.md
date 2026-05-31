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

All 30 collection fixtures migrated as-is — surface API (List/Map/Set
iteration, push/pop/foreach/map/filter/reduce, split methods) is preserved
v3→v4 (Phase 33 stdlib container rewrite kept the user-facing call shapes
stable). Verified end-to-end via v4 ironc on silvaserver podman.

Migrated:
- coll_*: coll_chain_advanced, coll_chain_map_filter_sum, coll_empty_array,
  coll_filter_basic, coll_float_sum, coll_foreach_basic, coll_map_basic,
  coll_reduce_basic, coll_split_filter, coll_split_map, coll_split_reduce,
  coll_sum_basic
- collection_*: collection_lifecycle
- array_*: array_param_passing
- push_*: push_interface_after_op, push_interface_collection,
  push_interface_get, push_interface_get_after_push, push_interface_len_empty,
  push_interface_len_pop, push_interface_loop_100, push_interface_multi_type,
  push_interface_pop_order, push_interface_prepopulated,
  push_interface_set_same_type, push_interface_typed_var
- split_*: split_collection, split_collection_basic,
  split_collection_multi_method, split_collection_param

## Control flow (control_*, defer_*, early_*, edge_*, args_*)

All 10 control-flow fixtures migrated as-is — defer surface
unchanged v3→v4 (Phase 32 defer-statement extended semantics without
breaking the v3 `defer free <binding>` ergonomic). Verified end-to-end via
v4 ironc on silvaserver podman.

Migrated:
- control_flow
- defer_multi_exit
- early_return_defer
- edge_all_filtered_out, edge_empty_collection, edge_single_element,
  edge_single_implementor, edge_zero_field
- args_threading
- audit_defer_in_for_body

## Expressions (bitwise_*, binary_*, int_*, str_*, tuple_*, expr_*, blind_cast_*, layout_*)

All 47 expression fixtures + 4 singleton surface fixtures migrated as-is.
Pure-expression semantics (arithmetic, bitwise, string ops, tuple
destructure, comptime int folding, SoA layout, blind-cast escape hatch)
are preserved v3→v4. `blind_cast_*` fixtures map to v4 `Ptr.cast[T]`
through the Phase 25 + Phase 33-06 RawPtr surface — no syntactic changes
in user code. Verified end-to-end via v4 ironc on silvaserver podman.

Migrated:
- bitwise_*: bitwise_and_or_xor, bitwise_compound_assign, bitwise_not,
  bitwise_precedence, bitwise_shift
- binary_*: binary_literal
- str_*: str_char_at, str_contains, str_count, str_len_repeat, str_pad,
  str_parse, str_replace, str_split_join, str_starts_ends,
  str_substring_indexof, str_trim, str_upper_lower
- tuple_*: tuple_equality, tuple_local_and_param, tuple_nested,
  tuple_return_basic, tuple_return_heterogeneous, tuple_return_smoke,
  tuple_wildcard_destructure
- expr_*: expr_inline_arithmetic, expr_inline_closures,
  expr_inline_generics, expr_inline_load, expr_inline_nested
- blind_cast_*: blind_cast_expr_common_layout, blind_cast_expr_resolved_type,
  blind_cast_leak_ident, blind_cast_owner_decl, blind_cast_type_sym_decl
- layout_*: layout_annotation, layout_annotation_warn, layout_bench,
  layout_common_field, layout_dead_field, layout_soa_select,
  layout_variant_split
- int_*: int_comptime_arith_overflow, int_comptime_neg_min,
  int_enum_value_overflow
- int32_*: int32_array, int32_basic, int32_narrowing

## Single-occurrence prefixes

Migrated to expressions/ (verified v4-compatible by silvaserver podman):
- functions.iron — function-decl surface smoke
- hello.iron — minimal hello-world smoke
- objects.iron — object-decl surface smoke
- variables.iron — val/var binding surface smoke

## HIR coverage (hir_*, mono_*, fusion_*, compose_*)

**Bulk INTERNAL_IR + OPTIMIZER classification.** The hir_*, mono_*, fusion_*,
and compose_* fixture families collectively exercise compiler internal IR
behavior (HIR-printer / HIR-lowering / LIR monomorphization / SoA fusion /
arena-soa-dead composition) rather than user-observable surface semantics.
These responsibilities are covered by v4's dedicated unit tests under
`tests/hir/` (test_hir_*.c, test_hir_lower.c, etc.) and `tests/lir/`
(test_lir_*.c, test_lir_mono_*.c, test_lir_fusion_*.c, test_value_range_*.c),
with more focused assertions than the integration round-trip can provide.

**INTERNAL_IR (hir_*, 125 fixtures):**
All 125 hir_* fixtures — INTERNAL_IR. Each fixture asserts hir_print or
HIR-lowering output for a specific surface construct (arithmetic, control
flow, closure, ADT, generic, heap expr, array literal, etc.). v4 covers
these with focused tests/hir/ unit tests against fresh HIR construction.
Representative essence-migrated samples land under hir-coverage/:
hir_array_literal, hir_binary_search, hir_bubble_sort,
hir_canary_closure_capture, hir_canary_for, hir_canary_heap_expr.

**OPTIMIZER (mono_*, fusion_*, compose_*, 29 fixtures):**
All 16 mono_*, 8 fusion_*, and 5 compose_* fixtures — OPTIMIZER. Mono
fusion, SoA fusion, dead-field compression, and arena-soa-dead composition
are pass-level invariants exercised by tests/lir/ unit tests
(test_lir_mono_*.c, test_lir_fusion_*.c). The integration fixtures only
prove the optimizer didn't crash on a surface example; the unit tests prove
the optimizer produces the expected IR shape, which is the load-bearing
property.

## v3-specific (v3_*, plus v4-removed surface)

**Bulk REMOVED classification — v3 syntax / v3-only semantics that v4
removed or replaced.** Even though most v3_* fixtures happen to still
compile under v4 (because they exercise constructs that survived the v4
transition), they were originally authored to lock v3-spec semantics. The
v4 corpus has its own targeted fixtures for the same surface under
`tests/integration/v4/4.4-readonly/` (readonly), `4.8-rc-policy` (rc),
`4.9-weak-rc-policy` (weak rc), etc.

**REMOVED (v3_*, 13 fixtures):**
v3_iface_default_body, v3_init_anonymous_and_named, v3_init_fieldless_marker,
v3_methods_in_block, v3_patch_implements, v3_patch_primitive,
v3_pub_field_synthesis, v3_pure_method, v3_readonly_transitive, v3_self_return,
v3_spec_visibility_example, v3_str_concat_chained, v3_str_describable_example
— all REMOVED. v3-spec lock fixtures. v3_pure_method also genuinely fails
under v4 ironc (pure-method body restrictions changed v3→v4). The other 12
happen to still compile under v4 but exist for v3 traceability and don't
add coverage beyond v4-corpus equivalents.

**REMOVED (v4 surface no longer accepts):**
- audit_struct_method_mutation — `patch object` + readonly receiver-method
  pattern; build fails under v4 ironc. v4 receiver-method semantics covered
  by `tests/integration/v4/7.5-stdlib/` (mutex_guard, channel_bounded, etc.).
- empty_literal_return — `return []` empty-literal type inference; build
  fails under v4 ironc. v4 covers concrete empty-collection construction
  via dedicated 7.5-stdlib fixtures.

## Misc bulk-migrated surface fixtures

In addition to the per-category essence migrations above, **95 surface-level
fixtures from miscellaneous v3 prefixes** were verified to run cleanly under
v4 ironc and migrated to `tests/integration/v4/migrated-from-v3/misc/`. These
exercise small surface-feature smoke tests (arena, audit, bug regressions,
comptime, copy/dce/const-fold optimizer probes from the surface side,
empty-collection construction, extern, fill, fmt, generic dispatch, heap,
hex literals, hint, import, inline, interface dispatch, io/log/math/time
stdlib, lambda capture, loop patterns, nested control flow, net stdlib,
null heap/rc alloc, parallel for, rc patterns, smoke, soa fusion surface,
spawn/await, stack arrays, static dispatch, store/load elim surface,
strength reduction surface, stress fixtures, test_ stdlib, url stdlib,
value-range surface) that complement the structured per-category subdirs.

See `tests/integration/v4/migrated-from-v3/misc/` for the file list — the
fixtures are name-prefixed by feature group (audit_*, bug_*, comptime_*,
const_*, copy_*, dce_*, empty_*, extern_*, fill_*, fmt_*, generic_*,
heap_*, hex_*, hint_*, import_*, inline_*, interface_*, io_*, lambda_*,
log_*, loop_*, math_*, nested_*, net_*, null_*, parallel_*, rc_*, smoke_*,
soa_*, spawn_*, stack_*, static_*, store_*, strength_*, stress_*, test_*,
time_*, url_*, value_*, plus arena_cross_arena_fix and arena_split_collection).
Total: 95.

---

## Final accounting

| Bucket | Count |
|---|---|
| v3-archive .iron total | **388** |
| Migrated to v4/migrated-from-v3/ | **225** |
|   ├── closures/ (capture_*) | 20 |
|   ├── adt-match/ (adt_*, match_*) | 12 |
|   ├── collections/ (coll_*, collection_*, array_*, push_*, split_*) | 30 |
|   ├── control-flow/ (control_*, defer_*, early_*, edge_*, args_*, audit_defer_*) | 10 |
|   ├── expressions/ (bitwise_*, binary_*, str_*, tuple_*, expr_*, blind_*, layout_*, int_*, int32_*, functions, hello, objects, variables) | 52 |
|   ├── hir-coverage/ (representative hir_* essence samples) | 6 |
|   └── misc/ (bulk surface fixtures from 41 remaining prefixes) | 95 |
| Documented as REMOVED/INTERNAL_IR/OPTIMIZER (not migrated) | **163** |
|   ├── hir_* (INTERNAL_IR, minus 6 essence samples) | 119 |
|   ├── mono_* (OPTIMIZER) | 16 |
|   ├── fusion_* (OPTIMIZER) | 8 |
|   ├── compose_* (OPTIMIZER) | 5 |
|   ├── v3_* (REMOVED — v3-spec lock fixtures) | 13 |
|   └── v4-removed surface fixtures (audit_struct_method_mutation, empty_literal_return) | 2 |
| **Migrated + Documented** | **388** ✓ |

No archived fixture is left unaccounted for.

<!-- (Single-occurrence prefixes consolidated into the Expressions section above.) -->
<!-- Additional bulk-documented singletons logged by Task 6. -->

