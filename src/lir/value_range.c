/* value_range.c -- Phase 50: Whole-program value range analysis for field compression.
 *
 * Implements conservative dataflow analysis over LIR instructions to determine
 * per-field value ranges.  Fields in collection storage structs proven to fit
 * in narrower C types are reported via iron_vr_get_narrowed_type().
 *
 * Analysis overview:
 *   1. Per-function: track value ranges through CONST_INT, arithmetic, PHI, LOAD/STORE
 *   2. At SET_FIELD and CONSTRUCT: union value range into per-field global range
 *   3. Interprocedural: scan all functions, accumulate field ranges
 *   4. Query: select smallest safe C type from proven range
 */

#include "lir/value_range.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ── Range constants ────────────────────────────────────────────────────── */

static const ValueRange RANGE_TOP = { .min = 0, .max = 0, .is_top = true };

static inline ValueRange range_const(int64_t val) {
    return (ValueRange){ .min = val, .max = val, .is_top = false };
}

/* ── Type ladder: select smallest safe C type for a proven range ─────── */

static const char *select_narrowed_type(int64_t min, int64_t max) {
    /* Unsigned types (min >= 0) */
    if (min >= 0) {
        if (max <= UINT8_MAX)  return "uint8_t";    /* [0, 255] */
        if (max <= UINT16_MAX) return "uint16_t";    /* [0, 65535] */
        if (max <= UINT32_MAX) return "uint32_t";    /* [0, 4294967295] */
        return NULL;  /* uint64_t = original size, no savings */
    }
    /* Signed types (min < 0) */
    if (min >= INT8_MIN  && max <= INT8_MAX)  return "int8_t";   /* [-128, 127] */
    if (min >= INT16_MIN && max <= INT16_MAX) return "int16_t";  /* [-32768, 32767] */
    if (min >= INT32_MIN && max <= INT32_MAX) return "int32_t";  /* [-2^31, 2^31-1] */
    return NULL;  /* int64_t = original size, no savings */
}

/* ── Range arithmetic with overflow protection ──────────────────────────── */

static ValueRange range_union(ValueRange a, ValueRange b) {
    if (a.is_top || b.is_top) return RANGE_TOP;
    int64_t lo = a.min < b.min ? a.min : b.min;
    int64_t hi = a.max > b.max ? a.max : b.max;
    return (ValueRange){ .min = lo, .max = hi, .is_top = false };
}

static ValueRange range_add(ValueRange a, ValueRange b) {
    if (a.is_top || b.is_top) return RANGE_TOP;
    int64_t lo, hi;
    if (__builtin_add_overflow(a.min, b.min, &lo)) return RANGE_TOP;
    if (__builtin_add_overflow(a.max, b.max, &hi)) return RANGE_TOP;
    return (ValueRange){ .min = lo, .max = hi, .is_top = false };
}

static ValueRange range_sub(ValueRange a, ValueRange b) {
    if (a.is_top || b.is_top) return RANGE_TOP;
    /* [a.min - b.max, a.max - b.min] */
    int64_t lo, hi;
    if (__builtin_sub_overflow(a.min, b.max, &lo)) return RANGE_TOP;
    if (__builtin_sub_overflow(a.max, b.min, &hi)) return RANGE_TOP;
    return (ValueRange){ .min = lo, .max = hi, .is_top = false };
}

static ValueRange range_mul(ValueRange a, ValueRange b) {
    if (a.is_top || b.is_top) return RANGE_TOP;
    /* Compute all four products, take min/max */
    int64_t products[4];
    if (__builtin_mul_overflow(a.min, b.min, &products[0])) return RANGE_TOP;
    if (__builtin_mul_overflow(a.min, b.max, &products[1])) return RANGE_TOP;
    if (__builtin_mul_overflow(a.max, b.min, &products[2])) return RANGE_TOP;
    if (__builtin_mul_overflow(a.max, b.max, &products[3])) return RANGE_TOP;
    int64_t lo = products[0], hi = products[0];
    for (int i = 1; i < 4; i++) {
        if (products[i] < lo) lo = products[i];
        if (products[i] > hi) hi = products[i];
    }
    return (ValueRange){ .min = lo, .max = hi, .is_top = false };
}

/* ── Per-value range tracking (transient, per-function) ─────────────────── */

typedef struct {
    IronLIR_ValueId key;
    ValueRange value;
} VREntry;

/* Look up range for a value id. Returns TOP if not tracked. */
static ValueRange lookup_range(VREntry *value_ranges, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return RANGE_TOP;
    ptrdiff_t idx = hmgeti(value_ranges, vid);
    if (idx >= 0) return value_ranges[idx].value;
    return RANGE_TOP;
}

