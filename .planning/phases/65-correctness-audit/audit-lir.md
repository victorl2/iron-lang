# LIR Directory Correctness Audit

Audit of all 11 .c files in `src/lir/` (15,818 lines total).

Files audited:
- emit_c.c (6600 lines) -- Main IR-to-C emission backend
- emit_helpers.c (460 lines) -- Shared helpers, name mangling, EmitCtx cleanup
- emit_structs.c (829 lines) -- Type declarations, tagged unions, split collection structs
- emit_split.c (622 lines) -- Split collection prescan, arena helpers, per-interface emission
- emit_fusion.c (413 lines) -- Fused loop emission for chained collection operations
- lir.c (568 lines) -- LIR module/function/block/instruction constructors
- lir_optimize.c (3619 lines) -- Optimization passes (phi elimination, copy-prop, DCE, etc.)
- layout_analysis.c (688 lines) -- Field access analysis for dead-field elimination
- value_range.c (772 lines) -- Value range analysis for field compression
- verify.c (588 lines) -- LIR verifier (operand validity, block connectivity)
- print.c (659 lines) -- LIR pretty printer

---

## 1. Blind Casts (AUDIT-01)

| # | File:Line | Cast | Guard | Severity | Suggested Fix | Regression Fixture |
|---|-----------|------|-------|----------|---------------|-------------------|
| 1 | emit_c.c:509 | `instr->construct.type->object.decl` cast via kind check | Guarded by `kind == IRON_TYPE_OBJECT` | L | Safe -- union access gated by kind | blind_cast_construct_object |
| 2 | emit_c.c:517-518 | `instr->construct.type->enu.decl` accessed after `kind == IRON_TYPE_ENUM` | Guarded by enum kind check | L | Safe -- correctly gated | blind_cast_construct_enum |
| 3 | emit_c.c:533 | `(Iron_EnumVariant *)adt_ed->variants[variant_idx]` | Guarded by `variant_idx < adt_ed->variant_count` bounds check | M | Cast from `void*` array -- relies on correct storage. Add assert for variant_count > 0 | blind_cast_enum_variant |
| 4 | emit_c.c:3154 | `(Iron_EnumVariant *)adt_ed->variants[variant_idx]` in CONSTRUCT statement | Same guard pattern as above | M | Same concern -- ensure variant array populated correctly | blind_cast_construct_variant |
| 5 | emit_structs.c:57-58 | `td->type->object.decl` access | Guarded by `td->kind == IRON_LIR_TYPE_OBJECT && td->type->kind == IRON_TYPE_OBJECT` | L | Double-guarded, safe | blind_cast_topo_visit |
| 6 | emit_structs.c:67 | `(Iron_Field *)od->fields[i]` | No kind check; relies on ObjectDecl.fields being Iron_Field* | M | Cast from `void*` field array. If ObjectDecl changes, this silently breaks. Add `_Static_assert` on field layout | blind_cast_field_access |
| 7 | emit_structs.c:69 | `(Iron_TypeAnnotation *)f->type_ann` | Relies on field->type_ann being TypeAnnotation | M | Same pattern as #6 -- void* to concrete type without validation | blind_cast_type_ann |
| 8 | emit_structs.c:244 | `od = td->type->object.decl` after kind checks | Guarded by `td->type && td->type->kind == IRON_TYPE_OBJECT && td->type->object.decl` | L | Triple-guarded, safe | blind_cast_struct_body |
| 9 | emit_structs.c:259-260 | `(Iron_Field *)od->fields[i]` and `(Iron_TypeAnnotation *)ta` | Same pattern as #6, #7 repeated in emit_object_struct_body | M | Same concern -- add compile-time field layout assertion | blind_cast_struct_fields |
| 10 | emit_structs.c:315 | `(Iron_Field *)od->fields[i]` in emit_estimate_type_size | Unguarded void* -> Iron_Field* | M | Same pattern. No runtime check possible since fields is `void**` | blind_cast_estimate_size |
| 11 | emit_split.c:31-38 | `in2->array_lit.elem_type->interface.decl` | Guarded by `kind == IRON_TYPE_INTERFACE` check | L | Correctly gated | blind_cast_split_iface |
| 12 | emit_fusion.c:58-59 | `node_instr = fn->value_table[node->call_vid]` | Bounds-checked but cast relies on instruction being correct kind | L | Read-only access, value_table entries are always IronLIR_Instr* | blind_cast_fusion_node |
| 13 | lir_optimize.c:109-110 | `phi->kind = IRON_LIR_LOAD; phi->load.ptr = alloca_id` -- in-place mutation of phi to load | Not a cast but a reinterpretation of the instruction union | M | In-place instruction mutation relies on union layout compatibility between PHI and LOAD members | blind_cast_phi_to_load |
| 14 | lir_optimize.c:640-656 | Switch over `instr->kind` accesses `instr->binop`, `instr->unop` etc. per-case | Each case accesses the correct union member | L | Standard tagged-union pattern, safe | blind_cast_optimize_switch |
| 15 | layout_analysis.c:86-87 | `fn->value_table[ptr_vid]->kind == IRON_LIR_ALLOCA` | Bounds-checked via `ptr_vid < fn->next_value_id` and NULL check | L | Safe -- guarded correctly | blind_cast_layout_alloca |
| 16 | value_range.c:121-126 | `fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF` then access `func_ref.func_name` | Guarded by kind check | L | Standard union access pattern | blind_cast_vr_funcref |
| 17 | verify.c:107-108 | `instr->call.func_ptr` accessed when `instr->call.func_decl == NULL` | Guarded by IRON_LIR_CALL case and null check | L | Safe -- verifier correctly handles both call modes | blind_cast_verify_call |
| 18 | emit_c.c:1407-1417 | `(Iron_EnumVariant *)ged->variants[vi]` in boxed ADT field access | Guarded by variant_count loop bound | M | Same void* cast pattern as #3 | blind_cast_boxed_adt |

