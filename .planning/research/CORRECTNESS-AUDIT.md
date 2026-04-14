# Iron Compiler Correctness Audit

**Date:** 2026-04-12
**Scope:** All C source files in src/ (excluding src/vendor/ and src/cli/toml.c)
**Dimensions:** Blind casts, enum switch exhaustiveness, null safety, arena lifetimes, integer safety, allocation error handling
**Cross-platform findings:** See CROSS-PLATFORM-DEBT.md

## Methodology

This audit systematically examined all C source files across 7 dimensions:

1. **Blind Casts (AUDIT-01):** Every pointer cast from a generic type (Iron_Node *, void *) to a concrete struct was checked for a preceding kind guard (switch case, if check, or assertion).
2. **Enum Switch Exhaustiveness (AUDIT-02):** Every switch statement over an Iron enum type was checked for missing cases and evaluated whether the default clause is safe.
3. **Null Safety (AUDIT-03):** Every pointer dereference was traced to its source and checked for NULL guard presence, especially arena allocations, function returns, and array element access.
4. **Arena Lifetimes (AUDIT-04):** Every cross-arena pointer storage pattern was identified -- arena-allocated pointers stored in heap structures (stb_ds) or vice versa.
5. **Integer Safety (AUDIT-05):** Every arithmetic operation, size_t/int truncation, and buffer size calculation was evaluated for overflow/underflow risk.
6. **Allocation Error Handling (AUDIT-06):** Every malloc, realloc, calloc, arena_alloc, and stb_ds allocation was checked for NULL return handling.
7. **Cross-Platform (AUDIT-08):** Moved to CROSS-PLATFORM-DEBT.md. See that document for all platform-specific findings.

The audit was conducted directory-by-directory across 5 passes:
- Plan 01: Parser + Lexer (src/parser/, src/lexer/)
- Plan 02: Analyzer (src/analyzer/)
- Plan 03: HIR + Comptime (src/hir/, src/comptime/)
- Plan 04: LIR (src/lir/)
- Plan 05: Runtime + Stdlib + Infrastructure (src/runtime/, src/stdlib/, src/cli/, src/diagnostics/, src/pkg/, src/util/)

Each finding has a severity rating: **H** (high -- crash, UB, data corruption, or security risk), **M** (medium -- correctness risk under abnormal conditions such as OOM), **L** (low -- theoretical or cosmetic).

---

## Overall Summary

| Dimension | High | Medium | Low | Total |
|-----------|------|--------|-----|-------|
| 1. Blind Casts | 8 | 198 | 65 | 271 |
| 2. Enum Switch Exhaustiveness | 0 | 17 | 46 | 63 |
| 3. Null Safety | 6 | 63 | 63 | 132 |
| 4. Arena Lifetimes | 0 | 33 | 32 | 65 |
| 5. Integer Safety | 0 | 23 | 68 | 91 |
| 6. Allocation Error Handling | 4 | 285 | 44 | 333 |
| **Total** | **18** | **619** | **318** | **955** |

Note: Cross-Platform (AUDIT-08) findings are documented separately in CROSS-PLATFORM-DEBT.md and not included in these totals.

---

