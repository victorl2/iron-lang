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
#include <stdlib.h>

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
    /* stb_ds lookups allocate a default entry on a NULL map. This pointer
     * is passed by value, so such an allocation would be lost. */
    if (!value_ranges || vid == IRON_LIR_VALUE_INVALID) return RANGE_TOP;
    ptrdiff_t idx = hmgeti(value_ranges, vid);
    if (idx >= 0) return value_ranges[idx].value;
    return RANGE_TOP;
}

/* Parameter IDs and deliberately sparse hand-built LIR can be below
 * next_value_id without having an instruction slot in value_table.  Always
 * bounds-check the actual stb_ds array before following a producer. */
static IronLIR_Instr *lookup_value_instr(IronLIR_Func *fn,
                                         IronLIR_ValueId vid) {
    if (!fn || !fn->value_table || vid == IRON_LIR_VALUE_INVALID ||
        (ptrdiff_t)vid >= arrlen(fn->value_table)) return NULL;
    return fn->value_table[vid];
}

/* Resolve the callee name from a CALL instruction.
 * Handles both direct calls (via func_decl) and indirect calls via func_ref. */
static const char *resolve_call_name(IronLIR_Instr *instr, IronLIR_Func *fn) {
    if (instr->call.func_decl && instr->call.func_decl->name) {
        return instr->call.func_decl->name;
    }
    /* Try resolving via func_ptr -> FUNC_REF */
    IronLIR_ValueId fptr = instr->call.func_ptr;
    IronLIR_Instr *fref = lookup_value_instr(fn, fptr);
    if (fref && fref->kind == IRON_LIR_FUNC_REF) {
        return fref->func_ref.func_name;
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

/* ── Conservative function-local range proof (P7) ─────────────────────── */

struct IronVR_LocalFuncAnalysis {
    IronLIR_Func *fn;
    size_t value_count;
    ValueRange *value_ranges;   /* indexed by ValueId */
    ValueRange *alloca_ranges;  /* indexed by ALLOCA ValueId */
    bool *alloca_eligible;      /* false for aliases/address-observable slots */
    bool *address_observable;  /* includes address-taken SSA temporaries */
};

typedef struct {
    struct IronVR_LocalFuncAnalysis *result;
    unsigned char *value_state;   /* 0 unseen, 1 visiting, 2 complete */
    unsigned char *alloca_state;
} LocalProofCtx;

static bool local_is_generic_integer(const Iron_Type *type) {
    return type && (type->kind == IRON_TYPE_INT || type->kind == IRON_TYPE_UINT);
}

static bool local_alloca_is_capture_alias(IronLIR_Func *fn,
                                          IronLIR_Instr *alloca_instr) {
    if (!fn || !alloca_instr || !alloca_instr->alloca.name_hint ||
        !fn->capture_metadata) return false;
    for (int i = 0; i < fn->capture_count; i++) {
        if (fn->capture_metadata[i].name &&
            strcmp(fn->capture_metadata[i].name,
                   alloca_instr->alloca.name_hint) == 0) return true;
    }
    return false;
}

static void local_mark_observable_allocas(struct IronVR_LocalFuncAnalysis *lf) {
    IronLIR_Func *fn = lf->fn;
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
#define REJECT_ALLOCA(vid_) do { \
    IronLIR_ValueId _v = (vid_); \
    if (_v != IRON_LIR_VALUE_INVALID && _v < lf->value_count) { \
        lf->alloca_eligible[_v] = false; \
        lf->address_observable[_v] = true; \
    } \
} while (0)
            switch ((int)in->kind) {
            case IRON_LIR_ADDR_OF:
                REJECT_ALLOCA(in->addr_of.target);
                break;
            case IRON_LIR_CALL:
                if (in->call.self_by_addr && in->call.arg_count > 0)
                    REJECT_ALLOCA(in->call.args[0]);
                for (int ai = 0; ai < in->call.arg_count; ai++) {
                    if (in->call.args_by_addr && in->call.args_by_addr[ai])
                        REJECT_ALLOCA(in->call.args[ai]);
                }
                break;
            case IRON_LIR_STORE:
            case IRON_LIR_RETURN:
                /* P2b intentionally uses an ALLOCA id as the scalar C local
                 * in ordinary stores, calls and returns.  Address passage is
                 * represented explicitly by ADDR_OF or args_by_addr. */
                break;
            case IRON_LIR_MAKE_CLOSURE:
                for (int ci = 0; ci < in->make_closure.capture_count; ci++)
                    REJECT_ALLOCA(in->make_closure.captures[ci]);
                break;
            case IRON_LIR_PTR_LOAD:
                REJECT_ALLOCA(in->ptr_load.fp);
                break;
            case IRON_LIR_PTR_STORE:
                REJECT_ALLOCA(in->ptr_store.fp);
                REJECT_ALLOCA(in->ptr_store.value);
                break;
            case IRON_LIR_PTR_OFFSET:
                REJECT_ALLOCA(in->ptr_offset.ptr);
                break;
            case IRON_LIR_PTR_DIFF:
                REJECT_ALLOCA(in->ptr_diff.a);
                REJECT_ALLOCA(in->ptr_diff.b);
                break;
            /* ALLOCA ids used directly by scalar expressions are the safe C
             * local aliases created by P2b, not address observations. */
            default:
                break;
            }
#undef REJECT_ALLOCA
        }
    }
}