---

## 2. Enum Switch Exhaustiveness (AUDIT-02)

### IronLIR_OpKind Enum (44 values: CONST_INT through POISON, plus INSTR_COUNT)

| # | File:Line | Switch Subject | Missing Cases | Severity | Suggested Fix | Regression Fixture |
|---|-----------|---------------|---------------|----------|---------------|-------------------|
| 1 | emit_c.c:159-681 | emit_expr_to_buf switch | CONST_STRING, ALLOCA, STORE, SET_FIELD, SET_INDEX, JUMP, BRANCH, SWITCH, RETURN, HEAP_ALLOC, RC_ALLOC, FREE, ARRAY_LIT, INTERP_STRING, SPAWN, PARALLEL_FOR, AWAIT, PHI, POISON, INSTR_COUNT | L | Non-expression instructions correctly fall through to `default: emit_val()`. Not all opcodes produce inlineable expressions | emit_expr_exhaustive |
| 2 | emit_c.c:707-4153 | emit_instr main switch | All 44 opcodes handled including INSTR_COUNT sentinel | L | Fully exhaustive. All cases present including POISON and INSTR_COUNT | emit_instr_exhaustive |
| 3 | lir.c:65-93 | iron_lir_module_destroy switch for per-instr cleanup | Missing: HEAP_ALLOC, RC_ALLOC, FREE, CAST, JUMP, BRANCH, RETURN, ALLOCA, LOAD, STORE, GET_FIELD, SET_FIELD, GET_INDEX, SET_INDEX, all binops, all unops, all consts, FUNC_REF, IS_NULL, IS_NOT_NULL, SLICE, AWAIT, POISON | L | Missing cases fall to `default: break`. Only instructions with stb_ds arrays need cleanup. Safe -- no leaks from missing cases since those instructions use arena memory | lir_destroy_exhaustive |
| 4 | lir_optimize.c:302-377 | analyze_array_param_modes inner switch | Handles: GET_INDEX, SET_INDEX, GET_FIELD, STORE, CALL, RETURN, SET_FIELD, CONSTRUCT, MAKE_CLOSURE, SLICE. Missing: all others | M | The `default:` is intentionally absent -- non-matching instructions continue the loop. Consider adding `default: break;` for clarity and -Wswitch coverage | optimize_param_switch |
| 5 | lir_optimize.c:640-790 | is_pure/copy-prop switch | Covers all opcodes via grouped cases with `default:` fallthrough | L | Fully exhaustive with default | optimize_pure_switch |
| 6 | verify.c:35-200 | collect_operands switch | Covers all opcodes: all 44 values handled individually | L | Fully exhaustive -- the verifier handles every instruction kind | verify_operands_exhaustive |
| 7 | print.c:38-548 | print_instr switch | Covers all opcodes including PHI and POISON | L | Fully exhaustive | print_instr_exhaustive |

### Iron_TypeKind Switches