## 1. Blind Casts (AUDIT-01)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | typecheck.c:1360 | H | `(Iron_ObjectDecl *)callee_sym->decl_node` when sym_kind==SYM_TYPE -- if decl_node is InterfaceDecl or EnumDecl, memory is silently misinterpreted as ObjectDecl | Add `decl_node->kind == IRON_NODE_OBJECT_DECL` check before cast | blind_cast_type_sym_decl.iron |
| 2 | typecheck.c:1785 | H | `(Iron_ObjectDecl *)sym->decl_node` in CONSTRUCT handler -- same pattern as line 1360; SYM_TYPE can point to InterfaceDecl/EnumDecl | Add kind check on decl_node before cast | blind_cast_type_sym_decl.iron |
| 3 | typecheck.c:3064 | H | `(Iron_IntLit *)rs->value` accesses resolved_type through wrong concrete type -- aliasing-unsafe; reads resolved_type field at potentially wrong offset | Use a generic expr type or proper union access for resolved_type | blind_cast_expr_resolved_type.iron |
| 4 | resolve.c:236 | H | `(Iron_ObjectDecl *)owner_sym->decl_node` -- fails when owner is an enum type using `super`; the cast misinterprets EnumDecl memory as ObjectDecl | Add kind check on decl_node before cast | blind_cast_owner_decl.iron |
| 5 | resolve.c:251 | H | `(Iron_ObjectDecl *)owner_sym->decl_node` -- same pattern as line 236, second call site | Add kind check | blind_cast_owner_decl.iron |
| 6 | resolve.c:674 | H | In-place reinterpretation of `Iron_EnumConstruct` as `Iron_MethodCallExpr` -- layout-dependent undefined behavior; relies on identical memory layout between unrelated struct types | Use fresh allocation or add static_assert on compatible layouts | enum_construct_reinterpret.iron |
| 7 | resolve.c:680 | H | In-place reinterpretation of `Iron_EnumConstruct` as `Iron_FieldAccess` -- same dangerous layout-dependent UB as line 674 | Use fresh allocation | enum_construct_reinterpret.iron |
| 8 | hir_lower.c:109 | H | `((Iron_ExprNode *)node)->resolved_type` -- locally-defined struct assumes `{span, kind, resolved_type}` prefix layout across ALL AST node types; if any type violates this prefix, this reads garbage | Add _Static_assert or compile-time check for compatible prefix layout across all AST expression types | blind_cast_expr_common_layout.iron |
| 9 | escape.c:251 | M | `(Iron_Ident *)ls->expr` without kind check -- expr_ident_name can return non-NULL from FieldAccess chain root; casting FieldAccess as Ident reads wrong memory | Add `ls->expr->kind == IRON_NODE_IDENT` check before cast | blind_cast_leak_ident.iron |
| 10 | typecheck.c:443 | M | `(Iron_InterfaceDecl *)csym->decl_node` relies on sym_kind implying InterfaceDecl without kind check on decl_node | Add `assert(csym->decl_node->kind == IRON_NODE_INTERFACE_DECL)` | blind_cast_iface_decl.iron |
| 11 | typecheck.c:503 | M | `(Iron_Ident *)generic_params[i]` relies on generic_params[] only containing Ident nodes | Add kind check | blind_cast_generic_param.iron |
| 12 | typecheck.c:708 | M | `(Iron_Ident *)ed->generic_params[i]` -- same pattern as line 503 | Add kind check | blind_cast_generic_param.iron |
| 13 | typecheck.c:1401 | M | `(Iron_Ident *)od->generic_params[gi]` -- same pattern | Add kind check | blind_cast_generic_param.iron |
| 14 | typecheck.c:1525 | M | `(Iron_FuncDecl *)fn_sym->decl_node` relies on sym_kind without kind check on decl_node | Add kind check | blind_cast_func_sym_decl.iron |
| 15 | typecheck.c:1531 | M | `(Iron_Ident *)fd->generic_params[gi]` -- same generic_params pattern | Add kind check | blind_cast_generic_param.iron |
| 16 | typecheck.c:2189 | M | `(Iron_Ident *)ed->generic_params[gi]` -- same pattern | Add kind check | blind_cast_generic_param.iron |
| 17 | typecheck.c:2210 | M | `(Iron_Ident *)ed->generic_params[gi]` -- nested loop, same | Add kind check | blind_cast_generic_param.iron |
| 18 | typecheck.c:2301 | M | `(Iron_Ident *)ed->generic_params[gi]` -- same pattern | Add kind check | blind_cast_generic_param.iron |
| 19 | typecheck.c:2771 | M | `(Iron_IsExpr *)is_s->condition` without kind re-check after classify_is_check | Add `assert(is_s->condition->kind == IRON_NODE_IS)` | blind_cast_is_expr.iron |
| 20 | typecheck.c:3056 | M | `(Iron_Block *)ss->body` assumes spawn body is always Block without kind check | Add `if (ss->body && ss->body->kind == IRON_NODE_BLOCK)` guard | blind_cast_spawn_body.iron |
| 21 | typecheck.c:3225 | M | `(Iron_InterfaceDecl *)iface_sym->decl_node` relies on sym_kind | Add kind check on decl_node | blind_cast_iface_decl.iron |
| 22 | resolve.c:612 | M | `(Iron_EnumDecl *)esym->decl_node` relies on sym_kind without kind check | Add kind check on decl_node | blind_cast_enum_decl.iron |
| 23 | resolve.c:689 | M | `(Iron_EnumDecl *)esym->decl_node` -- same pattern | Add kind check | blind_cast_enum_decl.iron |
| 24 | iron_net.c:594 | M | `(uint8_t *)(uintptr_t)base` casts away const from iron_string_cstr() to pass as recv buffer -- writes into immutable Iron_String data | Use a local buffer and copy back | test_blind_cast_recv_into_string |
| 25 | emit_c.c:533 | M | `(Iron_EnumVariant *)adt_ed->variants[variant_idx]` -- cast from void* array relies on correct storage | Add assert for variant array integrity | blind_cast_enum_variant |
| 26 | emit_c.c:3154 | M | `(Iron_EnumVariant *)adt_ed->variants[variant_idx]` in CONSTRUCT statement | Same concern as #25 | blind_cast_construct_variant |
| 27 | emit_structs.c:57-67 | M | `(Iron_Field *)od->fields[i]` -- cast from void* field array without kind check | Add _Static_assert on field layout | blind_cast_field_access |
| 28 | emit_structs.c:69 | M | `(Iron_TypeAnnotation *)f->type_ann` -- relies on field->type_ann being TypeAnnotation without validation | Same pattern as #27 | blind_cast_type_ann |
| 29 | emit_structs.c:259-260 | M | `(Iron_Field *)od->fields[i]` and `(Iron_TypeAnnotation *)ta` repeated in emit_object_struct_body | Add compile-time field layout assertion | blind_cast_struct_fields |
| 30 | emit_structs.c:315 | M | `(Iron_Field *)od->fields[i]` in emit_estimate_type_size -- unguarded void* cast | No runtime check possible since fields is void** | blind_cast_estimate_size |
| 31 | lir_optimize.c:109-110 | M | Phi in-place mutation to LOAD: rewrites kind + union member, relies on union layout compatibility | Add static_assert(sizeof(phi) >= sizeof(load)) | blind_cast_phi_to_load |
| 32 | lir_optimize.c:186-187 | M | Same phi-to-load rewrite pattern at second call site | Same fix | blind_cast_phi_rewrite |
| 33 | lir_optimize.c:447-448 | M | `(Iron_EnumVariant *)ed->variants[vi]` in collect_mono_enums | Guarded by loop bound but same void* cast pattern | blind_cast_mono_enum |
| 34 | emit_c.c:1407-1417 | M | `(Iron_EnumVariant *)ged->variants[vi]` in boxed ADT field access | Guarded by variant_count loop bound | blind_cast_boxed_adt |

**Remaining M/L findings:** 171 guarded M-severity casts in HIR/comptime (all switch-case guarded), 16 L-severity structural-assumption casts in analyzer, 31 L-severity structural-assumption casts in analyzer analysis passes, 5 L-severity structural-assumption casts in parser, 13 L-severity casts in LIR, 14 L-severity casts in runtime/stdlib, 4 L-severity casts in infrastructure, 2 L-severity const-stripping casts in hir_verify.c. These are systematically the same patterns (EnumVariant, Field, Param, MatchCase array iteration casts) across all directories.

**Blind Cast Totals:** 8H, 198M, 65L = 271 findings

---