static ValueRange local_eval_value(LocalProofCtx *ctx, IronLIR_ValueId vid);

static ValueRange local_eval_alloca(LocalProofCtx *ctx,
                                    IronLIR_ValueId alloca_id) {
    struct IronVR_LocalFuncAnalysis *lf = ctx->result;
    IronLIR_Func *fn = lf->fn;
    if (alloca_id == IRON_LIR_VALUE_INVALID ||
        alloca_id >= lf->value_count ||
        !lf->alloca_eligible[alloca_id]) return RANGE_TOP;
    if (ctx->alloca_state[alloca_id] == 2)
        return lf->alloca_ranges[alloca_id];
    if (ctx->alloca_state[alloca_id] == 1)
        return RANGE_TOP; /* loop-carried or otherwise cyclic */

    ctx->alloca_state[alloca_id] = 1;
    bool found_store = false;
    ValueRange result = RANGE_TOP;
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
            if (in->kind != IRON_LIR_STORE ||
                in->store.ptr != alloca_id) continue;
            ValueRange stored = local_eval_value(ctx, in->store.value);
            if (!found_store) {
                result = stored;
                found_store = true;
            } else {
                result = range_union(result, stored);
            }
            if (result.is_top) goto done;
        }
    }

done:
    if (!found_store) result = RANGE_TOP;
    lf->alloca_ranges[alloca_id] = result;
    ctx->alloca_state[alloca_id] = 2;
    return result;
}

static ValueRange local_eval_value(LocalProofCtx *ctx, IronLIR_ValueId vid) {
    struct IronVR_LocalFuncAnalysis *lf = ctx->result;
    IronLIR_Func *fn = lf->fn;
    if (vid == IRON_LIR_VALUE_INVALID || vid >= lf->value_count)
        return RANGE_TOP;
    if (lf->address_observable[vid]) return RANGE_TOP;
    if (ctx->value_state[vid] == 2) return lf->value_ranges[vid];
    if (ctx->value_state[vid] == 1) return RANGE_TOP;

    /* Parameters are synthetic ValueIds with no defining instruction. */
    IronLIR_Instr *in = ((ptrdiff_t)vid < arrlen(fn->value_table))
                         ? fn->value_table[vid] : NULL;
    if (!in) return RANGE_TOP;

    ctx->value_state[vid] = 1;
    ValueRange result = RANGE_TOP;
    switch ((int)in->kind) {
    case IRON_LIR_CONST_INT:
        result = range_const(in->const_int.value);
        break;
    case IRON_LIR_ALLOCA:
        result = local_eval_alloca(ctx, vid);
        break;
    case IRON_LIR_LOAD:
        result = local_eval_alloca(ctx, in->load.ptr);
        break;
    case IRON_LIR_ADD:
        result = range_add(local_eval_value(ctx, in->binop.left),
                           local_eval_value(ctx, in->binop.right));
        break;
    case IRON_LIR_SUB:
        result = range_sub(local_eval_value(ctx, in->binop.left),
                           local_eval_value(ctx, in->binop.right));
        break;
    case IRON_LIR_MUL:
        result = range_mul(local_eval_value(ctx, in->binop.left),
                           local_eval_value(ctx, in->binop.right));
        break;
    case IRON_LIR_PHI: {
        bool found = false;
        for (int i = 0; i < in->phi.count; i++) {
            ValueRange incoming = local_eval_value(ctx, in->phi.values[i]);
            result = found ? range_union(result, incoming) : incoming;
            found = true;
            if (result.is_top) break;
        }
        if (!found) result = RANGE_TOP;
        break;
    }
    default:
        result = RANGE_TOP;
        break;
    }
    lf->value_ranges[vid] = result;
    ctx->value_state[vid] = 2;
    return result;
}