| # | File:Line | Switch Subject | Missing Cases | Severity | Suggested Fix | Regression Fixture |
|---|-----------|---------------|---------------|----------|---------------|-------------------|
| 8 | emit_helpers.c:115-206 | emit_type_to_c switch over Iron_TypeKind | Covers: INT, INT8-64, UINT, UINT8-64, FLOAT, FLOAT32, FLOAT64, BOOL, STRING, VOID, NULL, ERROR, OBJECT, ENUM, INTERFACE, NULLABLE, RC, FUNC, ARRAY, GENERIC_PARAM, TUPLE. Falls through to `return "int"` | M | The fallthrough `return "int"` is an unreachable-but-dangerous default. If a new type kind is added, it silently maps to `int`. Add `assert(0 && "unknown type kind")` | emit_type_to_c_exhaustive |
| 9 | emit_c.c:3462-3488 | INTERP_STRING format specifier switch over type->kind | Handles INT/INT8-64, UINT/UINT8-64, FLOAT/FLOAT32/FLOAT64, BOOL. Default: `%s` | L | Unhandled types treated as strings via `%s` -- reasonable default for interpolation | interp_string_type_switch |
| 10 | emit_structs.c:262-297 | emit_object_struct_body field type emission | Not a switch; uses if-chain on TypeAnnotation flags (is_func, is_nullable, is_array) | L | Not an enum switch -- sequential if-else chain covers all annotation modes | struct_body_type_chain |

---

## 3. Null Safety (AUDIT-03)

| # | File:Line | Dereference | Null Check | Severity | Suggested Fix | Regression Fixture |
|---|-----------|-------------|-----------|----------|---------------|-------------------|
| 1 | emit_c.c:42 | `ctx->opt_info->stack_array_ids` | Only checks `!ctx->opt_info->stack_array_ids` but not `ctx->opt_info` itself | L | ctx->opt_info is initialized before any emission; caller contract ensures non-NULL | null_opt_info |
| 2 | emit_c.c:144-145 | `fn->value_table[vid]` in emit_expr_to_buf | Bounds-checked via `vid >= arrlen(fn->value_table)` and NULL check | L | Correctly guarded | null_value_table_expr |
| 3 | emit_c.c:509 | `instr->construct.type->object.decl` | Guarded by `instr->construct.type && kind == IRON_TYPE_OBJECT && decl` | L | Safe | null_construct_type |
| 4 | emit_c.c:1349 | `fn->value_table[instr->field.object]` in type-ref check | Preceding `emit_val_is_type_ref` returns false if vid out of bounds | L | Safe -- type_ref check guards the access | null_type_ref_field |
| 5 | emit_c.c:2276 | `sp_entry->impls[ji]` in pop dispatch | `sp_entry` was resolved from iface_reg and checked non-NULL | L | Safe | null_pop_impl |
| 6 | emit_c.c:3276 | `instr->array_lit.elem_type->interface.decl->name` | Guarded by `is_iface_array` which checks elem_type, kind, and decl non-NULL | L | Correctly guarded | null_array_lit_iface |
| 7 | emit_helpers.c:60-63 | `ctx->module->funcs[fi]` in emit_resolve_func_c_name | Loop iterates `ctx->module->func_count` -- no explicit NULL check on `ctx->module` | M | ctx->module is a required field, but `ctx->module->funcs[fi]` could be NULL if func array has holes. Add `if (!f) continue;` guard | null_resolve_func |
| 8 | emit_helpers.c:136-137 | `t->object.decl->name` in emit_type_to_c IRON_TYPE_OBJECT | No check on `t->object.decl` being NULL | M | If `t->object.decl` is NULL, this dereferences NULL. Add `if (!t->object.decl) return "void"` guard | null_object_decl_name |
| 9 | emit_helpers.c:139-142 | `t->enu.mangled_name` then `t->enu.decl->name` | `t->enu.decl` not checked for NULL before accessing `->name` | M | Fallthrough to `emit_mangle_name(t->enu.decl->name, ...)` will crash if `t->enu.decl` is NULL | null_enum_decl_name |
| 10 | emit_helpers.c:147 | `t->interface.decl->name` | No NULL check on `t->interface.decl` | M | Same pattern as #8, #9 -- add guard | null_iface_decl_name |
| 11 | emit_structs.c:56-58 | `td->type->object.decl` in ir_topo_visit | Guarded by triple-check: `td->type && kind == IRON_TYPE_OBJECT && td->type->object.decl` | L | Safe | null_topo_visit |
| 12 | emit_split.c:115 | `entry->iface_name` used in strlen | No NULL check on iface_name | L | iface_name populated during registration; NULL would indicate registry corruption | null_iface_name |
| 13 | emit_fusion.c:54 | `calloc()` return value | Not checked for NULL | M | calloc may return NULL on OOM. Add check or document assumption | null_calloc_fusion |
| 14 | emit_fusion.c:79 | `calloc()` for node_in_type | Not checked for NULL | M | Same as #13 | null_calloc_fusion2 |
| 15 | lir.c:10 | `ARENA_ALLOC(fn->arena, IronLIR_Instr)` | Arena allocations never return NULL (arena design) | L | Safe by arena contract | null_arena_alloc |
| 16 | lir_optimize.c:64-78 | `ARENA_ALLOC(fn->arena, IronLIR_Instr)` in make_alloca_instr | Same arena contract | L | Safe | null_optimize_arena |
| 17 | layout_analysis.c:31-33 | `snprintf` with `collection_vid` and `field_name` | field_name checked non-NULL at call site (line 104) | L | Safe | null_layout_field |
| 18 | value_range.c:69 | `__builtin_add_overflow(a.min, b.min, &lo)` | Not a null issue but values checked for is_top before arithmetic | L | Safe -- TOP returns early | null_vr_overflow |
| 19 | emit_c.c:3082 | `malloc(sizeof(...))` in HEAP_ALLOC | No NULL check on malloc return | H | malloc can return NULL; generated C code dereferences immediately on next line: `*_vN = inner_val`. Add NULL check or document | null_heap_alloc_malloc |
| 20 | emit_c.c:3108-3109 | `malloc(sizeof(...))` in RC_ALLOC | No NULL check on malloc return | H | Same as #19 -- generated C has immediate dereference after malloc | null_rc_alloc_malloc |
| 21 | emit_structs.c:470 | `malloc(sizeof(...))` in indirect variant wrapping constructor | No NULL check in generated static inline function | M | Generated code could segfault if malloc returns NULL | null_indirect_malloc |