## 2. Enum Switch Exhaustiveness (AUDIT-02)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | printer.c:28 | M | `op_str` switch over Iron_TokenKind silently returns "?" for all bitwise operators (AMP, PIPE, CARET, TILDE, SHL, SHR and compound assignments) | Add cases for all bitwise operators | switch_op_str_bitwise.iron |
| 2 | hir_lower.c:326 | M | `ast_op_to_hir_binop` default silently maps unknown ops to IRON_HIR_BINOP_ADD | Return error/poison value or assert | switch_hir_binop_default.iron |
| 3 | hir_lower.c:351 | M | `compound_assign_base_op` default silently maps unknown ops to IRON_TOK_PLUS | Return error value | switch_hir_compound_default.iron |
| 4 | hir_lower.c:1175 | M | Unary op mapping default maps to IRON_HIR_UNOP_NEG | Should be assertion/error | switch_hir_unop_default.iron |
| 5 | hir_to_lir.c:463 | M | `collect_mono_enums_node` switch handles only 17 of 25+ node kinds; misses FOR, DEFER, LAMBDA, INDEX, SLICE, ARRAY_LIT, CONSTRUCT -- mono enums inside those bodies are silently missed | Add cases for missing node kinds | optimize_mono_enum_switch |
| 6 | hir_to_lir.c:1012 | M | `elem_suffix` switch over Iron_TypeKind missing FLOAT32, FLOAT64, INT8, INT16, INT64, NULLABLE, FUNC, TUPLE -- narrow-int and tuple element arrays get wrong suffix | Add missing type kind cases | switch_elem_suffix.iron |
| 7 | hir_verify.c:307 | M | verify_expr switch missing IRON_HIR_EXPR_ENUM_CONSTRUCT and IRON_HIR_EXPR_PATTERN -- their sub-expressions/nested patterns are not verified | Add cases for ENUM_CONSTRUCT and PATTERN | switch_verify_expr.iron |
| 8 | comptime.c:310 | M | `eval_expr` switch handles 13 node kinds; missing METHOD_CALL, FIELD_ACCESS, INDEX, SLICE, INTERP_STRING, LAMBDA, IS, MATCH | Emits error on hit -- safe but incomplete | switch_comptime_eval.iron |
| 9 | comptime.c:898 | M | `val_to_ast` returns NULL for CVAL_STRUCT and CVAL_NULL -- cannot round-trip struct/null comptime values | Add STRUCT val-to-ast conversion | switch_comptime_val_to_ast.iron |
| 10 | escape.c:101 | M | `collect_stmt` switch missing MATCH, DEFER, SPAWN bodies -- heap bindings inside these are missed | Add cases for MATCH, DEFER, SPAWN | switch_escape_missing_match.iron |
| 11 | escape.c:227 | M | `validate_node` switch missing MATCH, SPAWN, DEFER -- free/leak validation misses nested control flow | Add missing cases | switch_escape_validate_missing.iron |
| 12 | concurrency.c:109 | M | `collect_spawn_refs` missing CALL, METHOD_CALL, MATCH, DEFER, FREE, LEAK -- outer refs in call args inside spawn not tracked | Add missing cases | switch_conc_spawn_refs.iron |
| 13 | typecheck.c:2927 | M | `covered[256]` array silently truncates variant coverage check for enums with >256 variants | Use dynamic allocation sized to vc | switch_large_enum_coverage.iron |
| 14 | emit_helpers.c:115-206 | M | `emit_type_to_c` fallthrough returns "int" for unknown type kinds -- dangerous silent default if new types added | Add `assert(0 && "unknown type kind")` | emit_type_to_c_exhaustive |
| 15 | lir_optimize.c:302-377 | M | `analyze_array_param_modes` inner switch missing default break -- no -Wswitch coverage | Add `default: break;` for clarity | optimize_param_switch |
| 16 | lir_optimize.c:435-470 | M | `collect_mono_enums_node` in optimizer missing 8 AST node kinds -- silently misses monomorphized enums | Add missing node kind cases | optimize_mono_enum_switch |
| 17 | iron_net.c:78-131 | M | WSA/errno translation switches have default catch-all mapping to IRON_ERR_NET_UNKNOWN; may mask novel errors | Add logging in default case for debug builds | test_enum_switch_wsa_translate |

**Remaining L-severity findings:** 46 L-severity findings across all directories representing intentional partial switches with safe defaults (e.g., parser token dispatch, statement-as-expression fallthrough, verifier/printer exhaustive switches, concurrency/capture/init_check walker defaults).

**Enum Switch Totals:** 0H, 17M, 46L = 63 findings

---

