/* fusion_safety.c -- see fusion_safety.h. */
#include "lir/fusion_safety.h"
#include "analyzer/types.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FS_UNKNOWN = 0, FS_ACTIVE, FS_SAFE, FS_UNSAFE };

typedef struct { const IronLIR_Func *key; int state; char *reason; } FsEntry;
typedef struct { char *key; IronLIR_Func *value; } FsName;

struct IronLIR_FusionSafety {
    IronLIR_Module *module;
    FsEntry        *funcs;    /* stb_ds hash: function -> verdict */
    FsName         *by_name;  /* stb_ds string hash: name -> function */
};

IronLIR_FusionSafety *iron_lir_fusion_safety_new(IronLIR_Module *module) {
    IronLIR_FusionSafety *s = (IronLIR_FusionSafety *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->module = module;
    for (int i = 0; module && i < module->func_count; i++) {
        IronLIR_Func *fn = module->funcs[i];
        if (fn && fn->name) shput(s->by_name, (char *)fn->name, fn);
    }
    return s;
}

void iron_lir_fusion_safety_free(IronLIR_FusionSafety *s) {
    if (!s) return;
    for (ptrdiff_t i = 0; i < hmlen(s->funcs); i++) free(s->funcs[i].reason);
    hmfree(s->funcs);
    shfree(s->by_name);
    free(s);
}

IronLIR_Func *iron_lir_fusion_find_func(IronLIR_FusionSafety *s, const char *name) {
    if (!s || !name) return NULL;
    ptrdiff_t i = shgeti(s->by_name, name);
    return i >= 0 ? s->by_name[i].value : NULL;
}

static IronLIR_Instr *fs_def(IronLIR_Func *fn, IronLIR_ValueId v) {
    if (v == IRON_LIR_VALUE_INVALID || v >= (IronLIR_ValueId)arrlen(fn->value_table))
        return NULL;
    return fn->value_table[v];
}

static Iron_Type *fs_type(IronLIR_Func *fn, IronLIR_ValueId v) {
    IronLIR_Instr *d = fs_def(fn, v);
    if (d) return d->type;
    if (v >= 1 && v <= (IronLIR_ValueId)fn->param_count) return fn->params[v - 1].type;
    return NULL;
}

/* A shift panics on a negative count, so only a constant count >= 0 is free
 * of that (a count past the width yields 0 or the sign, by definition). */
static bool fs_shift_is_safe(IronLIR_Func *fn, IronLIR_Instr *in) {
    IronLIR_Instr *count = fs_def(fn, in->binop.right);
    return count && count->kind == IRON_LIR_CONST_INT && count->const_int.value >= 0;
}

/* Integer division and modulo panic on a zero divisor. */
static bool fs_div_is_safe(IronLIR_Func *fn, IronLIR_Instr *in) {
    if (!in->type) return false;
    if (!iron_type_is_integer(in->type)) return true;
    IronLIR_Instr *d = fs_def(fn, in->binop.right);
    return d && d->kind == IRON_LIR_CONST_INT && d->const_int.value != 0;
}

/* A field read is a plain member access only on a value held directly: a
 * heap, rc or pointer receiver reads shared storage behind a liveness
 * check. */
static bool fs_field_read_is_safe(IronLIR_Func *fn, IronLIR_Instr *in) {
    IronLIR_ValueId obj = in->field.object;
    Iron_Type *ot = fs_type(fn, obj);
    if (!ot) return false;
    if (ot->kind != IRON_TYPE_OBJECT && ot->kind != IRON_TYPE_TUPLE &&
        ot->kind != IRON_TYPE_ENUM)
        return false;
    IronLIR_Instr *d = fs_def(fn, obj);
    if (!d) return true;   /* a by-value parameter */
    if (d->kind == IRON_LIR_HEAP_ALLOC || d->kind == IRON_LIR_RC_ALLOC) return false;
    if (d->kind == IRON_LIR_LOAD) {
        IronLIR_Instr *slot = fs_def(fn, d->load.ptr);
        if (!slot || slot->kind != IRON_LIR_ALLOCA) return false;
        const Iron_Type *st = slot->alloca.alloc_type;
        if (st && st->kind != IRON_TYPE_OBJECT && st->kind != IRON_TYPE_TUPLE &&
            st->kind != IRON_TYPE_ENUM)
            return false;
    }
    return true;
}

static bool fs_scalar_type(const Iron_Type *t) {
    return t && (iron_type_is_numeric(t) || t->kind == IRON_TYPE_BOOL);
}

bool iron_lir_fusion_instr_is_value_only(IronLIR_Func *fn, IronLIR_Instr *in) {
    if (!fn || !in) return false;
    switch ((int)in->kind) {
    case IRON_LIR_CONST_INT: case IRON_LIR_CONST_FLOAT: case IRON_LIR_CONST_BOOL:
    case IRON_LIR_FUNC_REF:
        return true;
    case IRON_LIR_ADD: case IRON_LIR_SUB: case IRON_LIR_MUL:
    case IRON_LIR_EQ: case IRON_LIR_NEQ: case IRON_LIR_LT: case IRON_LIR_LTE:
    case IRON_LIR_GT: case IRON_LIR_GTE:
    case IRON_LIR_AND: case IRON_LIR_OR:
    case IRON_LIR_BAND: case IRON_LIR_BOR: case IRON_LIR_BXOR:
        /* (+ and == on strings or objects are runtime or hook calls.) */
        return fs_scalar_type(fs_type(fn, in->binop.left)) &&
               fs_scalar_type(fs_type(fn, in->binop.right));
    case IRON_LIR_NEG: case IRON_LIR_NOT: case IRON_LIR_BNOT:
        return fs_scalar_type(fs_type(fn, in->unop.operand));
    case IRON_LIR_DIV: case IRON_LIR_MOD:
        return fs_scalar_type(fs_type(fn, in->binop.left)) && fs_div_is_safe(fn, in);
    case IRON_LIR_SHL: case IRON_LIR_SHR:
        return fs_shift_is_safe(fn, in);
    case IRON_LIR_CAST:
        /* Numeric conversions wrap or saturate; they never panic. */
        return fs_scalar_type(in->cast.target_type) &&
               fs_scalar_type(fs_type(fn, in->cast.value));
    default:
        return false;
    }
}

static void fs_set(IronLIR_FusionSafety *s, IronLIR_Func *fn, int state, const char *reason) {
    ptrdiff_t i = hmgeti(s->funcs, fn);
    if (i >= 0) {
        free(s->funcs[i].reason);
        s->funcs[i].state = state;
        s->funcs[i].reason = reason ? strdup(reason) : NULL;
        return;
    }
    FsEntry e = { fn, state, reason ? strdup(reason) : NULL };
    hmputs(s->funcs, e);
}

static const char *fs_call_name(IronLIR_Func *fn, IronLIR_Instr *call) {
    IronLIR_Instr *ref = fs_def(fn, call->call.func_ptr);
    if (ref && ref->kind == IRON_LIR_FUNC_REF) return ref->func_ref.func_name;
    if (call->call.func_ptr == IRON_LIR_VALUE_INVALID && call->call.func_decl)
        return call->call.func_decl->name;
    return NULL;
}

static bool fs_is_capture(IronLIR_Func *fn, const char *name) {
    if (!name) return false;
    for (int i = 0; i < fn->capture_count; i++)
        if (fn->capture_metadata && fn->capture_metadata[i].name &&
            strcmp(fn->capture_metadata[i].name, name) == 0)
            return true;
    return false;
}

static const char *fs_instr_reason(IronLIR_FusionSafety *s, IronLIR_Func *fn,
                                   IronLIR_Instr *in, char *buf, size_t n);

const char *iron_lir_fusion_unsafe_reason(IronLIR_FusionSafety *s, IronLIR_Func *fn) {
    if (!s || !fn) return "is not a function of this program";
    ptrdiff_t at = hmgeti(s->funcs, fn);
    if (at >= 0) {
        if (s->funcs[at].state == FS_SAFE) return NULL;
        if (s->funcs[at].state == FS_UNSAFE) return s->funcs[at].reason;
        return "is recursive";   /* FS_ACTIVE: reached itself */
    }
    if (fn->is_extern || fn->block_count == 0) {
        fs_set(s, fn, FS_UNSAFE, "is implemented outside Iron, so its effects are unknown");
        return s->funcs[hmgeti(s->funcs, fn)].reason;
    }
    if (fn->param_is_var) {
        for (int i = 0; i < fn->param_count; i++) {
            if (fn->param_is_var[i]) {
                fs_set(s, fn, FS_UNSAFE, "writes through a `var` parameter");
                return s->funcs[hmgeti(s->funcs, fn)].reason;
            }
        }
    }
    fs_set(s, fn, FS_ACTIVE, NULL);
    /* The reason reported is the most telling one: a call or a write before
     * an operation that can panic, before the String a println builds. */
    char buf[320];
    const char *why = NULL;
    for (int pass = 0; pass < 3 && !why; pass++) {
        for (int bi = 0; bi < fn->block_count && !why; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count && !why; ii++) {
                IronLIR_Instr *in = block->instrs[ii];
                bool effect = in->kind == IRON_LIR_CALL || in->kind == IRON_LIR_STORE ||
                              in->kind == IRON_LIR_SET_FIELD || in->kind == IRON_LIR_SET_INDEX ||
                              in->kind == IRON_LIR_PTR_STORE;
                bool trap = in->kind == IRON_LIR_DIV || in->kind == IRON_LIR_MOD ||
                            in->kind == IRON_LIR_SHL || in->kind == IRON_LIR_SHR ||
                            in->kind == IRON_LIR_GET_INDEX;
                if ((pass == 0 && !effect) || (pass == 1 && !trap)) continue;
                why = fs_instr_reason(s, fn, in, buf, sizeof(buf));
            }
        }
    }
    fs_set(s, fn, why ? FS_UNSAFE : FS_SAFE, why);
    return why ? s->funcs[hmgeti(s->funcs, fn)].reason : NULL;
}