/* Resolve the callee name from a CALL instruction.
 * Handles both direct calls (via func_decl) and indirect calls via func_ref. */
static const char *resolve_call_name(IronLIR_Instr *instr, IronLIR_Func *fn) {
    if (instr->call.func_decl && instr->call.func_decl->name) {
        return instr->call.func_decl->name;
    }
    /* Try resolving via func_ptr -> FUNC_REF */
    IronLIR_ValueId fptr = instr->call.func_ptr;
    if (fptr != IRON_LIR_VALUE_INVALID &&
        fptr < fn->next_value_id && fn->value_table &&
        fn->value_table[fptr] &&
        fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
        return fn->value_table[fptr]->func_ref.func_name;
    }
    return NULL;
}

/* ── Per-function analysis ──────────────────────────────────────────────── */

/* Track STORE/LOAD for alloca-based variable tracking:
 * maps alloca ValueId -> most recent stored ValueRange */
typedef struct {
    IronLIR_ValueId key;
    ValueRange value;
} AllocaRange;

/* ── Return range collection (Pass 0) ─────────────────────────────────── */

/* Collect return ranges for all functions in the module.
 * For each function, compute the union of all RETURN instruction ranges.
 * For recursive functions, use one-level unrolling: skip RETURN paths
 * whose value traces back to a CALL to the same function. */
static void collect_return_ranges(ValueRangeAnalysis *vra, IronLIR_Module *module) {
    sh_new_strdup(vra->func_return_ranges);

    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (!fn || fn->is_extern || fn->block_count == 0) continue;
        if (!fn->name) continue;

        /* Compute per-value ranges for this function (same logic as Pass 1) */
        VREntry *value_ranges = NULL;
        AllocaRange *alloca_ranges = NULL;

        /* Track which value IDs come from a self-recursive CALL */
        VREntry *recursive_values = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *instr = blk->instrs[ii];
                if (instr->id == IRON_LIR_VALUE_INVALID) continue;

                ValueRange range = RANGE_TOP;

                switch (instr->kind) {
                case IRON_LIR_CONST_INT:
                    range = range_const(instr->const_int.value);
                    break;

                case IRON_LIR_ADD:
                case IRON_LIR_SUB:
                case IRON_LIR_MUL: {
                    ValueRange lhs = lookup_range(value_ranges, instr->binop.left);
                    ValueRange rhs = lookup_range(value_ranges, instr->binop.right);
                    if (instr->kind == IRON_LIR_ADD)
                        range = range_add(lhs, rhs);
                    else if (instr->kind == IRON_LIR_SUB)
                        range = range_sub(lhs, rhs);
                    else
                        range = range_mul(lhs, rhs);
                    break;
                }

                case IRON_LIR_PHI: {
                    bool first = true;
                    for (int pi = 0; pi < instr->phi.count; pi++) {
                        ValueRange incoming = lookup_range(value_ranges,
                            instr->phi.values[pi]);
                        if (first) { range = incoming; first = false; }
                        else { range = range_union(range, incoming); }
                        if (range.is_top) break;
                    }
                    if (first) range = RANGE_TOP;
                    break;
                }

                case IRON_LIR_LOAD: {
                    ptrdiff_t ai = hmgeti(alloca_ranges, instr->load.ptr);
                    if (ai >= 0) range = alloca_ranges[ai].value;
                    else range = RANGE_TOP;
                    break;
                }

                case IRON_LIR_STORE: {
                    ValueRange stored = lookup_range(value_ranges, instr->store.value);
                    ptrdiff_t ai = hmgeti(alloca_ranges, instr->store.ptr);
                    if (ai >= 0)
                        alloca_ranges[ai].value = range_union(alloca_ranges[ai].value, stored);
                    else
                        hmput(alloca_ranges, instr->store.ptr, stored);
                    goto collect_next;
                }

                case IRON_LIR_CALL: {
                    /* Mark self-recursive call results so we can skip them in RETURNs */
                    const char *callee = resolve_call_name(instr, fn);
                    if (callee && strcmp(callee, fn->name) == 0) {
                        hmput(recursive_values, instr->id, (ValueRange){0});
                    }
                    range = RANGE_TOP;
                    break;
                }

                default:
                    range = RANGE_TOP;
                    break;
                }

                hmput(value_ranges, instr->id, range);
                collect_next:;
            }
        }

        /* Collect return ranges: union of all non-recursive RETURN values */
        ValueRange func_range = RANGE_TOP;
        bool has_return = false;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *instr = blk->instrs[ii];
                if (instr->kind != IRON_LIR_RETURN) continue;
                if (instr->ret.is_void) continue;

                IronLIR_ValueId ret_vid = instr->ret.value;
                if (ret_vid == IRON_LIR_VALUE_INVALID) continue;

                /* One-level unrolling: skip returns that come from self-recursive calls */
                if (hmgeti(recursive_values, ret_vid) >= 0) continue;

                /* Also check if the return value is a LOAD from an alloca that was
                 * stored from a recursive call */
                if (ret_vid < fn->next_value_id && fn->value_table &&
                    fn->value_table[ret_vid]) {
                    IronLIR_Instr *ret_instr = fn->value_table[ret_vid];
                    if (ret_instr->kind == IRON_LIR_LOAD) {
                        /* Check alloca: if the only store was from a recursive call, skip */
                        ptrdiff_t ai = hmgeti(alloca_ranges, ret_instr->load.ptr);
                        if (ai >= 0 && alloca_ranges[ai].value.is_top) {
                            /* Conservative: alloca has TOP range, might be from recursive call */
                            /* Don't skip -- just use the TOP range for safety */
                        }
                    }
                }

                ValueRange ret_range = lookup_range(value_ranges, ret_vid);

                if (!has_return) {
                    func_range = ret_range;
                    has_return = true;
                } else {
                    func_range = range_union(func_range, ret_range);
                }
            }
        }

        if (has_return && !func_range.is_top) {
            shput(vra->func_return_ranges, fn->name, func_range);
        }

        hmfree(value_ranges);
        hmfree(alloca_ranges);
        hmfree(recursive_values);
    }
}