## 3. Null Safety (AUDIT-03)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | typecheck.c:1360 | H | `callee_sym->decl_node` can be NULL for builtin primitive type syms (created in resolve.c with decl_node=NULL); null deref when cast to ObjectDecl | Add `if (!callee_sym->decl_node) break;` before cast | null_deref_builtin_type.iron |
| 2 | escape.c:251 | H | `(Iron_Ident *)ls->expr` -- expr_ident_name returns non-NULL from FIELD_ACCESS chain root; accessing id->resolved_sym through FieldAccess type reads wrong memory | Add explicit `ls->expr->kind == IRON_NODE_IDENT` check | null_deref_leak_ident.iron |
| 3 | emit_c.c:3082 | H | Generated HEAP_ALLOC C code does `malloc(sizeof(T))` then immediately dereferences `*_vN = val` without NULL check -- segfault on OOM in user programs | Add NULL check in generated code | null_heap_alloc_malloc |
| 4 | emit_c.c:3108-3109 | H | Generated RC_ALLOC C code: same malloc-without-NULL-check pattern as HEAP_ALLOC | Add NULL check in generated code | null_rc_alloc_malloc |
| 5 | iron_runtime.h:497 | H | IRON_LIST_IMPL `_push` calls realloc but does not check for NULL return -- if realloc fails, `self->items` becomes NULL and the next line `self->items[self->count++] = item` is a NULL dereference | Check realloc return and abort or return error | test_null_safety_list_push_oom |
| 6 | iron_runtime.h:640-641 | H | IRON_MAP_IMPL `_put` calls realloc for both keys and values without NULL check -- same NULL deref risk on OOM | Check realloc returns | test_null_safety_map_put_oom |
| 7 | parser.c:78 | M | `p->tokens[p->token_count - 1]` underflows if token_count==0 | Add `assert(p->token_count > 0)` in iron_parser_create | null_empty_token_stream.iron |
| 8 | parser.c:147-868 | M | ~12 ARENA_ALLOC calls in parser.c dereference result without NULL check (OOM path) | Add null checks or use iron_arena_alloc_or_abort wrapper | null_arena_oom.iron |
| 9 | parser.c:1632 | M | `iron_lex_all` return dereferenced without null check | Check if expr_toks is NULL | null_interp_lex.iron |
| 10 | lexer.c:363-532 | M | 5 iron_arena_strdup calls in token-producing paths unchecked -- NULL value fields cause downstream deref | Add null check; return error token on NULL | null_lexer_strdup.iron |
| 11 | typecheck.c:149 | M | `iron_symbol_create` return used without NULL check | Check for NULL | null_safety_tc_define.iron |
| 12 | typecheck.c:1581 | M | `obj_id->resolved_type->object.decl->name` triple deref without NULL checks | Add decl null check | null_deref_obj_method.iron |
| 13 | typecheck.c:3059 | M | `blk->stmts[i]->kind` without NULL check on stmts[i] | Add `if (!blk->stmts[i]) continue;` | null_deref_spawn_stmts.iron |
| 14 | typecheck.c:3113,3170 | M | `param_types` alloc result unchecked before use | Add NULL check after alloc | null_alloc_param_types.iron |
| 15 | resolve.c:42 | M | `ctx->current_scope->parent` deref without checking if current_scope is NULL | Add NULL check | null_safety_pop_scope.iron |
| 16 | hir_to_lir.c:482 | M | `blk->stmts[i]` no NULL check on individual stmts | Add `if (!blk->stmts[i]) continue;` | null_hir_blk_stmts |
| 17 | hir_to_lir.c:540 | M | `iff->elif_conds[i]`, `iff->elif_bodies[i]` no NULL check | Add NULL guards | null_hir_elif |
| 18 | hir_to_lir.c:557 | M | `mat->cases[i]` no NULL check | Depends on parser correctness | null_hir_match_cases |
| 19 | hir_verify.c:507 | M | `expr->array_lit.elements[i]` no NULL check per element | Add NULL check before verify_expr | null_hir_array_elements |
| 20 | comptime.c:28-32 | M | `cval_alloc` dereferences return of iron_arena_alloc without NULL check | arena_alloc may return NULL on OOM | null_comptime_cval |
| 21 | comptime.c:494 | M | `call->callee->kind` no NULL check on callee | Add `if (!call->callee)` guard | null_comptime_callee |
| 22 | comptime.c:802,808 | M | `is->elif_conds[i]` and `is->elif_bodies[i]` no NULL check | Depends on parser correctness | null_comptime_elif |
| 23 | comptime.c:870 | M | `iterable->as_array.elems[i]` bounded by count but not NULL-checked | Add NULL guard | null_comptime_array_elem |
| 24 | emit_helpers.c:136-147 | M | `t->object.decl->name`, `t->enu.decl->name`, `t->interface.decl->name` all dereference decl without NULL check | Add NULL guard per decl type | null_object_decl_name |
| 25 | emit_helpers.c:60-63 | M | `ctx->module->funcs[fi]` could be NULL if func array has holes | Add `if (!f) continue;` guard | null_resolve_func |
| 26 | emit_fusion.c:54,79 | M | Two calloc() return values not checked for NULL | Add check or document assumption | null_calloc_fusion |
| 27 | emit_structs.c:470 | M | Generated indirect variant malloc without NULL check | Generated function will segfault if malloc fails | null_indirect_malloc |
| 28 | capture.c:377,416,453 | M | 3 iron_arena_alloc returns not checked before use | Add NULL checks | null_alloc_capture_arr.iron |
| 29 | iface_collect.c:23,40 | M | decl accessed without NULL check in program loop | Add `if (!decl) continue;` | null_iface_decl.iron |
| 30 | iron_string.c:81,85 | M | memcpy and count_codepoints use cstr without NULL check in heap path | Add `if (cstr)` guard | test_null_safety_string_from_null |
| 31 | iron_threads.c:132-133 | M | pool_queue_grow malloc failure returns silently; caller writes to possibly-NULL queue | Return error or abort on OOM | test_null_safety_pool_queue_grow |
| 32 | iron_threads.c:237,414,700,756 | M | 4 IRON_THREAD_CREATE return values unchecked -- if pthread_create fails, thread handle is garbage | Check return values | test_null_safety_handle_create_thread |
| 33 | iron_net_init.c:49-53 | M | TOCTOU race in iron_net_ensure_wsa_lock -- two threads may both call IRON_MUTEX_INIT | Use INIT_ONCE / pthread_once | test_null_safety_wsa_init_race |
| 34 | iron_runtime.h:487-488,624-625 | M | IRON_LIST/MAP _clone calls malloc without NULL check; memcpy into NULL on OOM | Check malloc return | test_null_safety_list_clone_oom |
| 35 | arena.c:12-13 | M | arena_new_chunk malloc returns NULL; iron_arena_create does not check -- a.head could be NULL | Add NULL check in iron_arena_create | test_null_safety_arena_create_oom |
| 36 | arena.c:114-115 | M | iron_arena_track realloc NULL return not checked -- subsequent tracked_ptrs access dereferences NULL | Check realloc return | test_null_safety_arena_track_oom |
| 37 | resolver.c:42-43 | M | StringSet realloc NULL return not checked | Check realloc return | test_null_safety_resolver_set_oom |
| 38 | test_runner.c:46-48 | M | strvec_push realloc checked but returns silently on fail -- tests silently skipped | Abort or log error | test_null_safety_testrunner_strvec |

**Remaining L-severity findings:** 63 L-severity findings across all directories -- predominantly safe-by-construction patterns, defensive-only concerns, and theoretical edge cases.

**Null Safety Totals:** 6H, 63M, 63L = 132 findings

---