/* NULL when the instruction is allowed in a fused callback. */
static const char *fs_instr_reason(IronLIR_FusionSafety *s, IronLIR_Func *fn,
                                   IronLIR_Instr *in, char *buf, size_t n) {
    if (!in) return NULL;
    if (iron_lir_fusion_instr_is_value_only(fn, in)) return NULL;
    switch ((int)in->kind) {
    case IRON_LIR_CONST_INT: case IRON_LIR_CONST_FLOAT: case IRON_LIR_CONST_BOOL:
    case IRON_LIR_CONST_NULL: case IRON_LIR_IS_NULL: case IRON_LIR_IS_NOT_NULL:
    case IRON_LIR_JUMP: case IRON_LIR_BRANCH: case IRON_LIR_SWITCH:
    case IRON_LIR_RETURN: case IRON_LIR_PHI:
        return NULL;
    case IRON_LIR_DIV: case IRON_LIR_MOD:
        return "divides by a value that may be zero";
    case IRON_LIR_SHL: case IRON_LIR_SHR:
        return "shifts by a count that may be negative";
    case IRON_LIR_ALLOCA:
        if (in->alloca.is_boxed) return "shares a variable with another closure";
        return NULL;
    case IRON_LIR_LOAD: {
        IronLIR_Instr *slot = fs_def(fn, in->load.ptr);
        if (!slot || slot->kind != IRON_LIR_ALLOCA) return "reads through a pointer";
        return NULL;
    }
    case IRON_LIR_STORE: {
        IronLIR_Instr *slot = fs_def(fn, in->store.ptr);
        if (!slot || slot->kind != IRON_LIR_ALLOCA) return "writes through a pointer";
        if (slot->alloca.global_name) {
            snprintf(buf, n, "writes the global '%s'", slot->alloca.global_name);
            return buf;
        }
        if (slot->alloca.is_boxed || fs_is_capture(fn, slot->alloca.name_hint)) {
            snprintf(buf, n, "writes the captured variable '%s'",
                     slot->alloca.name_hint ? slot->alloca.name_hint : "?");
            return buf;
        }
        return NULL;
    }
    case IRON_LIR_GET_FIELD:
        if (fs_field_read_is_safe(fn, in)) return NULL;
        return "reads a field through a heap, rc or pointer value";
    case IRON_LIR_GET_INDEX:
        if (in->index.bounds_elide) return NULL;
        return "indexes a list or array, which can panic";
    case IRON_LIR_CONSTRUCT:
        if (in->type && (in->type->kind == IRON_TYPE_OBJECT ||
                         in->type->kind == IRON_TYPE_TUPLE))
            return NULL;
        return "builds a value that owns memory";
    case IRON_LIR_CALL: {
        const char *name = fs_call_name(fn, in);
        if (!name) return "calls a function value";
        IronLIR_Func *callee = iron_lir_fusion_find_func(s, name);
        if (!callee) {
            snprintf(buf, n, "calls '%s', whose effects the compiler cannot rule out",
                     name[0] == '$' ? "a destructor" : name);
            return buf;
        }
        if (callee == fn) return "is recursive";
        const char *why = iron_lir_fusion_unsafe_reason(s, callee);
        if (!why) return NULL;
        /* Keep the innermost reason: "calls 'a', which calls 'b', which ..."
         * stops helping after one level. */
        if (strncmp(why, "calls '", 7) == 0 && strstr(why, "', which ")) {
            const char *leaf = strstr(why, "', which ") + 9;
            snprintf(buf, n, "calls '%s', which %s", callee->name, leaf);
        } else {
            snprintf(buf, n, "calls '%s', which %s", callee->name, why);
        }
        return buf;
    }
    case IRON_LIR_SET_FIELD: case IRON_LIR_SET_INDEX: case IRON_LIR_PTR_STORE:
        return "writes into an object, a list or through a pointer";
    case IRON_LIR_CONST_STRING: case IRON_LIR_INTERP_STRING:
        return "builds a String";
    case IRON_LIR_ARRAY_LIT: case IRON_LIR_SLICE:
        return "builds a list";
    case IRON_LIR_HEAP_ALLOC: case IRON_LIR_RC_ALLOC: case IRON_LIR_ARENA_ALLOC:
        return "allocates";
    case IRON_LIR_MAKE_CLOSURE:
        return "creates a closure";
    case IRON_LIR_SPAWN: case IRON_LIR_PARALLEL_FOR: case IRON_LIR_AWAIT:
        return "starts or awaits a task";
    case IRON_LIR_GENCHECK: case IRON_LIR_PTR_LOAD:
        return "reads through a pointer";
    default:
        return "performs an operation the compiler cannot prove free of effects";
    }
}

