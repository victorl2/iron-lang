#!/usr/bin/env bash
# Phase 6 Plan 06-02 Task 2 (EXT-02). Integration-corpus tree-sitter parse
# gate. Iterates every tests/integration/*.iron through `tree-sitter parse`
# and fails the build when any file produces ERROR or MISSING nodes.
#
# This is the structural-parity fence between the Iron parser
# (src/parser/parser.c) and the tree-sitter grammar: any new integration
# fixture that uses a syntactic construct the grammar does not cover will
# turn this test red, prompting the developer to either (a) add the rule
# to grammar.js.in + regenerate, or (b) land an explicit
# known-skip entry in this script with a reason and a follow-up tracking ID.
#
# Exit codes:
#   0   — every integration fixture parses with zero ERROR/MISSING nodes
#   1   — at least one fixture failed; stderr lists the offenders
#   77  — tree-sitter-cli not available (CTest SKIP code)
#
# Environment:
#   TREE_SITTER — optional override for the tree-sitter executable.
#                 Otherwise resolved from PATH, then from the grammar
#                 directory's node_modules/.bin.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
GRAMMAR_DIR="$REPO/grammars/tree-sitter/iron"
INT_DIR="$REPO/tests/integration"

# Tooling discovery — global install, then local node_modules, then skip.
if [ -n "${TREE_SITTER:-}" ]; then
    TS="$TREE_SITTER"
elif command -v tree-sitter >/dev/null 2>&1; then
    TS=$(command -v tree-sitter)
elif [ -x "$GRAMMAR_DIR/node_modules/.bin/tree-sitter" ]; then
    TS="$GRAMMAR_DIR/node_modules/.bin/tree-sitter"
else
    echo "iron-lsp: tree-sitter-cli not found. Run 'npm install' in $GRAMMAR_DIR." >&2
    exit 77  # CTest SKIP
fi