## 4. Arena Lifetimes (AUDIT-04)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | parser.c:372-2719 | M | 17 sites where stb_ds heap-managed arrays are stored directly into arena-allocated AST nodes without transfer -- when arena freed, stb_ds arrays leak; if stb_ds arrays freed while AST live, dangling pointers | Copy stb_ds arrays to arena allocation before storing, or document lifetime coupling | arena_func_params.iron |
| 2 | typecheck.c:693 | M | `mono_registry` stb_ds map never freed -- memory leak of hash map with arena-allocated values | Add shfree for mono_registry at typecheck end | arena_mono_registry_leak.iron |
| 3 | scope.c:17 | M | `sh_new_strdup(s->symbols)` stb_ds map allocated on heap but scope on arena -- if arena reset, stb_ds map leaks | Add shfree in scope destructor or accept as design tradeoff | arena_scope_leak.iron |
| 4 | resolve.c:749-874 | M | Built-in symbols allocated via arena stored in stb_ds hash maps -- same coupling pattern | Design tradeoff; document | arena_resolve_builtins.iron |
| 5 | hir.c:29 | M | name_table stb_ds array holds VarInfo structs with name fields pointing into AST arena strings -- if AST arena freed while HIR module alive, pointers dangle | Document: HIR module must not outlive AST arena | arena_hir_name_table |
| 6 | hir_to_lir.c:398 | M | calloc'd DF arrays for dominance frontiers -- correct but fragile if early return | Add cleanup-on-error path | arena_hir_df_cleanup |
| 7 | hir_lower.c:1088 | M | stb_ds `parts` array ownership transferred to HIR expr without arrfree -- if lower_expr_hir fails partway, parts array leaks | Add cleanup path for partial failure | arena_hir_parts_leak |
| 8 | emit_fusion.c:168,359 | M | `calloc(32, 1)` for fuse variable names -- leaked, never freed | Free at end of emit_fused_chain or use arena | arena_fuse_var_leak |
| 9 | iface_collect.c:55 | M | Registry map and impl arrays never explicitly freed -- program-lifetime leak | Add cleanup function | arena_iface_registry_leak.iron |
| 10 | iron_string.c:72-79 | M | OOM fallback in iron_string_from_cstr silently truncates to SSO_MAX bytes -- caller cannot detect truncation | Return error indicator or set flag | test_arena_string_oom_truncation |
| 11 | iron_string.c:172-176 | M | iron_string_intern frees input's heap.data if already interned -- use-after-free if caller retains pointer | Document that intern consumes input ownership | test_arena_intern_uaf |
| 12 | iron_string.c:244-252 | M | iron_runtime_shutdown frees interned string heap.data then calls shfree -- potential double-free if key aliased to value data | Verify shput always strdup's keys independently | test_arena_shutdown_double_free |
| 13 | iron_rc.c:33-48 | M | When last strong ref drops with weak_count > 0, ctrl block kept alive but value destroyed -- ctrl block leaks if all weak refs dropped without cleanup | Add weak-count decrement to free ctrl | test_arena_rc_ctrl_leak |
| 14 | iron_rc.c:57-58 | M | No iron_weak_release function to decrement weak_count -- weak refs cannot be explicitly dropped, ctrl blocks permanently leaked | Add Iron_weak_release API | test_arena_weak_release_missing |
| 15 | comptime.c:1242 | M | `replace_in_node` modifies AST in place -- replaced nodes unreachable in arena but not freed (bump allocator); benign waste | Document arena waste on comptime replacement | arena_comptime_replace |
| 16 | arena.c:110-119 | M | iron_arena_track: tracked pointers freed in iron_arena_free -- if tracked pointer aliases arena-internal data, dangling pointer | Document: tracked ptrs must not alias arena-internal data | test_arena_track_alias |

**Remaining L-severity findings:** 32 L-severity findings representing correctly-scoped arena patterns (strbuf create+free, arena alloc within function scope, stb_ds arrays properly freed, etc.).

**Arena Lifetime Totals:** 0H, 33M, 32L = 65 findings

---

## 5. Integer Safety (AUDIT-05)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | parser.c:2570 | M | `atoi(num->value)` for enum variant values -- atoi has UB on overflow; no range validation | Use strtol with range check | int_enum_value_overflow.iron |
| 2 | lexer.c:278 | M | PUSH_CHAR macro silently truncates string content at 4095 bytes without diagnostic | Emit diagnostic when string exceeds buffer or use dynamic resizing | int_lexer_string_truncation.iron |
| 3 | typecheck.c:2927 | M | `bool covered[256]` with `if (vc > 256) vc = 256` silently truncates exhaustiveness check for large enums | Use dynamic allocation sized to vc | int_safety_large_enum.iron |
| 4 | types.c:169 | M | `char buf[1024]` for tuple mangled name -- deeply nested tuples silently truncated | Check pos against sizeof(buf) and fail gracefully | int_safety_tuple_name.iron |
| 5 | hir_lower.c:425 | M | `snprintf(slot_field, sizeof(slot_field), ...)` fixed 256-byte buffer; very long nested enum variant paths silently truncated | Use dynamic allocation or check for truncation | int_hir_slot_field_trunc |
| 6 | hir_to_lir.c:1897 | M | `(int)arm->pattern->int_lit.value` truncates int64_t to int; match values > INT_MAX silently wrong | Use int64_t for case_values or check range | int_hir_match_value_trunc |
| 7 | comptime.c:106 | M | `(Iron_ComptimeValKind)kind_int` unchecked int-to-enum cast from untrusted cache file | Add bounds check | int_comptime_cache_kind |
| 8 | comptime.c:148 | M | `ftell(f) - start` where ftell can return -1 on error -- subtraction produces large positive value | Check ftell != -1 before using | int_comptime_ftell |
| 9 | comptime.c:398-401 | M | int64_t addition, subtraction, multiplication overflow is undefined behavior in C (signed) | Use overflow-safe arithmetic or __builtin_add_overflow | int_comptime_arith_overflow |
| 10 | comptime.c:474 | M | `-operand->as_int` negation of INT64_MIN is UB | Check for INT64_MIN before negation | int_comptime_neg_min |
| 11 | concurrency.c:99 | M | MAX_SPAWN_CAPTURES=64 hard cap silently stops analysis without diagnostic | Emit warning when cap hit | int_safety_spawn_cap.iron |
| 12 | init_check.c:23 | M | MAX_UNINIT_VARS=256 hard cap silently truncates analysis | Emit warning when cap hit | int_safety_uninit_cap.iron |
| 13 | init_check.c:270-276 | M | `bool before[256]; bool snapshots[256]; bool branch_snap[256]` -- 768 bytes on stack per if/else; deep nesting causes stack overflow | Use arena allocation for snapshot buffers | int_safety_stack_depth.iron |
| 14 | value_range.c:222-276 | M | `narrow_from_comparison` uses `const_val - 1` and `const_val + 1` which wrap at INT64 boundaries | Add overflow guards | int_vr_narrow_wrap |
| 15 | iron_string.c:130 | M | `total = la + lb` in iron_string_concat -- no overflow check; two strings > 2GB each wrap size_t on 32-bit | Add overflow check: `if (la > SIZE_MAX - lb)` return empty | test_int_safety_concat_overflow |
| 16 | iron_string.c:478 | M | `total = len * (size_t)n` in Iron_string_repeat -- large n wraps without check | Add overflow check before multiplication | test_int_safety_repeat_overflow |
| 17 | iron_string.c:421 | M | `total = slen + count * (newlen - oldlen)` in Iron_string_replace -- unsigned subtraction wraps; product can overflow | Use signed arithmetic or bounds-check | test_int_safety_replace_overflow |
| 18 | iron_runtime.h:497 | M | IRON_LIST_IMPL `_push` doubles capacity via `self->capacity * 2` -- overflows int64_t at INT64_MAX/2 | Add saturation check before doubling | test_int_safety_list_capacity_overflow |
| 19 | iron_runtime.h:640 | M | IRON_MAP_IMPL `_put` same capacity doubling pattern | Same fix | test_int_safety_map_capacity_overflow |
| 20 | iron_net.c:593-594 | M | `(int64_t)iron_string_byte_len(&buf)` used as recv capacity -- size_t can exceed INT_MAX on Win32 recv path | Cast to int with bounds check on Win32 | test_int_safety_recv_cap_truncate |