static int local_block_index(IronLIR_Func *fn, IronLIR_BlockId id) {
    for (int i = 0; i < fn->block_count; i++)
        if (fn->blocks[i]->id == id) return i;
    return -1;
}

/* Row B, column A is true when A dominates B. */
static bool *local_compute_dominators(IronLIR_Func *fn, bool **reachable_out) {
    int n = fn->block_count;
    bool *reachable = (bool *)calloc((size_t)n, sizeof(bool));
    bool *dom = (bool *)calloc((size_t)n * (size_t)n, sizeof(bool));
    int *work = NULL;
    if (!reachable || !dom) iron_oom_abort("value_range.c:dominators");

    if (n > 0) {
        reachable[0] = true;
        arrput(work, 0);
    }
    while (arrlen(work) > 0) {
        int bi = arrpop(work);
        IronLIR_Block *blk = fn->blocks[bi];
        for (ptrdiff_t si = 0; si < arrlen(blk->succs); si++) {
            int next = local_block_index(fn, blk->succs[si]);
            if (next >= 0 && !reachable[next]) {
                reachable[next] = true;
                arrput(work, next);
            }
        }
    }
    arrfree(work);

    for (int b = 0; b < n; b++) {
        if (!reachable[b]) continue;
        if (b == 0) dom[b * n] = true;
        else for (int d = 0; d < n; d++) dom[b * n + d] = reachable[d];
    }

    bool changed;
    do {
        changed = false;
        for (int b = 1; b < n; b++) {
            if (!reachable[b]) continue;
            bool *next = (bool *)calloc((size_t)n, sizeof(bool));
            if (!next) iron_oom_abort("value_range.c:dominator row");
            bool first_pred = true;
            IronLIR_Block *blk = fn->blocks[b];
            for (ptrdiff_t pi = 0; pi < arrlen(blk->preds); pi++) {
                int pred = local_block_index(fn, blk->preds[pi]);
                if (pred < 0 || !reachable[pred]) continue;
                if (first_pred) {
                    memcpy(next, &dom[pred * n], (size_t)n * sizeof(bool));
                    first_pred = false;
                } else {
                    for (int d = 0; d < n; d++)
                        next[d] = next[d] && dom[pred * n + d];
                }
            }
            if (first_pred) memset(next, 0, (size_t)n * sizeof(bool));
            next[b] = true;
            if (memcmp(next, &dom[b * n], (size_t)n * sizeof(bool)) != 0) {
                memcpy(&dom[b * n], next, (size_t)n * sizeof(bool));
                changed = true;
            }
            free(next);
        }
    } while (changed);

    *reachable_out = reachable;
    return dom;
}

static bool local_value_is_slot(IronLIR_Func *fn, IronLIR_ValueId vid,
                                IronLIR_ValueId slot_id) {
    if (vid == slot_id) return true; /* P2b C-local alias */
    IronLIR_Instr *in = (vid != IRON_LIR_VALUE_INVALID &&
                          (ptrdiff_t)vid < arrlen(fn->value_table))
                         ? fn->value_table[vid] : NULL;
    return in && in->kind == IRON_LIR_LOAD && in->load.ptr == slot_id;
}