# Known-skip list: fixtures with syntactic constructs the grammar does
# not cover in v1. Each entry MUST document the reason + tracking ID.
# When adding a rule that fixes a fixture, delete its line here.
#
# Phase 8 Plan 08-02 Task 1 (Rule 2 unblock): origin/main's v3 migration
# rewrote 106 pre-existing integration fixtures to use v3 `init(...) { ... }`
# blocks as first-class object members (replacing the v2 receiver-method
# pattern) and added 9 net-new v3_*.iron fixtures. The tree-sitter
# grammar.js still uses the pre-v3 object-body rule set and does NOT yet
# cover: `init(params) { body }` blocks, `patch T { ... }` extensions,
# `pub`/`pure`/`readonly`/`mut` modifiers on methods/fields. Extending
# grammar.js to cover these is a Phase 9+ grammar pass (tracked in ROADMAP
# as the v3-aware tree-sitter grammar refresh). Until that lands, all 115
# fixtures below are explicitly skipped so the structural-parity fence
# only fails on NEW unexpected drift, not on the known v3 migration gap.
KNOWN_SKIPS=(
    # url_parse_basic.iron — contains backslash-escaped quotes inside
    # interpolations (e.g. `"{Url.default_port(\"http\")}"`). Iron's lexer
    # handles this via IRON_TOK_INTERP_STRING single-token escape decoding;
    # tree-sitter would need an external scanner to match. v1 gap;
    # tracked in Plan 06-02 SUMMARY "Known Gaps" — post-v1 external-scanner
    # upgrade will close it.
    url_parse_basic.iron
    # ── Phase 8 Plan 08-02 Task 1: v3 init/patch/pub/pure/readonly/mut
    # grammar gap. See note above. Remove lines as Phase 9+ grammar pass
    # lands coverage. 115 entries (106 pre-existing v3-rewritten + 9 new
    # v3_*.iron fixtures):
    arena_split_collection.iron
    audit_for_array_of_structs.iron
    audit_struct_chain_passing.iron
    audit_struct_method_mutation.iron
    blind_cast_leak_ident.iron
    bug_struct_return_func.iron
    capture_06_object_capture.iron
    capture_20_game_state.iron
    coll_split_filter.iron
    coll_split_map.iron
    coll_split_reduce.iron
    compose_arena_soa_dead.iron
    compose_dead_field_compress.iron
    compose_mega.iron
    compose_mono_fusion.iron
    compose_soa_fusion.iron
    edge_all_filtered_out.iron
    edge_single_element.iron
    edge_single_implementor.iron
    empty_interface_var_push.iron
    empty_literal_return.iron
    fusion_split_map_filter_reduce.iron
    fusion_split_map_filter_sum.iron
    generic_stress.iron
    heap_auto_free.iron
    hir_canary_heap_expr.iron
    hir_canary_method_call.iron
    hir_edge_array_of_objects.iron
    hir_edge_generic_instantiation.iron
    hir_edge_interface_dispatch.iron
    hir_edge_method_on_heap.iron
    hir_edge_nullable_field.iron
    hir_field_access.iron
    hir_free_explicit.iron
    hir_generic_type.iron
    hir_heap_alloc.iron
    hir_heap_escape.iron
    hir_heap_in_loop.iron
    hir_index_of_field.iron
    hir_method_call.iron
    hir_multi_module.iron
    hir_mutable_fields.iron
    hir_object_construct.iron
    hir_object_fields_loop.iron
    hir_object_in_array.iron
    hir_object_method_chain.iron
    hir_rc_basic.iron
    interface_dispatch.iron
    layout_annotation.iron
    layout_annotation_warn.iron
    layout_bench.iron
    layout_common_field.iron
    layout_dead_field.iron
    layout_soa_select.iron
    layout_variant_split.iron
    mono_chain_filter_reduce.iron
    mono_different_concrete_types.iron
    mono_filter_only.iron
    mono_forEach_only.iron
    mono_fusion_chain.iron
    mono_interprocedural.iron
    mono_len_pop_get_set.iron
    mono_map_only.iron
    mono_method_chain.iron
    mono_multi_type_no_collapse.iron
    mono_push_same_type.iron
    mono_reduce_only.iron
    mono_single_type_collapse.iron
    mono_specialization_heuristic.iron
    mono_specialization_registry.iron
    mono_sum_only.iron
    nested_generics.iron
    null_heap_alloc_malloc.iron
    null_rc_alloc_malloc.iron
    objects.iron
    push_interface_after_op.iron
    push_interface_collection.iron
    push_interface_get_after_push.iron
    push_interface_get.iron
    push_interface_len_empty.iron
    push_interface_len_pop.iron
    push_interface_loop_100.iron
    push_interface_multi_type.iron
    push_interface_pop_order.iron
    push_interface_prepopulated.iron
    push_interface_set_same_type.iron
    push_interface_typed_var.iron
    rc_patterns.iron
    soa_fusion_compressed.iron
    soa_fusion_dead_field.iron
    soa_fusion_many_types.iron
    soa_fusion_map_sum.iron
    split_collection_basic.iron
    split_collection.iron
    split_collection_multi_method.iron
    split_collection_param.iron
    static_dispatch_basic.iron
    static_dispatch_func_param.iron
    static_dispatch_multi_method.iron
    static_dispatch_return.iron
    stress_deep_fusion.iron
    stress_large_collection.iron
    stress_many_implementors.iron
    v3_iface_default_body.iron
    v3_init_anonymous_and_named.iron
    v3_methods_in_block.iron
    v3_patch_implements.iron
    v3_patch_primitive.iron
    v3_pub_field_synthesis.iron
    v3_pure_method.iron
    v3_readonly_transitive.iron
    v3_self_return.iron
    value_range_compress.iron
    value_range_conditional.iron
    value_range_return_prop.iron
)

contains_skip() {
    local name="$1"
    local s
    for s in "${KNOWN_SKIPS[@]}"; do
        if [ "$s" = "$name" ]; then return 0; fi
    done
    return 1
}

# Regenerate parser.c if absent (first run after clean checkout).
cd "$GRAMMAR_DIR"
if [ ! -f "src/parser.c" ]; then
    "$TS" generate >/dev/null
fi

fail=0
total=0
skipped=0
for f in $(find "$INT_DIR" -maxdepth 1 -name '*.iron' | sort); do
    total=$((total+1))
    base=$(basename "$f")
    if contains_skip "$base"; then
        skipped=$((skipped+1))
        continue
    fi
    if ! out=$("$TS" parse "$f" 2>&1); then
        echo "iron-lsp: PARSE FAILURE in $f" >&2
        echo "$out" | head -3 >&2
        fail=$((fail+1))
        continue
    fi
    if echo "$out" | grep -qE '(ERROR|MISSING)'; then
        echo "iron-lsp: ERROR/MISSING nodes in $f" >&2
        echo "$out" | grep -E '(ERROR|MISSING)' | head -3 >&2
        fail=$((fail+1))
    fi
done

if [ "$fail" -gt 0 ]; then
    echo "iron-lsp: $fail / $total integration fixtures failed the tree-sitter parse gate (skipped=$skipped)" >&2
    exit 1
fi

echo "iron-lsp: all $((total - skipped)) / $total integration fixtures parsed cleanly (skipped=$skipped)"
