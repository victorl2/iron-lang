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

## LIR Core Files -- Additional Findings

The following findings supplement the per-dimension tables above with items specific to the non-emit LIR core files: lir.c, lir_optimize.c, layout_analysis.c, value_range.c, verify.c, and print.c.

### Blind Casts -- LIR Core

| # | File:Line | Cast | Guard | Severity | Suggested Fix | Regression Fixture |
|---|-----------|------|-------|----------|---------------|-------------------|
| 19 | lir_optimize.c:186-187 | Phi in-place to LOAD: `phi->kind = IRON_LIR_LOAD; phi->load.ptr = alloca_id` | Relies on union member overlap between `phi` and `load` | M | The IronLIR_Instr union stores phi and load data in separate members. Rewriting kind + member works because alloc_instr memset(0) the entire union, but this is fragile if union layout changes. Add static_assert(sizeof(phi) >= sizeof(load)) | blind_cast_phi_rewrite |
| 20 | lir_optimize.c:447-448 | `(Iron_EnumVariant *)ed->variants[vi]` in collect_mono_enums | Guarded by loop bound and ed->variants non-NULL | M | Same void* cast pattern as emit files | blind_cast_mono_enum |
| 21 | layout_analysis.c:153-154 | `collect_self_fields` accesses `fn->params` without bounds check | Guarded by `fn->param_count >= 1` at call site | L | Safe -- param_count check is sufficient | blind_cast_layout_self |
| 22 | verify.c:186-194 | SPAWN instruction accesses `instr->spawn.pool_val` and PARALLEL_FOR `instr->parallel_for.*` | Guarded by case label in switch | L | Standard tagged-union pattern | blind_cast_verify_spawn |
| 23 | print.c:492-516 | PARALLEL_FOR accesses multiple sub-fields | Guarded by case label | L | Standard tagged-union pattern | blind_cast_print_pfor |

### Enum Switch Exhaustiveness -- LIR Core

| # | File:Line | Switch Subject | Missing Cases | Severity | Suggested Fix | Regression Fixture |
|---|-----------|---------------|---------------|----------|---------------|-------------------|
| 11 | lir_optimize.c:435-470 | collect_mono_enums_node switch over node kinds | Missing: 8 AST node kinds (ENUM_DECL, IMPORT_STMT, etc.) documented in STATE.md | M | Already tracked as known issue in Phase 65 STATE.md decisions. Silently misses monomorphized enums from uncovered node kinds | optimize_mono_enum_switch |
| 12 | lir_optimize.c:803-900 | strength_reduction switch | Handles ADD, SUB, MUL, DIV, MOD. Missing: all others | L | Intentional -- only arithmetic ops can be strength-reduced. Others fall to default:break | optimize_strength_switch |

### Null Safety -- LIR Core

| # | File:Line | Dereference | Null Check | Severity | Suggested Fix | Regression Fixture |
|---|-----------|-------------|-----------|----------|---------------|-------------------|
| 22 | lir_optimize.c:30-34 | `find_block` returns NULL if block not found | Callers at lines 137,174 check `if (!pred) continue` | L | Correctly handled | null_find_block |
| 23 | lir_optimize.c:225-231 | `find_ir_func` returns NULL if not found | Callers check return value | L | Safe | null_find_ir_func |
| 24 | layout_analysis.c:64-66 | `hmgeti(split_ids, array_vid)` result checked | Correctly returns -1 if not found | L | Safe | null_layout_split |
| 25 | value_range.c:107-112 | `lookup_range` returns RANGE_TOP if vid not in map | Safe default for unknown values | L | Correctly handles missing entries | null_vr_lookup |
| 26 | verify.c:11-16 | `block_id_valid` iterates fn->blocks | Returns false for BLOCK_INVALID | L | Safe | null_verify_block |
| 27 | print.c:13-21 | `resolve_block_label` returns "<invalid>" for unknown IDs | Safe default | L | Safe | null_print_label |

### Arena Lifetimes -- LIR Core

| # | File:Line | Pattern | Severity | Suggested Fix | Regression Fixture |
|---|-----------|---------|----------|---------------|-------------------|
| 13 | lir_optimize.c:596 | `fusible_calls` stb_ds array (per-function prescan) | Freed at end of block scope | L | Correctly scoped | arena_fusible_calls |
| 14 | layout_analysis.c:52-56 | `value_to_split` and `alloca_to_split` hashmaps | Both freed at lines 134-135 | L | Correctly freed | arena_layout_maps |
| 15 | layout_analysis.c:162-163 | `self_vids` and `seen` hashmaps in collect_self_fields | Freed at lines 215-216 | L | Correctly freed | arena_self_fields |

### Integer Safety -- LIR Core