static bool local_const_value(IronLIR_Func *fn, IronLIR_ValueId vid,
                              int64_t *out) {
    IronLIR_Instr *in = (vid != IRON_LIR_VALUE_INVALID &&
                          (ptrdiff_t)vid < arrlen(fn->value_table))
                         ? fn->value_table[vid] : NULL;
    if (!in || in->kind != IRON_LIR_CONST_INT) return false;
    *out = in->const_int.value;
    return true;
}

static IronLIR_InstrKind local_invert_comparison(IronLIR_InstrKind kind) {
    switch ((int)kind) {
    case IRON_LIR_LT: return IRON_LIR_GT;
    case IRON_LIR_LTE: return IRON_LIR_GTE;
    case IRON_LIR_GT: return IRON_LIR_LT;
    case IRON_LIR_GTE: return IRON_LIR_LTE;
    default: return kind;
    }
}

static bool local_is_canonical_loop_header(const char *label) {
    return label && (strstr(label, "while_header") != NULL ||
                     strstr(label, "for_header") != NULL);
}

/* Prove a canonical monotonic induction slot.  This deliberately recognizes a
 * small, high-value subset: one constant initializer, one constant-step update,
 * a single-entry natural loop, and a constant header bound.  Anything less
 * explicit remains 64-bit. */