**Remaining findings:** 3 additional M-severity findings in types.c and resolve.c (fixed-size buffers), plus 68 L-severity findings representing safe-in-practice patterns (ptrdiff_t-to-int casts for small values, snprintf truncation of cosmetic strings, theoretical overflow in unreachable ranges).

**Integer Safety Totals:** 0H, 23M, 68L = 91 findings

---

## 6. Allocation Error Handling (AUDIT-06)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | emit_c.c:3085 | H | Generated C code for HEAP_ALLOC: `malloc(sizeof(T))` without NULL check -- emitted C will segfault if malloc returns NULL on target machine | Add NULL check in generated code before dereference | alloc_heap_generated |
| 2 | emit_c.c:3109 | H | Generated C code for RC_ALLOC: same malloc-without-NULL-check pattern in emitted C | Add NULL check in generated code | alloc_rc_generated |
| 3 | iron_runtime.h:497 | H | IRON_LIST_IMPL `_push` realloc: NULL return not checked -- writes to NULL pointer on OOM | Check realloc return | test_alloc_list_push_oom |
| 4 | iron_runtime.h:640-641 | H | IRON_MAP_IMPL `_put` realloc for keys and values: neither checked -- NULL deref on OOM | Check realloc returns | test_alloc_map_put_oom |
| 5 | parser.c:147-2735 | M | ~70 unchecked ARENA_ALLOC calls in parser.c; all dereference result without NULL check | Add null checks or use iron_arena_alloc_or_abort wrapper | alloc_parser_arena |
| 6 | parser.c (strdup) | M | ~40 unchecked iron_arena_strdup calls in parser.c | Same | alloc_parser_strdup |
| 7 | lexer.c:363-532 | M | 5 unchecked iron_arena_strdup in token-producing paths | Add null check; return error token on NULL | alloc_lexer_strdup.iron |
| 8 | typecheck.c:149-3408 | M | 26 unchecked iron_arena_alloc calls in typecheck.c | Add NULL checks | alloc_error_tc_define.iron |
| 9 | resolve.c:50-666 | M | 3 unchecked iron_symbol_create + 1 unchecked iron_arena_alloc | Add NULL checks | alloc_error_define_sym.iron |
| 10 | hir.c:12-641 | M | 54 unchecked ARENA_ALLOC calls across HIR constructors (1 malloc + 53 arena) | Add NULL checks | alloc_hir_module |
| 11 | hir_lower.c:173-various | M | 15 unchecked iron_arena_alloc calls | Add NULL checks | alloc_hir_lower |
| 12 | hir_to_lir.c:666-various | M | 10 unchecked iron_arena_alloc calls | Add NULL checks | alloc_hir_to_lir |
| 13 | comptime.c:28-955 | M | 16 unchecked iron_arena_alloc calls | Add NULL checks | alloc_comptime |
| 14 | emit_helpers.c:96-174 | M | 3 iron_strbuf_create calls (internal malloc); if fails, subsequent appendf calls are UB | Check strbuf validity | alloc_strbuf_optional |
| 15 | emit_fusion.c:54,79 | M | 2 calloc calls without NULL check -- OOM crash on dereference | Add NULL check | alloc_fusion_calloc |
| 16 | emit_split.c:85-89 | M | Generated realloc in _iron_sl_realloc_tracked checks NULL but callers don't -- silently lose data on OOM | Callers should check return | alloc_sl_realloc |
| 17 | emit_structs.c:470 | M | Generated indirect variant malloc without NULL check | Generated function segfaults if malloc fails | alloc_indirect_variant |
| 18 | lir.c-lir_optimize.c | M | 6 stb_ds arrput/hmput calls without OOM check | Standard stb_ds concern | alloc_lir_value_table |
| 19 | value_range.c-layout_analysis.c | M | 3 stb_ds hmput/shput calls without OOM check | Same | alloc_vr_stbds |
| 20 | capture.c:377-454 | M | 4 unchecked iron_arena_alloc/strdup calls | Add NULL checks | alloc_error_capture_arr.iron |
| 21 | iron_string.c:72-521 | M | 9 malloc calls in string operations that return empty string on failure -- silent data loss | Abort or propagate error | test_alloc_concat_oom |
| 22 | iron_threads.c:132-133 | M | pool_queue_grow malloc failure silently drops expansion; next enqueue writes to possibly-freed queue | Return error flag; abort if critical | test_alloc_pool_queue_grow_oom |
| 23 | iron_threads.c:332-333 | M | Iron_pool_destroy elastic snapshot malloc: if NULL, joins skipped -- leaked threads never joined | Fall back to iterating slots under lock | test_alloc_pool_destroy_snapshot_oom |
| 24 | iron_threads.c:922-923 | M | iron_threads_init does not check Iron_pool_create return; pools could be NULL | Check return and abort | test_alloc_threads_init_oom |
| 25 | iron_runtime.h:479,487,615-625 | M | IRON_LIST/MAP _create_with_capacity and _clone malloc unchecked | Check returns | test_alloc_list_create_cap_oom |
| 26 | arena.c:12-13 | M | arena_new_chunk malloc returns NULL; iron_arena_create doesn't check | Check return and set capacity=0 | test_alloc_arena_chunk_oom |
| 27 | arena.c:114 | M | iron_arena_track realloc NULL return not checked | Check return | test_alloc_arena_track_oom |
| 28 | resolver.c:42-98 | M | 2 unchecked realloc + 5 unchecked strdup calls | Check returns | test_alloc_resolver_set_oom |