/* ── W0618: a hinted function that cannot be fused ────────────────────────── */

void iron_lir_check_fusible_hints(IronLIR_Module *module, Iron_DiagList *diags,
                                  Iron_Arena *arena) {
    if (!module || !diags) return;
    IronLIR_FusionSafety *s = NULL;
    /* A generic function is lowered once per instance: one warning per hint. */
    struct { uint64_t key; bool value; } *seen = NULL;
    for (int i = 0; i < module->func_count; i++) {
        IronLIR_Func *fn = module->funcs[i];
        if (!fn || !fn->is_fusible_hint) continue;
        uint64_t where = ((uint64_t)fn->fusible_hint_span.line << 20) ^
                         (uint64_t)(uintptr_t)fn->fusible_hint_span.filename;
        if (hmgeti(seen, where) >= 0) continue;
        if (!s) s = iron_lir_fusion_safety_new(module);
        const char *why = iron_lir_fusion_unsafe_reason(s, fn);
        if (!why) continue;
        hmput(seen, where, true);
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "`//iron:fusible` function '%s' cannot run inside a fused loop: it %s",
                 fn->fusible_hint_name ? fn->fusible_hint_name : fn->name, why);
        iron_diag_emit(diags, arena, IRON_DIAG_WARNING, IRON_WARN_FUSIBLE_HINT_UNMET,
                       fn->fusible_hint_span,
                       iron_arena_strdup(arena, msg, strlen(msg)),
                       "list pipelines that call it keep one loop per stage; "
                       "remove the hint or make the function free of effects and panics");
    }
    hmfree(seen);
    iron_lir_fusion_safety_free(s);
}