static void analyze_function_ranges(ValueRangeAnalysis *vra, IronLIR_Func *fn) {
    if (!fn || fn->is_extern || fn->block_count == 0) return;

    VREntry *value_ranges = NULL;     /* per-value ranges (stb_ds hashmap) */
    AllocaRange *alloca_ranges = NULL; /* per-alloca ranges (stb_ds hashmap) */

    /* Pass 1: Compute per-value ranges */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->id == IRON_LIR_VALUE_INVALID) continue;

            ValueRange range = RANGE_TOP;

            switch (instr->kind) {
            case IRON_LIR_CONST_INT:
                range = range_const(instr->const_int.value);
                break;

            case IRON_LIR_ADD:
            case IRON_LIR_SUB:
            case IRON_LIR_MUL: {
                ValueRange lhs = lookup_range(value_ranges, instr->binop.left);
                ValueRange rhs = lookup_range(value_ranges, instr->binop.right);
                if (instr->kind == IRON_LIR_ADD)
                    range = range_add(lhs, rhs);
                else if (instr->kind == IRON_LIR_SUB)
                    range = range_sub(lhs, rhs);
                else
                    range = range_mul(lhs, rhs);
                break;
            }

            case IRON_LIR_PHI: {
                /* Union of all incoming value ranges */
                bool first = true;
                for (int pi = 0; pi < instr->phi.count; pi++) {
                    ValueRange incoming = lookup_range(value_ranges,
                        instr->phi.values[pi]);
                    if (first) {
                        range = incoming;
                        first = false;
                    } else {
                        range = range_union(range, incoming);
                    }
                    if (range.is_top) break;
                }
                if (first) range = RANGE_TOP;
                break;
            }

            case IRON_LIR_LOAD: {
                /* Look up the range of the alloca being loaded from */
                ptrdiff_t ai = hmgeti(alloca_ranges, instr->load.ptr);
                if (ai >= 0) {
                    range = alloca_ranges[ai].value;
                } else {
                    range = RANGE_TOP;
                }
                break;
            }

            case IRON_LIR_STORE: {
                /* Track the stored value's range for the alloca */
                ValueRange stored = lookup_range(value_ranges, instr->store.value);
                ptrdiff_t ai = hmgeti(alloca_ranges, instr->store.ptr);
                if (ai >= 0) {
                    /* Union with existing (multiple stores to same alloca) */
                    alloca_ranges[ai].value = range_union(alloca_ranges[ai].value, stored);
                } else {
                    hmput(alloca_ranges, instr->store.ptr, stored);
                }
                /* STORE produces no value -- skip putting into value_ranges */
                goto next_instr;
            }

            case IRON_LIR_CALL: {
                /* Look up callee's return range from pre-computed func_return_ranges */
                const char *callee = resolve_call_name(instr, fn);
                if (callee && vra->func_return_ranges) {
                    ptrdiff_t fri = shgeti(vra->func_return_ranges, callee);
                    if (fri >= 0) {
                        range = vra->func_return_ranges[fri].value;
                        break;
                    }
                }
                /* Indirect calls or unknown functions stay TOP */
                range = RANGE_TOP;
                break;
            }

            default:
                /* DIV, MOD, comparisons, etc. -> TOP (conservative) */
                range = RANGE_TOP;
                break;
            }

            hmput(value_ranges, instr->id, range);
            next_instr:;
        }
    }

    /* Pass 2: Collect field ranges from SET_FIELD and CONSTRUCT */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];

            if (instr->kind == IRON_LIR_SET_FIELD) {
                /* Determine the type name from the object value */
                IronLIR_ValueId obj_vid = instr->field.object;
                const char *field_name = instr->field.field;
                if (!field_name) continue;

                /* Get the type of the object being set */
                const char *type_name = NULL;
                if (obj_vid < fn->next_value_id && fn->value_table &&
                    fn->value_table[obj_vid]) {
                    IronLIR_Instr *obj_instr = fn->value_table[obj_vid];
                    if (obj_instr->type && obj_instr->type->kind == IRON_TYPE_OBJECT &&
                        obj_instr->type->object.decl) {
                        type_name = obj_instr->type->object.decl->name;
                    }
                }
                if (!type_name) continue;

                /* Look up the value range of the stored field value */
                ValueRange val_range = lookup_range(value_ranges, instr->field.value);

                /* Build key "type_name:field_name" and union into field_ranges */
                char key_buf[512];
                snprintf(key_buf, sizeof(key_buf), "%s:%s", type_name, field_name);
                ptrdiff_t fri = shgeti(vra->field_ranges, key_buf);
                if (fri >= 0) {
                    vra->field_ranges[fri].value =
                        range_union(vra->field_ranges[fri].value, val_range);
                } else {
                    const char *key_str = iron_arena_strdup(vra->arena,
                        key_buf, strlen(key_buf));
                    shput(vra->field_ranges, key_str, val_range);
                }
            }

            if (instr->kind == IRON_LIR_CONSTRUCT) {
                /* Get type info from construct instruction */
                Iron_Type *ctype = instr->construct.type;
                if (!ctype || ctype->kind != IRON_TYPE_OBJECT || !ctype->object.decl)
                    continue;
                Iron_ObjectDecl *od = ctype->object.decl;
                const char *type_name = od->name;
                if (!type_name) continue;

                /* For each field in declaration order, look up its value range */
                int fc = instr->construct.field_count;
                if (fc > od->field_count) fc = od->field_count;
                for (int fi = 0; fi < fc; fi++) {
                    Iron_Field *f = (Iron_Field *)od->fields[fi];
                    if (!f->name) continue;

                    IronLIR_ValueId fval = instr->construct.field_vals[fi];
                    ValueRange val_range = lookup_range(value_ranges, fval);

                    char key_buf[512];
                    snprintf(key_buf, sizeof(key_buf), "%s:%s", type_name, f->name);
                    ptrdiff_t fri = shgeti(vra->field_ranges, key_buf);
                    if (fri >= 0) {
                        vra->field_ranges[fri].value =
                            range_union(vra->field_ranges[fri].value, val_range);
                    } else {
                        const char *key_str = iron_arena_strdup(vra->arena,
                            key_buf, strlen(key_buf));
                        shput(vra->field_ranges, key_str, val_range);
                    }
                }
            }
        }
    }

    hmfree(value_ranges);
    hmfree(alloca_ranges);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void iron_vr_analyze(ValueRangeAnalysis *vra,
                     IronLIR_Module *module,
                     Iron_IfaceRegistry *iface_reg) {
    if (!vra || !module) return;
    (void)iface_reg;  /* reserved for future interprocedural call-site analysis */

    /* Initialize the string hash map */
    sh_new_strdup(vra->field_ranges);

    /* Pass 0: Collect function return ranges for interprocedural propagation */
    collect_return_ranges(vra, module);

    /* Scan all functions: accumulate field ranges with union semantics */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        analyze_function_ranges(vra, fn);
    }
}

const char *iron_vr_get_narrowed_type(ValueRangeAnalysis *vra,
                                       const char *type_name,
                                       const char *field_name) {
    if (!vra || !vra->field_ranges || !type_name || !field_name) return NULL;

    char key[512];
    snprintf(key, sizeof(key), "%s:%s", type_name, field_name);
    ptrdiff_t idx = shgeti(vra->field_ranges, key);
    if (idx < 0) return NULL;  /* no range data -> conservative, no compression */

    ValueRange r = vra->field_ranges[idx].value;
    if (r.is_top) return NULL;  /* unknown range */

    return select_narrowed_type(r.min, r.max);
}

void iron_vr_free(ValueRangeAnalysis *vra) {
    if (!vra) return;
    shfree(vra->field_ranges);
    vra->field_ranges = NULL;
    shfree(vra->func_return_ranges);
    vra->func_return_ranges = NULL;
}