---

## 4. Arena Lifetimes (AUDIT-04)

| # | File:Line | Pattern | Severity | Suggested Fix | Regression Fixture |
|---|-----------|---------|----------|---------------|-------------------|
| 1 | emit_helpers.c:96-109 | `iron_strbuf_create` + `iron_strbuf_free` in emit_optional_struct_name | Correctly freed; result arena-strdup'd before free | L | Safe pattern | arena_optional_name |
| 2 | emit_helpers.c:155-162 | Same strbuf+free pattern in emit_type_to_c for IRON_TYPE_RC | Result arena-strdup'd before free | L | Safe | arena_rc_type |
| 3 | emit_helpers.c:174-193 | Same pattern for IRON_TYPE_ARRAY | Result arena-strdup'd before free | L | Safe | arena_array_type |
| 4 | emit_c.c:3446-3447 | `iron_strbuf_create` for fmt_sb and args_sb in INTERP_STRING | Both freed at end of block (lines 3603-3604 approx.) | L | Safe -- block-scoped | arena_interp_string |
| 5 | emit_fusion.c:168-170 | `calloc(32, 1)` for new_var (_fuse_v%d) | Leaked -- never freed | M | Small leak per fusion chain map operation. Free at end of emit_fused_chain or use arena | arena_fuse_var_leak |
| 6 | emit_fusion.c:359-360 | Same calloc leak in split fusion path | Same issue as #5 | M | Same fix | arena_fuse_var_leak2 |
| 7 | emit_split.c:127-133 | `iface_collection_vids` stb_ds array | Freed at line 621 via `arrfree` | L | Correctly freed | arena_split_vids |
| 8 | emit_structs.c:119 | `emitted_mono_list_types` stb_ds hashmap | Freed at line 218 via `shfree` | L | Correctly freed | arena_mono_list_map |
| 9 | emit_c.c:420-460 | `emit_ctx_cleanup` frees all stb_ds maps and strbufs | Comprehensive cleanup | L | Covers all EmitCtx fields | arena_ctx_cleanup |
| 10 | lir.c:51-112 | `iron_lir_module_destroy` frees all stb_ds arrays per instruction | Comprehensive cleanup | L | Covers all dynamic arrays | arena_module_destroy |
| 11 | lir_optimize.c:103 | `phis` stb_ds array in phi_eliminate | Freed at line 192 via `arrfree` | L | Correctly freed | arena_phi_phis |
| 12 | lir_optimize.c:275 | `aliases` stb_ds hashmap | Freed at end of inner loop scope | L | Correctly freed | arena_optimize_aliases |

