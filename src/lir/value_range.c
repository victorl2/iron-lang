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

/* Intersect two ranges: max of mins, min of maxes.
 * Used for AND-chain accumulation at block entry. */
static ValueRange range_intersect(ValueRange a, ValueRange b) {
    if (a.is_top) return b;
    if (b.is_top) return a;
    int64_t lo = a.min > b.min ? a.min : b.min;
    int64_t hi = a.max < b.max ? a.max : b.max;
    if (lo > hi) return RANGE_TOP;  /* empty intersection = unknown */
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

/* ── Conditional narrowing types ───────────────────────────────────────── */

/* Per-block entry range overrides: narrowed ranges from conditional branches.
 * key = block ID, value = array of per-value narrowed ranges. */
typedef struct {
    IronLIR_BlockId key;
    VREntry *value;  /* stb_ds hashmap of value_id -> narrowed range */
} BlockRangeEntry;

/* Record a narrowed range for a value at a target block's entry.
 * If the target already has an entry for this value (AND-chain accumulation),
 * intersect the ranges rather than replacing. */
static void record_block_entry_range(BlockRangeEntry **block_entry_ranges,
                                     IronLIR_BlockId target,
                                     IronLIR_ValueId vid,
                                     ValueRange narrowed) {
    ptrdiff_t bi = hmgeti(*block_entry_ranges, target);
    if (bi >= 0) {
        /* Block already has entry ranges -- check for this value */
        VREntry *entries = (*block_entry_ranges)[bi].value;
        ptrdiff_t vi = hmgeti(entries, vid);
        if (vi >= 0) {
            /* AND-chain: intersect with existing narrowing */
            entries[vi].value = range_intersect(entries[vi].value, narrowed);
        } else {
            hmput(entries, vid, narrowed);
        }
        (*block_entry_ranges)[bi].value = entries;
    } else {
        VREntry *entries = NULL;
        hmput(entries, vid, narrowed);
        hmput(*block_entry_ranges, target, entries);
    }
}

/* Apply block entry ranges into the working value_ranges and alloca_ranges.
 * For values that have narrowed entry ranges, REPLACE their current range.
 * The intersection for AND-chains is already computed in record_block_entry_range;
 * here we just apply the final narrowed range for this block. */
static void apply_block_entry_ranges(BlockRangeEntry *block_entry_ranges,
                                     IronLIR_BlockId block_id,
                                     VREntry **value_ranges,
                                     AllocaRange **alloca_ranges) {
    ptrdiff_t bi = hmgeti(block_entry_ranges, block_id);
    if (bi < 0) return;

    VREntry *entries = block_entry_ranges[bi].value;
    for (int i = 0; i < hmlen(entries); i++) {
        IronLIR_ValueId vid = entries[i].key;
        ValueRange narrowed = entries[i].value;
        if (narrowed.is_top) continue;

        /* Check if this is an alloca (variable) or a direct value */
        ptrdiff_t ai = hmgeti(*alloca_ranges, vid);
        if (ai >= 0) {
            /* Replace alloca range with narrowed value */
            (*alloca_ranges)[ai].value = narrowed;
        } else {
            /* Replace or set the value range */
            hmput(*value_ranges, vid, narrowed);
        }
    }
}

/* Compute narrowed ranges from a comparison + branch.
 * Given: `if (var_vid CMP const_val)`, produce narrowed ranges for
 * the true and false target blocks. */
static void narrow_from_comparison(IronLIR_InstrKind cmp_kind,
                                   ValueRange existing,
                                   int64_t const_val,
                                   ValueRange *true_range,
                                   ValueRange *false_range) {
    if (existing.is_top) {
        /* Start from full int64 range if unknown */
        existing.min = INT64_MIN;
        existing.max = INT64_MAX;
        existing.is_top = false;
    }

    switch ((int)(cmp_kind)) {
    case IRON_LIR_LT:
        /* x < C: true -> [min, C-1], false -> [C, max] */
        *true_range = (ValueRange){ .min = existing.min,
                                     .max = const_val - 1 < existing.max ? const_val - 1 : existing.max,
                                     .is_top = false };
        *false_range = (ValueRange){ .min = const_val > existing.min ? const_val : existing.min,
                                      .max = existing.max,
                                      .is_top = false };
        break;
    case IRON_LIR_LTE:
        /* x <= C: true -> [min, C], false -> [C+1, max] */
        *true_range = (ValueRange){ .min = existing.min,
                                     .max = const_val < existing.max ? const_val : existing.max,
                                     .is_top = false };
        *false_range = (ValueRange){ .min = const_val + 1 > existing.min ? const_val + 1 : existing.min,
                                      .max = existing.max,
                                      .is_top = false };
        break;
    case IRON_LIR_GT:
        /* x > C: true -> [C+1, max], false -> [min, C] */
        *true_range = (ValueRange){ .min = const_val + 1 > existing.min ? const_val + 1 : existing.min,
                                     .max = existing.max,
                                     .is_top = false };
        *false_range = (ValueRange){ .min = existing.min,
                                      .max = const_val < existing.max ? const_val : existing.max,
                                      .is_top = false };
        break;
    case IRON_LIR_GTE:
        /* x >= C: true -> [C, max], false -> [min, C-1] */
        *true_range = (ValueRange){ .min = const_val > existing.min ? const_val : existing.min,
                                     .max = existing.max,
                                     .is_top = false };
        *false_range = (ValueRange){ .min = existing.min,
                                      .max = const_val - 1 < existing.max ? const_val - 1 : existing.max,
                                      .is_top = false };
        break;
    case IRON_LIR_EQ:
        /* x == C: true -> [C, C], false -> keep existing */
        *true_range = range_const(const_val);
        *false_range = existing;
        break;
    case IRON_LIR_NEQ:
        /* x != C: true -> keep existing, false -> [C, C] */
        *true_range = existing;
        *false_range = range_const(const_val);
        break;
    /* -Wswitch-enum opt-out: narrowing analyzer only recognizes comparison
     * opcodes; every other LIR opcode is ignored and the true/false ranges
     * default to TOP (no information). */
    default:
        *true_range = RANGE_TOP;
        *false_range = RANGE_TOP;
        break;
    }

    /* Validate: if min > max after narrowing, result is empty -> TOP */
    if (!true_range->is_top && true_range->min > true_range->max)
        *true_range = RANGE_TOP;
    if (!false_range->is_top && false_range->min > false_range->max)
        *false_range = RANGE_TOP;
}

/* ── Return range collection (Pass 0) ─────────────────────────────────── */

/* Collect return ranges for all functions in the module.
 * For each function, compute the union of all RETURN instruction ranges.
 * For recursive functions, use one-level unrolling: skip RETURN paths
 * whose value traces back to a CALL to the same function. */
/* Analyze a single block's conditional branch terminator and record narrowed
 * ranges for target blocks.  Shared by collect_return_ranges and
 * analyze_function_ranges to avoid code duplication. */
static void detect_branch_narrowing(IronLIR_Block *blk, IronLIR_Func *fn,
                                    VREntry *value_ranges,
                                    AllocaRange *alloca_ranges,
                                    BlockRangeEntry **block_entry_ranges) {
    if (blk->instr_count == 0) return;
    IronLIR_Instr *term = blk->instrs[blk->instr_count - 1];
    if (term->kind != IRON_LIR_BRANCH) return;

    IronLIR_ValueId cond_vid = term->branch.cond;
    IronLIR_BlockId then_block = term->branch.then_block;
    IronLIR_BlockId else_block = term->branch.else_block;

    if (cond_vid == IRON_LIR_VALUE_INVALID) return;
    if (cond_vid >= fn->next_value_id || !fn->value_table) return;
    IronLIR_Instr *cond_instr = fn->value_table[cond_vid];
    if (!cond_instr) return;

    /* Check if the condition is a comparison instruction (LT/LTE/GT/GTE/EQ/NEQ) */
    bool is_cmp = (cond_instr->kind >= IRON_LIR_EQ &&
                   cond_instr->kind <= IRON_LIR_GTE);
    if (!is_cmp) return;

    IronLIR_ValueId left_vid = cond_instr->binop.left;
    IronLIR_ValueId right_vid = cond_instr->binop.right;

    IronLIR_Instr *left_instr = NULL;
    IronLIR_Instr *right_instr = NULL;
    if (left_vid < fn->next_value_id && fn->value_table)
        left_instr = fn->value_table[left_vid];
    if (right_vid < fn->next_value_id && fn->value_table)
        right_instr = fn->value_table[right_vid];

    IronLIR_ValueId var_vid = IRON_LIR_VALUE_INVALID;
    int64_t const_val = 0;
    IronLIR_InstrKind effective_cmp = cond_instr->kind;
    bool found_pair = false;

    if (right_instr && right_instr->kind == IRON_LIR_CONST_INT) {
        var_vid = left_vid;
        const_val = right_instr->const_int.value;
        found_pair = true;
    } else if (left_instr && left_instr->kind == IRON_LIR_CONST_INT) {
        var_vid = right_vid;
        const_val = left_instr->const_int.value;
        /* Flip: LT <-> GT, LTE <-> GTE */
        switch ((int)(cond_instr->kind)) {
        case IRON_LIR_LT:  effective_cmp = IRON_LIR_GT;  break;
        case IRON_LIR_LTE: effective_cmp = IRON_LIR_GTE; break;
        case IRON_LIR_GT:  effective_cmp = IRON_LIR_LT;  break;
        case IRON_LIR_GTE: effective_cmp = IRON_LIR_LTE; break;
        /* -Wswitch-enum opt-out: only directional comparisons flip; EQ and
         * NEQ are symmetric so they keep their original opcode (default
         * no-op). */
        default: break;
        }
        found_pair = true;
    }

    if (!found_pair || var_vid == IRON_LIR_VALUE_INVALID) return;

    /* Look up the variable's current range */
    ValueRange existing = lookup_range(value_ranges, var_vid);
    IronLIR_ValueId narrow_target = var_vid;

    /* If var_vid is a LOAD from an alloca, narrow the alloca instead */
    if (var_vid < fn->next_value_id && fn->value_table &&
        fn->value_table[var_vid] &&
        fn->value_table[var_vid]->kind == IRON_LIR_LOAD) {
        IronLIR_ValueId alloca_vid = fn->value_table[var_vid]->load.ptr;
        ptrdiff_t ai = hmgeti(alloca_ranges, alloca_vid);
        if (ai >= 0) {
            existing = alloca_ranges[ai].value;
            narrow_target = alloca_vid;
        }
    }

    ValueRange true_range, false_range;
    narrow_from_comparison(effective_cmp, existing, const_val,
                           &true_range, &false_range);

    if (!true_range.is_top && then_block != IRON_LIR_BLOCK_INVALID)
        record_block_entry_range(block_entry_ranges, then_block,
                                 narrow_target, true_range);
    if (!false_range.is_top && else_block != IRON_LIR_BLOCK_INVALID)
        record_block_entry_range(block_entry_ranges, else_block,
                                 narrow_target, false_range);
}

static void collect_return_ranges(ValueRangeAnalysis *vra, IronLIR_Module *module) {
    sh_new_strdup(vra->func_return_ranges);

    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (!fn || fn->is_extern || fn->block_count == 0) continue;
        if (!fn->name) continue;

        /* Compute per-value ranges with conditional narrowing */
        VREntry *value_ranges = NULL;
        AllocaRange *alloca_ranges = NULL;
        BlockRangeEntry *block_entry_ranges = NULL;

        /* Track which value IDs come from a self-recursive CALL */
        VREntry *recursive_values = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];

            /* Apply block entry ranges from conditional narrowing */
            apply_block_entry_ranges(block_entry_ranges, blk->id,
                                     &value_ranges, &alloca_ranges);

            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *instr = blk->instrs[ii];
                if (instr->id == IRON_LIR_VALUE_INVALID) continue;

                ValueRange range = RANGE_TOP;

                switch ((int)(instr->kind)) {
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

                /* -Wswitch-enum opt-out: range collector computes tight
                 * intervals for CONST_INT and arithmetic opcodes; every
                 * other LIR opcode defaults to RANGE_TOP (unknown). */
                default:
                    range = RANGE_TOP;
                    break;
                }

                hmput(value_ranges, instr->id, range);
                collect_next:;
            }

            /* Detect conditional narrowing at BRANCH terminators */
            detect_branch_narrowing(blk, fn, value_ranges, alloca_ranges,
                                    &block_entry_ranges);
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
                        ptrdiff_t ai = hmgeti(alloca_ranges, ret_instr->load.ptr);
                        if (ai >= 0 && alloca_ranges[ai].value.is_top) {
                            /* Conservative: TOP range, might be from recursive call */
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
        for (int i = 0; i < hmlen(block_entry_ranges); i++)
            hmfree(block_entry_ranges[i].value);
        hmfree(block_entry_ranges);
    }
}