**Systemic pattern:** The vast majority of M-severity allocation findings (285 total) follow two patterns: (a) unchecked iron_arena_alloc/ARENA_ALLOC calls (~250 sites across parser, analyzer, HIR, comptime), reflecting a design choice that the arena allocator never fails; (b) unchecked stb_ds dynamic array/hashmap operations (~20 sites) where realloc failure is silently swallowed by the stb_ds library design.

**Remaining L-severity findings:** 44 L-severity findings representing correctly-checked allocations or non-critical paths (diagnostic strings, arena contract guarantees, etc.).

**Allocation Error Handling Totals:** 4H, 285M, 44L = 333 findings

---

## Top-20 Must-Fix Issues

These are the highest-severity issues that MUST be resolved in Phase 67 (Correctness Fixes). Selection criteria:
1. All severity-H issues, ranked by blast radius (more code paths affected = higher rank)
2. If fewer than 20 H issues exist, fill remaining slots with the highest-impact M issues

| Rank | File:Line | Dimension | Description | Why Must-Fix | Suggested Fix | Regression Fixture |
|------|-----------|-----------|-------------|-------------|---------------|-------------------|
| 1 | iron_runtime.h:497 | Null Safety + Allocation | IRON_LIST_IMPL `_push` calls realloc without NULL check; `self->items[self->count++] = item` dereferences NULL on OOM | Crash in every Iron program that uses arrays under memory pressure -- the most exercised runtime path in the entire codebase; affects 100% of Iron programs that use list push | Check realloc return; on NULL either abort or return error; set items=old on failure | test_null_safety_list_push_oom |
| 2 | iron_runtime.h:640-641 | Null Safety + Allocation | IRON_MAP_IMPL `_put` calls realloc for keys and values without NULL check; NULL deref on OOM | Crash in every Iron program that uses maps under memory pressure -- second most exercised runtime path; affects all programs using map insertion | Check both realloc returns; on NULL either abort or return error | test_null_safety_map_put_oom |
| 3 | emit_c.c:3082-3085 | Null Safety + Allocation | Generated HEAP_ALLOC C code does `T *_vN = malloc(sizeof(T)); *_vN = val;` without NULL check | Crash in compiled Iron programs using `heap` keyword on OOM -- every heap allocation in user code is affected; no recovery possible | Emit NULL check + abort/error after malloc in generated code | null_heap_alloc_malloc |
| 4 | emit_c.c:3108-3109 | Null Safety + Allocation | Generated RC_ALLOC C code: same malloc-without-NULL-check pattern | Crash in compiled Iron programs using `rc` keyword on OOM -- every reference-counted allocation in user code is affected | Same fix as rank 3: emit NULL check in generated code | null_rc_alloc_malloc |
| 5 | typecheck.c:1360 | Blind Cast + Null Safety | `(Iron_ObjectDecl *)callee_sym->decl_node` when callee_sym is SYM_TYPE -- decl_node can be NULL (builtin primitives) or point to InterfaceDecl/EnumDecl | Silent memory misinterpretation or NULL deref when user code constructs interface/enum types via constructor syntax; wrong field offsets read from InterfaceDecl disguised as ObjectDecl | Add `if (!callee_sym->decl_node) break;` then `if (callee_sym->decl_node->kind != IRON_NODE_OBJECT_DECL) break;` | blind_cast_type_sym_decl.iron |
| 6 | typecheck.c:1785 | Blind Cast | `(Iron_ObjectDecl *)sym->decl_node` in CONSTRUCT handler -- identical pattern to line 1360 | Same user-visible consequence: UB when constructing non-object types; affects any code using enum/interface in constructor position | Same fix: add kind check before cast | blind_cast_type_sym_decl.iron |
| 7 | resolve.c:674 | Blind Cast | In-place reinterpretation of `Iron_EnumConstruct` as `Iron_MethodCallExpr` -- reads/writes memory at offsets that differ between the two struct types | Layout-dependent UB: if struct layouts diverge (e.g., after adding fields), user code with qualified enum variant names (`Enum.Variant(args)`) silently corrupts memory | Allocate fresh MethodCallExpr node or add _Static_assert on layout compatibility | enum_construct_reinterpret.iron |
| 8 | resolve.c:680 | Blind Cast | In-place reinterpretation of `Iron_EnumConstruct` as `Iron_FieldAccess` -- same dangerous pattern as line 674 | Same UB risk for parameterless enum variant access (`Enum.Variant`); affects every enum variant reference in user code | Allocate fresh FieldAccess node | enum_construct_reinterpret.iron |
| 9 | hir_lower.c:109 | Blind Cast | `((Iron_ExprNode *)node)->resolved_type` uses locally-defined struct assuming `{span, kind, resolved_type}` prefix layout across all AST expression types | If any AST node type has a different field order, this reads garbage for resolved_type -- the entire HIR lowering pass depends on this assumption; incorrect type propagation causes wrong codegen | Add _Static_assert for each AST expr type that offset of resolved_type matches Iron_ExprNode | blind_cast_expr_common_layout.iron |
| 10 | resolve.c:236,251 | Blind Cast | `(Iron_ObjectDecl *)owner_sym->decl_node` fails when owner is an enum type using `super` keyword | Crash or UB when enum methods reference `super` -- affects any enum method using self/super pattern | Add `decl_node->kind` check before cast | blind_cast_owner_decl.iron |
| 11 | typecheck.c:3064 | Blind Cast | `(Iron_IntLit *)rs->value` reads resolved_type via wrong concrete type -- aliasing-unsafe across all AST node types | UB under strict aliasing: compiler may optimize assuming IntLit fields, but actual node could be any expression type; affects spawn return type inference | Use proper union or generic access for resolved_type field | blind_cast_expr_resolved_type.iron |
| 12 | escape.c:251 | Blind Cast + Null Safety | `(Iron_Ident *)ls->expr` without kind check when expr_ident_name returns non-NULL from FieldAccess chain | Reads resolved_sym field from FieldAccess memory layout disguised as Ident -- wrong symbol resolution for leak statements with field access expressions | Add `ls->expr->kind == IRON_NODE_IDENT` guard before cast | blind_cast_leak_ident.iron |
| 13 | hir_to_lir.c:463 | Enum Switch | `collect_mono_enums_node` handles only 17 of 25+ node kinds; misses FOR, LAMBDA, INDEX, SLICE, ARRAY_LIT, CONSTRUCT, DEFER | Monomorphized enum types used inside for-loop bodies, lambda captures, array literals, and constructor args are silently missed; generated code uses generic dispatch instead of optimized paths | Add switch cases for all missing node kinds that can contain expressions | optimize_mono_enum_switch |
| 14 | comptime.c:398-401 | Integer Safety | Signed int64_t addition, subtraction, multiplication in comptime evaluator without overflow checks -- undefined behavior in C | Comptime expressions like `val x = comptime { 9223372036854775807 + 1 }` trigger signed integer overflow UB; compiler behavior undefined (crash, wrong result, or optimized-away check) | Use __builtin_add_overflow or manual overflow-safe wrappers | int_comptime_arith_overflow |
| 15 | comptime.c:474 | Integer Safety | `-operand->as_int` where operand can be INT64_MIN -- negation of INT64_MIN is UB | `comptime { -(-9223372036854775808) }` triggers UB; same consequences as rank 14 | Check for INT64_MIN before negation; return error | int_comptime_neg_min |
| 16 | iron_net.c:594 | Blind Cast | `(uint8_t *)(uintptr_t)base` casts away const from iron_string_cstr() return to use as recv buffer -- writes into immutable Iron_String SSO data | Data corruption: recv() writes into the read-only SSO buffer of an Iron_String, corrupting the string's inline storage; affects all TCP read operations | Use a local mutable buffer for recv, then copy into a new Iron_String | test_blind_cast_recv_into_string |
| 17 | hir_to_lir.c:1012 | Enum Switch | `elem_suffix` switch missing FLOAT32, FLOAT64, INT8, INT16, INT64, NULLABLE, FUNC, TUPLE type kinds | Arrays of narrow integers (Int8, Int16), specific floats (Float32), nullable types, function types, and tuples get wrong element suffix in generated C code -- silent wrong output | Add cases for all missing Iron_TypeKind values | switch_elem_suffix.iron |
| 18 | resolve.c:674,680 related | Blind Cast | The in-place AST node reinterpretation pattern in resolve.c depends on exact struct layout matching between Iron_EnumConstruct, Iron_MethodCallExpr, and Iron_FieldAccess | If any field is added/removed from these structs, all enum variant resolution silently breaks; this is the most fragile pattern in the entire codebase and has zero compile-time or runtime safety | Add _Static_assert on sizeof and critical field offsets, or migrate to fresh allocation | enum_construct_layout_assert.iron |
| 19 | parser.c:2570 | Integer Safety | `atoi(num->value)` for explicit enum variant values -- atoi has undefined behavior on overflow and no range validation | User-provided enum variant value like `variant X = 99999999999999` triggers atoi UB (overflow); compiler produces wrong enum tag | Use strtol with ERANGE check and INT_MAX/INT_MIN bounds validation | int_enum_value_overflow.iron |
| 20 | emit_helpers.c:115-206 | Enum Switch | `emit_type_to_c` falls through to `return "int"` for unknown Iron_TypeKind values -- if a new type kind is added, it silently maps to C `int` | Adding a new type kind to Iron (e.g., a new numeric width or composite type) silently generates wrong C code -- the emitted type is `int` instead of the correct C representation | Add `assert(0 && "unknown type kind")` or exhaustive case coverage | emit_type_to_c_exhaustive |