---

## 5. Integer Safety (AUDIT-05)

| # | File:Line | Operation | Severity | Suggested Fix | Regression Fixture |
|---|-----------|-----------|----------|---------------|-------------------|
| 1 | emit_structs.c:311-327 | `emit_estimate_type_size` accumulates field sizes | L | Uses `int` accumulator. Max realistic total << INT_MAX. No overflow risk for practical types | int_estimate_size |
| 2 | emit_structs.c:410-411 | Variant size comparison: `largest_size > 2 * smallest_size` | L | Both are `int` from estimate_type_size. Max ~100 fields * 24 bytes = 2400. No overflow risk | int_variant_size |
| 3 | emit_structs.c:434 | `snprintf(ikey_buf, sizeof(ikey_buf), ...)` with 512-byte buffer | L | iface_mangled + type_name unlikely to exceed 512 bytes. Truncation is silent but harmless | int_snprintf_ikey |
| 4 | emit_split.c:289 | `snprintf(soa_key_tmp, sizeof(soa_key_tmp), ...)` with 768-byte buffer | L | Same pattern -- generous buffer | int_snprintf_soa |
| 5 | emit_helpers.c:23-24 | `size_t total = 5 + len + 1` in emit_mangle_name | L | len is from strlen; total cannot overflow on 64-bit | int_mangle_name |
| 6 | emit_helpers.c:319 | `snprintf(buf, san_len + 16, ...)` in emit_make_block_label | L | san_len from strlen; +16 for "b%d" suffix. Safe | int_block_label |
| 7 | emit_c.c:1689 | `iron_strbuf_appendf(sb, "[%lld]", (long long)fill_count)` | L | fill_count bounded to [1,256] by preceding check | int_fill_count |
| 8 | value_range.c:66-71 | `range_add` uses `__builtin_add_overflow` | L | Correctly uses compiler overflow intrinsics, returns TOP on overflow | int_vr_add_safe |
| 9 | value_range.c:74-79 | `range_sub` uses `__builtin_sub_overflow` | L | Same safe pattern | int_vr_sub_safe |
| 10 | value_range.c:83-97 | `range_mul` uses `__builtin_mul_overflow` for all 4 products | L | Correctly handles all corner cases | int_vr_mul_safe |
| 11 | layout_analysis.c:32 | `snprintf(buf, sizeof(buf), "%u:%s", ...)` with 256-byte buffer | L | collection_vid is uint32 (max 10 digits), field_name is identifier. 256 bytes sufficient | int_layout_key |
| 12 | layout_analysis.c:265 | `char lower_type[256]` with `tl >= sizeof(lower_type)` truncation | L | Type names in practice are short identifiers. Truncation would only affect matching | int_layout_lowercase |
| 13 | emit_c.c:4180 | `int param_val_id = i + 1` for param iteration | L | param_count is small (< 100 in practice). No overflow risk | int_param_id |

---

## 6. Allocation Error Handling (AUDIT-06)