static void analyze_function_ranges(ValueRangeAnalysis *vra, IronLIR_Func *fn) {
    if (!fn || fn->is_extern || fn->block_count == 0) return;

    VREntry *value_ranges = NULL;     /* per-value ranges (stb_ds hashmap) */
    AllocaRange *alloca_ranges = NULL; /* per-alloca ranges (stb_ds hashmap) */
    BlockRangeEntry *block_entry_ranges = NULL; /* per-block narrowed ranges */

    /* Pass 1: Compute per-value ranges with conditional narrowing */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];

        /* Apply block entry ranges from conditional narrowing */
        apply_block_entry_ranges(block_entry_ranges, blk->id,
                                 &value_ranges, &alloca_ranges);

        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->id == IRON_LIR_VALUE_INVALID) continue;

            ValueRange range = RANGE_TOP;

            switch ((int)(instr->kind)) {
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

            /* -Wswitch-enum opt-out: range propagator handles additive /
             * multiplicative / constant opcodes; every other LIR opcode
             * (DIV, MOD, comparisons, memory ops, terminators, etc.)
             * conservatively produces RANGE_TOP. */
            default:
                /* DIV, MOD, comparisons, etc. -> TOP (conservative) */
                range = RANGE_TOP;
                break;
            }

            hmput(value_ranges, instr->id, range);
            next_instr:;
        }

        /* Detect conditional narrowing at BRANCH terminators */
        detect_branch_narrowing(blk, fn, value_ranges, alloca_ranges,
                                &block_entry_ranges);
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
                    if (!key_str) iron_oom_abort("value_range.c:analyze_function_ranges set_field_key");
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
                        if (!key_str) iron_oom_abort("value_range.c:analyze_function_ranges construct_key");
                        shput(vra->field_ranges, key_str, val_range);
                    }
                }
            }
        }
    }

    hmfree(value_ranges);
    hmfree(alloca_ranges);

    /* Free block entry ranges and their nested VREntry maps */
    for (int i = 0; i < hmlen(block_entry_ranges); i++) {
        hmfree(block_entry_ranges[i].value);
    }
    hmfree(block_entry_ranges);
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