---

## AUDIT-07: Runtime + Stdlib Audit Summary

This section summarizes findings specific to runtime (src/runtime/) and stdlib (src/stdlib/) files, as required by AUDIT-07.

### Runtime + Stdlib Totals (excluding infrastructure)

| Dimension | High | Medium | Low | Total |
|-----------|------|--------|-----|-------|
| Blind Casts (AUDIT-01) | 0 | 1 | 10 | 11 |
| Enum Switch (AUDIT-02) | 0 | 2 | 4 | 6 |
| Null Safety (AUDIT-03) | 2 | 8 | 14 | 24 |
| Arena Lifetimes (AUDIT-04) | 0 | 4 | 2 | 6 |
| Integer Safety (AUDIT-05) | 0 | 6 | 8 | 14 |
| Allocation Error Handling (AUDIT-06) | 2 | 16 | 14 | 32 |
| Cross-Platform (AUDIT-08) | 9 | 7 | 14 | 30 |
| **Total** | **13** | **44** | **66** | **123** |

### Key Runtime/Stdlib Findings

1. **IRON_LIST_IMPL / IRON_MAP_IMPL unchecked realloc** (H, AUDIT-03/06): `_push` and `_put` can NULL-deref on OOM. These macros in iron_runtime.h are the most exercised runtime paths -- every Iron array push and map insertion flows through them.

2. **iron_string.c OOM handling**: 9 string operations silently return empty strings or truncated data on malloc failure. The `iron_string_from_cstr` heap path truncates to SSO_MAX bytes without error indication.

3. **iron_threads.c unchecked thread creation**: 4 `IRON_THREAD_CREATE` calls (pool_create, elastic_spawn, handle_create, handle_create_self_ref) do not check return values. Failed thread creation leaves garbage thread handles.

4. **iron_rc.c weak reference leak**: No `iron_weak_release` API exists. Ctrl blocks for objects that ever had weak references are permanently leaked (even after all references dropped).

5. **iron_net.c recv-into-immutable-string**: `Iron_tcpsocket_read` casts away const from `iron_string_cstr()` return and passes it as a recv buffer, writing into the read-only SSO storage of an Iron_String.

6. **Cross-platform gaps**: 9 H-severity POSIX-only APIs in runtime/stdlib (clock_gettime, nanosleep, localtime_r, pthread-specific paths) with no Windows fallbacks. These are documented in CROSS-PLATFORM-DEBT.md.