| # | File:Line | Allocation | Error Check | Severity | Suggested Fix | Regression Fixture |
|---|-----------|------------|-------------|----------|---------------|-------------------|
| 1 | emit_helpers.c:24 | `iron_arena_alloc(arena, total, 1)` | No check -- arena design uses abort-on-OOM | L | Arena contract: never returns NULL | alloc_arena_mangle |
| 2 | emit_helpers.c:84 | `iron_arena_alloc(arena, len + 1, 1)` in emit_sanitize_label | Same arena contract | L | Safe | alloc_arena_sanitize |
| 3 | emit_helpers.c:96 | `iron_strbuf_create(64)` | strbuf calls malloc internally; no OOM check | M | Iron_StrBuf allocates with malloc. If malloc fails, strbuf.data is NULL and subsequent appendf calls are UB | alloc_strbuf_optional |
| 4 | emit_helpers.c:155 | `iron_strbuf_create(64)` in IRON_TYPE_RC | Same concern as #3 | M | Same fix | alloc_strbuf_rc |
| 5 | emit_helpers.c:174 | `iron_strbuf_create(64)` in IRON_TYPE_ARRAY | Same concern | M | Same fix | alloc_strbuf_array |
| 6 | emit_c.c:3085 | Generated `malloc(sizeof(...))` for HEAP_ALLOC | Generated code has no NULL check | H | Emitted C code will segfault if malloc returns NULL on target machine | alloc_heap_generated |
| 7 | emit_c.c:3109 | Generated `malloc(sizeof(...))` for RC_ALLOC | Same as #6 | H | Same issue | alloc_rc_generated |
| 8 | emit_structs.c:355-356 | `iron_arena_alloc` for topo sort colors array | Arena contract | L | Safe | alloc_topo_colors |
| 9 | emit_split.c:85-89 | Generated `realloc()` in _iron_sl_realloc_tracked | Generated code checks `if (!p) return NULL` but callers don't check | M | Callers of _iron_sl_realloc_tracked silently lose data on OOM | alloc_sl_realloc |
| 10 | emit_split.c:83 | Generated `realloc()` in _iron_sl_track | Generated code checks NULL but callers don't | M | Same pattern as #9 | alloc_sl_track |
| 11 | emit_fusion.c:54 | `calloc(chain->node_count, sizeof(char*))` | No NULL check | M | OOM would crash on subsequent dereference | alloc_fusion_calloc |
| 12 | emit_fusion.c:79 | `calloc(chain->node_count, sizeof(char*))` | No NULL check | M | Same issue | alloc_fusion_calloc2 |
| 13 | lir.c:10 | `ARENA_ALLOC(fn->arena, IronLIR_Instr)` | Arena contract -- never NULL | L | Safe | alloc_lir_instr |
| 14 | lir_optimize.c:64 | `ARENA_ALLOC(fn->arena, IronLIR_Instr)` in make_alloca_instr | Arena contract | L | Safe | alloc_optimize_alloca |
| 15 | value_range.c:32 | implicit stb_ds hmput/shput allocations | stb_ds uses realloc internally; no OOM check | M | stb_ds design: realloc failure is silent data loss | alloc_vr_stbds |
| 16 | layout_analysis.c:42 | `shput(la->field_used, key, true)` | stb_ds allocation | M | Same stb_ds concern | alloc_layout_shput |
| 17 | emit_structs.c:470 | Generated `malloc(sizeof(...))` in indirect variant constructor | No NULL check in generated inline function | M | Generated function will segfault if malloc fails | alloc_indirect_variant |

---

## 7. Cross-Platform (AUDIT-08)

| # | File:Line | Assumption | Severity | Suggested Fix | Regression Fixture |
|---|-----------|-----------|----------|---------------|-------------------|
| 1 | emit_c.c:3085 | Generated C uses `malloc` / `free` -- standard C, portable | L | No issue | xplat_malloc |
| 2 | emit_c.c:3718 | Generated C uses `alloca()` for VLA fill arrays | M | `alloca` is not standard C; not available on all platforms (e.g., some MSVC configurations). Consider `_alloca` on Windows or a fallback | xplat_alloca |
| 3 | emit_c.c:390-391 | `(long long)instr->const_int.value` with `%lld` format | L | int64_t-to-long-long cast is safe on all platforms where long long is >= 64 bits (guaranteed by C99) | xplat_lld |
| 4 | emit_helpers.c:119-131 | Generated C type names: `int64_t`, `uint8_t`, etc. | L | Standard `<stdint.h>` types -- fully portable | xplat_stdint |
| 5 | emit_c.c:3382 | `Iron_List` (unparameterized) in SLICE emission | M | Hardcoded `Iron_List` instead of parameterized list type. Slice on non-int arrays would produce wrong type | xplat_slice_type |
| 6 | emit_structs.c:311-327 | `emit_estimate_type_size` assumes String=16, Closure=16, arrays=24, others=8 | M | These sizes are compile-time estimates for variant split decisions, not runtime. They may not match actual struct layout on all platforms (e.g., 32-bit systems) | xplat_type_sizes |
| 7 | emit_split.c:85 | Generated `realloc` in tracked allocator | L | Standard C -- portable | xplat_realloc |
| 8 | emit_c.c:3741 | Generated `iron_string_from_literal` assumes literal has lifetime matching string struct | L | Iron_String design uses SSO; literals under 23 bytes are copied inline | xplat_sso |
| 9 | value_range.c:69-90 | `__builtin_add_overflow`, `__builtin_sub_overflow`, `__builtin_mul_overflow` | M | GCC/Clang builtins -- not available on MSVC. Need `#ifdef _MSC_VER` fallback with manual overflow checks | xplat_builtin_overflow |
| 10 | emit_c.c:3673 | Generated closure dispatch: `(ret (*)(void*, ...))fn` variadic cast | M | Casting function pointer to variadic type is implementation-defined in C. Works on x86-64 and ARM64 calling conventions but technically non-portable | xplat_variadic_cast |

---