| # | File:Line | Operation | Severity | Suggested Fix | Regression Fixture |
|---|-----------|-----------|----------|---------------|-------------------|
| 14 | lir_optimize.c:201-211 | `make_param_mode_key` snprintf into 16-byte buffer for param index | L | param_index is small integer. 16 bytes sufficient for "%d" of any int32 | int_param_mode_key |
| 15 | layout_analysis.c:272-273 | `snprintf(prefix, sizeof(prefix), "%s_", lower_type)` with 512-byte buffer | L | lower_type is 256 bytes max, prefix is 512. Safe | int_layout_prefix |
| 16 | value_range.c:222-276 | `narrow_from_comparison` uses `const_val - 1` and `const_val + 1` | M | If const_val is INT64_MIN, `const_val - 1` wraps. If INT64_MAX, `const_val + 1` wraps. Add overflow guards | int_vr_narrow_wrap |
| 17 | lir.c:17-21 | `fn->next_value_id++` in alloc_instr | L | value_id is uint32_t. Would overflow after ~4 billion instructions per function. Not realistic | int_lir_value_id |

### Allocation Error Handling -- LIR Core

| # | File:Line | Allocation | Error Check | Severity | Suggested Fix | Regression Fixture |
|---|-----------|------------|-------------|----------|---------------|-------------------|
| 18 | lir.c:19-21 | `arrput(fn->value_table, NULL)` to grow value_table | stb_ds realloc -- no OOM check | M | Standard stb_ds concern | alloc_lir_value_table |
| 19 | lir.c:27-28 | `arrput(block->instrs, instr)` to grow instruction array | Same stb_ds concern | M | Same fix needed | alloc_lir_instrs |
| 20 | lir_optimize.c:51-53 | `arrput(block->instrs, NULL)` in insert_store_before_terminator | stb_ds concern | M | Same | alloc_optimize_insert |
| 21 | lir_optimize.c:110 | `arrput(phis, instr)` in phi_eliminate | stb_ds concern | M | Same | alloc_optimize_phis |
| 22 | layout_analysis.c:66 | `hmput(value_to_split, ...)` | stb_ds concern | M | Same | alloc_layout_hmput |
| 23 | value_range.c:156-173 | `hmput` in record_block_entry_range | stb_ds concern | M | Same | alloc_vr_block_range |

### Cross-Platform -- LIR Core

| # | File:Line | Assumption | Severity | Suggested Fix | Regression Fixture |
|---|-----------|-----------|----------|---------------|-------------------|
| 11 | value_range.c:69-90 | `__builtin_add_overflow` etc. are GCC/Clang-only | M | Already documented in emit section (#9). Need MSVC fallback | xplat_vr_builtins |
| 12 | lir_optimize.c:254 | `for (int iteration = 0; iteration < 8; iteration++)` fixpoint limit | L | Platform-independent. 8 iterations is a hardcoded heuristic, not a portability concern | xplat_fixpoint_limit |
| 13 | print.c:29-30 | `iron_type_to_string(type, tmp)` for display | L | Uses internal arena -- platform-independent | xplat_print_type |

---

## Summary -- LIR

| Dimension | High | Medium | Low | Total |
|-----------|------|--------|-----|-------|
| Blind Casts | 0 | 10 | 13 | 23 |
| Enum Switch Exhaustiveness | 0 | 3 | 9 | 12 |
| Null Safety | 2 | 7 | 18 | 27 |
| Arena Lifetimes | 0 | 2 | 13 | 15 |
| Integer Safety | 0 | 1 | 16 | 17 |
| Allocation Error Handling | 2 | 13 | 8 | 23 |
| Cross-Platform | 0 | 6 | 7 | 13 |
| **Total** | **4** | **42** | **84** | **130** |

### High-Severity Findings (4)

1. **emit_c.c:3082-3085 (Null Safety + Allocation)**: Generated HEAP_ALLOC C code does `malloc(sizeof(T))` then immediately dereferences `*_vN = val` without NULL check. Segfault on OOM in user programs.
2. **emit_c.c:3108-3109 (Null Safety + Allocation)**: Same pattern for RC_ALLOC -- malloc without NULL check in generated code.
3. **emit_c.c:3085 (Allocation)**: Duplicate of #1 from allocation dimension.
4. **emit_c.c:3109 (Allocation)**: Duplicate of #2 from allocation dimension.

*Note: The H-severity findings are 2 unique issues each counted in both Null Safety and Allocation Error Handling dimensions.*

### Key Observations

- **emit_c.c is the largest file (6600 lines) but is well-structured**: The main `emit_instr` switch is fully exhaustive over all 44 IronLIR_OpKind values.
- **verify.c and print.c are fully exhaustive**: Both handle all instruction kinds in their switches.
- **stb_ds allocation failures are the dominant M-severity pattern**: 13 of 42 medium findings are stb_ds OOM paths. This is a project-wide design tradeoff (also noted in audit-parser-lexer.md and audit-analyzer.md).
- **void* cast patterns (Iron_Field, Iron_TypeAnnotation, Iron_EnumVariant)**: 6 sites cast from void* arrays using only the loop index as guard. These are correct given the AST/type design but fragile if field types change.
- **value_range.c has the best integer safety discipline**: All arithmetic uses `__builtin_*_overflow` intrinsics. One edge case remains in `narrow_from_comparison` where `const_val +/- 1` can wrap at INT64 boundaries.