static bool local_infer_induction(LocalProofCtx *ctx,
                                  IronLIR_ValueId slot_id,
                                  bool *dom, bool *reachable,
                                  ValueRange *out) {
    struct IronVR_LocalFuncAnalysis *lf = ctx->result;
    IronLIR_Func *fn = lf->fn;
    int n = fn->block_count;
    IronLIR_Instr *init_store = NULL, *update_store = NULL;
    int init_bi = -1, update_bi = -1;
    int64_t init_value = 0, delta = 0;
    int store_count = 0;

    for (int bi = 0; bi < n; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *st = blk->instrs[ii];
            if (st->kind != IRON_LIR_STORE || st->store.ptr != slot_id) continue;
            store_count++;
            int64_t c = 0;
            if (local_const_value(fn, st->store.value, &c)) {
                if (init_store) return false;
                init_store = st;
                init_bi = bi;
                init_value = c;
                continue;
            }
            IronLIR_Instr *expr = ((ptrdiff_t)st->store.value < arrlen(fn->value_table))
                                   ? fn->value_table[st->store.value] : NULL;
            if (!expr || (expr->kind != IRON_LIR_ADD &&
                          expr->kind != IRON_LIR_SUB) || update_store)
                return false;
            int64_t step = 0;
            if (expr->kind == IRON_LIR_ADD) {
                if (local_value_is_slot(fn, expr->binop.left, slot_id) &&
                    local_const_value(fn, expr->binop.right, &step)) {
                    delta = step;
                } else if (local_value_is_slot(fn, expr->binop.right, slot_id) &&
                           local_const_value(fn, expr->binop.left, &step)) {
                    delta = step;
                } else return false;
            } else {
                if (!local_value_is_slot(fn, expr->binop.left, slot_id) ||
                    !local_const_value(fn, expr->binop.right, &step) ||
                    __builtin_sub_overflow((int64_t)0, step, &delta))
                    return false;
            }
            if (delta == 0) return false;
            update_store = st;
            update_bi = bi;
        }
    }
    if (store_count != 2 || !init_store || !update_store) return false;

    for (int header_bi = 0; header_bi < n; header_bi++) {
        IronLIR_Block *header = fn->blocks[header_bi];
        if (!reachable[header_bi] ||
            !local_is_canonical_loop_header(header->label) ||
            header->instr_count == 0) continue;
        IronLIR_Instr *term = header->instrs[header->instr_count - 1];
        if (term->kind != IRON_LIR_BRANCH) continue;
        IronLIR_Instr *cmp = ((ptrdiff_t)term->branch.cond < arrlen(fn->value_table))
                             ? fn->value_table[term->branch.cond] : NULL;
        if (!cmp || (cmp->kind != IRON_LIR_LT && cmp->kind != IRON_LIR_LTE &&
                     cmp->kind != IRON_LIR_GT && cmp->kind != IRON_LIR_GTE))
            continue;

        IronLIR_InstrKind cmp_kind = cmp->kind;
        int64_t bound = 0;
        if (local_value_is_slot(fn, cmp->binop.left, slot_id) &&
            local_const_value(fn, cmp->binop.right, &bound)) {
            /* already normalized */
        } else if (local_value_is_slot(fn, cmp->binop.right, slot_id) &&
                   local_const_value(fn, cmp->binop.left, &bound)) {
            cmp_kind = local_invert_comparison(cmp_kind);
        } else continue;

        bool *member = (bool *)calloc((size_t)n, sizeof(bool));
        int *work = NULL;
        if (!member) iron_oom_abort("value_range.c:loop members");
        member[header_bi] = true;
        bool found_backedge = false;
        for (ptrdiff_t pi = 0; pi < arrlen(header->preds); pi++) {
            int pred = local_block_index(fn, header->preds[pi]);
            if (pred < 0 || !dom[pred * n + header_bi]) continue;
            found_backedge = true;
            if (!member[pred]) { member[pred] = true; arrput(work, pred); }
        }
        while (arrlen(work) > 0) {
            int bi = arrpop(work);
            IronLIR_Block *blk = fn->blocks[bi];
            for (ptrdiff_t pi = 0; pi < arrlen(blk->preds); pi++) {
                int pred = local_block_index(fn, blk->preds[pi]);
                if (pred >= 0 && dom[pred * n + header_bi] && !member[pred]) {
                    member[pred] = true;
                    arrput(work, pred);
                }
            }
        }
        arrfree(work);

        int then_bi = local_block_index(fn, term->branch.then_block);
        int else_bi = local_block_index(fn, term->branch.else_block);
        int outside_preds = 0;
        for (ptrdiff_t pi = 0; pi < arrlen(header->preds); pi++) {
            int pred = local_block_index(fn, header->preds[pi]);
            if (pred >= 0 && !member[pred]) outside_preds++;
        }
        bool structure_ok = found_backedge && outside_preds == 1 &&
            init_bi >= 0 && update_bi >= 0 &&
            !member[init_bi] && member[update_bi] &&
            dom[header_bi * n + init_bi] &&
            then_bi >= 0 && member[then_bi] &&
            else_bi >= 0 && !member[else_bi];
        if (!structure_ok) { free(member); continue; }

        /* The update must execute at most once between header checks.  A
         * nested/internal cycle containing the update could otherwise run it
         * an unbounded number of times before the guard is re-evaluated. */
        bool *seen = (bool *)calloc((size_t)n, sizeof(bool));
        int *repeat_work = NULL;
        if (!seen) iron_oom_abort("value_range.c:induction repeat proof");
        IronLIR_Block *update_blk = fn->blocks[update_bi];
        for (ptrdiff_t si = 0; si < arrlen(update_blk->succs); si++) {
            int succ = local_block_index(fn, update_blk->succs[si]);
            if (succ >= 0 && succ != header_bi && member[succ])
                arrput(repeat_work, succ);
        }
        bool repeats_before_header = false;
        while (arrlen(repeat_work) > 0 && !repeats_before_header) {
            int bi = arrpop(repeat_work);
            if (bi == update_bi) { repeats_before_header = true; break; }
            if (seen[bi]) continue;
            seen[bi] = true;
            IronLIR_Block *blk = fn->blocks[bi];
            for (ptrdiff_t si = 0; si < arrlen(blk->succs); si++) {
                int succ = local_block_index(fn, blk->succs[si]);
                if (succ >= 0 && succ != header_bi && member[succ])
                    arrput(repeat_work, succ);
            }
        }
        arrfree(repeat_work);
        free(seen);
        if (repeats_before_header) { free(member); continue; }

        ValueRange range = RANGE_TOP;
        if (delta > 0 && (cmp_kind == IRON_LIR_LT || cmp_kind == IRON_LIR_LTE)) {
            bool executes = cmp_kind == IRON_LIR_LT
                            ? init_value < bound : init_value <= bound;
            if (!executes) range = range_const(init_value);
            else {
                int64_t current_max = bound;
                int64_t next_max = 0;
                if (cmp_kind == IRON_LIR_LT &&
                    __builtin_sub_overflow(bound, (int64_t)1, &current_max)) {
                    free(member); continue;
                }
                if (__builtin_add_overflow(current_max, delta, &next_max)) {
                    free(member); continue;
                }
                range = (ValueRange){ .min = init_value,
                                      .max = next_max > init_value
                                             ? next_max : init_value,
                                      .is_top = false };
            }
        } else if (delta < 0 &&
                   (cmp_kind == IRON_LIR_GT || cmp_kind == IRON_LIR_GTE)) {
            bool executes = cmp_kind == IRON_LIR_GT
                            ? init_value > bound : init_value >= bound;
            if (!executes) range = range_const(init_value);
            else {
                int64_t current_min = bound;
                int64_t next_min = 0;
                if (cmp_kind == IRON_LIR_GT &&
                    __builtin_add_overflow(bound, (int64_t)1, &current_min)) {
                    free(member); continue;
                }
                if (__builtin_add_overflow(current_min, delta, &next_min)) {
                    free(member); continue;
                }
                range = (ValueRange){ .min = next_min < init_value
                                             ? next_min : init_value,
                                      .max = init_value,
                                      .is_top = false };
            }
        }
        free(member);
        if (!range.is_top) { *out = range; return true; }
    }
    return false;
}

static void analyze_local_ranges(ValueRangeAnalysis *vra, IronLIR_Func *fn) {
    if (!fn || fn->is_extern || fn->block_count == 0) return;
    size_t count = (size_t)fn->next_value_id;
    if ((size_t)arrlen(fn->value_table) > count)
        count = (size_t)arrlen(fn->value_table);
    if (count == 0) return;

    struct IronVR_LocalFuncAnalysis lf = {0};
    lf.fn = fn;
    lf.value_count = count;
    lf.value_ranges = (ValueRange *)malloc(count * sizeof(ValueRange));
    lf.alloca_ranges = (ValueRange *)malloc(count * sizeof(ValueRange));
    lf.alloca_eligible = (bool *)calloc(count, sizeof(bool));
    lf.address_observable = (bool *)calloc(count, sizeof(bool));
    unsigned char *value_state = (unsigned char *)calloc(count, 1);
    unsigned char *alloca_state = (unsigned char *)calloc(count, 1);
    if (!lf.value_ranges || !lf.alloca_ranges || !lf.alloca_eligible ||
        !lf.address_observable ||
        !value_state || !alloca_state)
        iron_oom_abort("value_range.c:analyze_local_ranges");
    for (size_t i = 0; i < count; i++) {
        lf.value_ranges[i] = RANGE_TOP;
        lf.alloca_ranges[i] = RANGE_TOP;
    }

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
            if (in->kind == IRON_LIR_ALLOCA && in->id < count &&
                local_is_generic_integer(in->alloca.alloc_type) &&
                !in->alloca.global_name &&
                !local_alloca_is_capture_alias(fn, in))
                lf.alloca_eligible[in->id] = true;
        }
    }
    local_mark_observable_allocas(&lf);

    LocalProofCtx ctx = {
        .result = &lf,
        .value_state = value_state,
        .alloca_state = alloca_state,
    };
    for (size_t i = 1; i < count; i++)
        (void)local_eval_value(&ctx, (IronLIR_ValueId)i);

    /* Cycles are TOP by default.  Recover the canonical bounded induction
     * subset with a natural-loop/dominator proof, then recompute dependent
     * values against the proven slot ranges. */
    bool *reachable = NULL;
    bool *dom = local_compute_dominators(fn, &reachable);
    bool inferred_any = false;
    for (size_t i = 1; i < count; i++) {
        if (!lf.alloca_eligible[i] || !lf.alloca_ranges[i].is_top) continue;
        ValueRange induction = RANGE_TOP;
        if (local_infer_induction(&ctx, (IronLIR_ValueId)i, dom, reachable,
                                  &induction)) {
            lf.alloca_ranges[i] = induction;
            alloca_state[i] = 2;
            inferred_any = true;
        }
    }
    if (inferred_any) {
        memset(value_state, 0, count);
        for (size_t i = 0; i < count; i++) lf.value_ranges[i] = RANGE_TOP;
        for (size_t i = 1; i < count; i++)
            (void)local_eval_value(&ctx, (IronLIR_ValueId)i);
    }
    free(dom);
    free(reachable);

    free(value_state);
    free(alloca_state);
    arrput(vra->local_functions, lf);
}

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
    if (!block_entry_ranges) return; /* avoid a lost default-map allocation */
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
    IronLIR_Instr *cond_instr = lookup_value_instr(fn, cond_vid);
    if (!cond_instr) return;

    /* Check if the condition is a comparison instruction (LT/LTE/GT/GTE/EQ/NEQ) */
    bool is_cmp = (cond_instr->kind >= IRON_LIR_EQ &&
                   cond_instr->kind <= IRON_LIR_GTE);
    if (!is_cmp) return;

    IronLIR_ValueId left_vid = cond_instr->binop.left;
    IronLIR_ValueId right_vid = cond_instr->binop.right;

    IronLIR_Instr *left_instr = NULL;
    IronLIR_Instr *right_instr = NULL;
    left_instr = lookup_value_instr(fn, left_vid);
    right_instr = lookup_value_instr(fn, right_vid);

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
    IronLIR_Instr *var_instr = lookup_value_instr(fn, var_vid);
    if (var_instr && var_instr->kind == IRON_LIR_LOAD) {
        IronLIR_ValueId alloca_vid = var_instr->load.ptr;
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
                IronLIR_Instr *ret_instr = lookup_value_instr(fn, ret_vid);
                if (ret_instr) {
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
                IronLIR_Instr *obj_instr = lookup_value_instr(fn, obj_vid);
                if (obj_instr) {
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
        analyze_local_ranges(vra, fn);
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

static const char *select_local_narrowed_type(Iron_TypeKind kind,
                                               ValueRange range) {
    if (range.is_top) return NULL;
    if (kind == IRON_TYPE_UINT) {
        if (range.min < 0) return NULL;
        if ((uint64_t)range.max <= UINT8_MAX) return "uint8_t";
        if ((uint64_t)range.max <= UINT16_MAX) return "uint16_t";
        if ((uint64_t)range.max <= UINT32_MAX) return "uint32_t";
        return NULL;
    }
    if (kind == IRON_TYPE_INT) {
        if (range.min >= INT8_MIN && range.max <= INT8_MAX) return "int8_t";
        if (range.min >= INT16_MIN && range.max <= INT16_MAX) return "int16_t";
        if (range.min >= INT32_MIN && range.max <= INT32_MAX) return "int32_t";
    }
    return NULL;
}

const char *iron_vr_get_local_narrowed_type(ValueRangeAnalysis *vra,
                                             IronLIR_Func *fn,
                                             IronLIR_ValueId value_id) {
    if (!vra || !fn || value_id == IRON_LIR_VALUE_INVALID) return NULL;
    for (ptrdiff_t i = 0; i < arrlen(vra->local_functions); i++) {
        struct IronVR_LocalFuncAnalysis *lf = &vra->local_functions[i];
        if (lf->fn != fn || value_id >= lf->value_count) continue;
        IronLIR_Instr *in = ((ptrdiff_t)value_id < arrlen(fn->value_table))
                             ? fn->value_table[value_id] : NULL;
        if (!in) return NULL; /* synthetic parameter or eliminated value */
        Iron_Type *type = in->kind == IRON_LIR_ALLOCA
                          ? in->alloca.alloc_type : in->type;
        if (!local_is_generic_integer(type)) return NULL;
        ValueRange range = in->kind == IRON_LIR_ALLOCA
                           ? lf->alloca_ranges[value_id]
                           : lf->value_ranges[value_id];
        return select_local_narrowed_type(type->kind, range);
    }
    return NULL;
}

void iron_vr_free(ValueRangeAnalysis *vra) {
    if (!vra) return;
    shfree(vra->field_ranges);
    vra->field_ranges = NULL;
    shfree(vra->func_return_ranges);
    vra->func_return_ranges = NULL;
    for (ptrdiff_t i = 0; i < arrlen(vra->local_functions); i++) {
        free(vra->local_functions[i].value_ranges);
        free(vra->local_functions[i].alloca_ranges);
        free(vra->local_functions[i].alloca_eligible);
        free(vra->local_functions[i].address_observable);
    }
    arrfree(vra->local_functions);
    vra->local_functions = NULL;
}
