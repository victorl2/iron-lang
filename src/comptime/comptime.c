/* comptime.c — Tree-walking AST interpreter for Iron compile-time evaluation.
 *
 * Implements:
 *   iron_comptime_eval_expr()  — evaluate a single expression node
 *   iron_comptime_val_to_ast() — convert comptime value back to AST literal
 *   iron_comptime_apply()      — replace all IRON_NODE_COMPTIME nodes in program
 */

#include "comptime/comptime.h"
#include "lexer/lexer.h"
#include "analyzer/types.h"
#include "util/arena.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>

#include "util/pthread_compat.h"
/* WR-10: <sys/stat.h> and <errno.h> were only needed by the
 * comptime_cache_{read,write} functions, which have been deleted as
 * dead code (cache read returned a value that was always discarded,
 * cache write had no corresponding live reader). */

/* ── Internal helpers ────────────────────────────────────────────────────── */

#define IRON_COMPTIME_MAX_DEPTH 256

/* The evaluator recurses once per nested Iron call; its frames stay small
 * (message buffers live in out-of-line helpers) so 256 calls fit in a
 * 1 MB thread stack, the Windows default. */
#if defined(__GNUC__) || defined(__clang__)
#define IRON_COMPTIME_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define IRON_COMPTIME_NOINLINE __declspec(noinline)
#else
#define IRON_COMPTIME_NOINLINE
#endif

static Iron_ComptimeVal *cval_alloc(Iron_ComptimeCtx *ctx,
                                     Iron_ComptimeValKind kind) {
    Iron_ComptimeVal *v = iron_arena_alloc(ctx->arena,
                                            sizeof(Iron_ComptimeVal),
                                            _Alignof(Iron_ComptimeVal));
    if (!v) { /* HARD-09 REPLACE (comptime.c:cval_alloc) */ return NULL; }
    v->kind = kind;
    return v;
}

static Iron_ComptimeVal *cval_int(Iron_ComptimeCtx *ctx, int64_t n) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_INT);
    v->as_int = n;
    return v;
}

static Iron_ComptimeVal *cval_float(Iron_ComptimeCtx *ctx, double d) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_FLOAT);
    v->as_float = d;
    return v;
}

static Iron_ComptimeVal *cval_bool(Iron_ComptimeCtx *ctx, bool b) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_BOOL);
    v->as_bool = b;
    return v;
}

static Iron_ComptimeVal *cval_string(Iron_ComptimeCtx *ctx,
                                      const char *data, size_t len) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_STRING);
    v->as_string.data = data;
    v->as_string.len  = len;
    return v;
}

/* A copy of v with storage of its own: lists and objects are values. */
static Iron_ComptimeVal *cval_copy(Iron_ComptimeCtx *ctx, Iron_ComptimeVal *v) {
    if (!v) return v;
    if (v->kind == IRON_CVAL_ARRAY) {
        Iron_ComptimeVal *c = cval_alloc(ctx, IRON_CVAL_ARRAY);
        if (!c) return v;
        c->as_array.count = v->as_array.count;
        c->as_array.elems = iron_arena_alloc(ctx->arena,
            (size_t)(v->as_array.count ? v->as_array.count : 1) * sizeof(Iron_ComptimeVal *),
            _Alignof(Iron_ComptimeVal *));
        if (!c->as_array.elems) return v;
        for (int i = 0; i < v->as_array.count; i++)
            c->as_array.elems[i] = cval_copy(ctx, v->as_array.elems[i]);
        return c;
    }
    if (v->kind == IRON_CVAL_STRUCT) {
        Iron_ComptimeVal *c = cval_alloc(ctx, IRON_CVAL_STRUCT);
        if (!c) return v;
        c->as_struct = v->as_struct;
        c->as_struct.field_vals = iron_arena_alloc(ctx->arena,
            (size_t)(v->as_struct.field_count ? v->as_struct.field_count : 1) *
                sizeof(Iron_ComptimeVal *), _Alignof(Iron_ComptimeVal *));
        if (!c->as_struct.field_vals) return v;
        for (int i = 0; i < v->as_struct.field_count; i++)
            c->as_struct.field_vals[i] = cval_copy(ctx, v->as_struct.field_vals[i]);
        return c;
    }
    return v;
}

static Iron_ComptimeVal *cval_null(Iron_ComptimeCtx *ctx) {
    return cval_alloc(ctx, IRON_CVAL_NULL);
}

/* ── HARD-02: LSP-mode comptime FS gating ──────────────────────────────────
 * LSP mode disables every filesystem side effect in the comptime stage:
 *   - no `read_file(path)` builtin — emits IRON_ERR_COMPTIME_FS_DISABLED_IN_LSP_MODE
 * CLI mode preserves existing behaviour byte-for-byte (parity with `iron check`).
 *
 * This helper is the single decision site; every FS call site routes through it
 * so the audit surface is one function, not N scattered checks. Plan 05 Task 01.
 *
 * WR-10: formerly this helper also gated the comptime source-hash cache
 * (comptime_cache_{read,write}), but the cache machinery was deleted as
 * dead code — read-side returns were discarded at the only call site,
 * write-side had no live reader, and the FS-I/O surface it added was
 * strictly cost for HARD-02 with zero benefit. The only remaining FS
 * consumer is read_file(). fnv1a_hash, <sys/stat.h>, and <errno.h> were
 * removed along with the cache.
 */
static inline bool comptime_fs_allowed(IronAnalysisMode mode) {
    return mode == IRON_ANALYSIS_MODE_CLI;
}

/* ── Local scope management ──────────────────────────────────────────────── */

static void push_frame(Iron_ComptimeCtx *ctx) {
    Iron_ComptimeBinding *frame = NULL;  /* empty stb_ds hashmap */
    sh_new_strdup(frame);
    arrput(ctx->local_frames, frame);
    ctx->frame_depth++;
}

static void pop_frame(Iron_ComptimeCtx *ctx) {
    if (ctx->frame_depth <= 0) return;
    Iron_ComptimeBinding *frame = ctx->local_frames[ctx->frame_depth - 1];
    shfree(frame);
    arrsetlen(ctx->local_frames, ctx->frame_depth - 1);
    ctx->frame_depth--;
}

static void bind_local(Iron_ComptimeCtx *ctx, const char *name,
                        Iron_ComptimeVal *val) {
    if (ctx->frame_depth <= 0) return;
    Iron_ComptimeBinding *frame = ctx->local_frames[ctx->frame_depth - 1];
    shput(frame, name, val);
    /* Update the frame pointer back (stb_ds may realloc) */
    ctx->local_frames[ctx->frame_depth - 1] = frame;
}

static Iron_ComptimeVal *lookup_local(Iron_ComptimeCtx *ctx,
                                       const char *name) {
    /* Search from innermost frame outward */
    for (int i = ctx->frame_depth - 1; i >= 0; i--) {
        Iron_ComptimeBinding *frame = ctx->local_frames[i];
        int idx = shgeti(frame, name);
        if (idx >= 0) {
            return frame[idx].value;
        }
    }
    return NULL;
}

/* ── Error helpers ───────────────────────────────────────────────────────── */

/* Build a call trace hint string from the call stack. */
static const char *build_call_trace(Iron_ComptimeCtx *ctx, Iron_Arena *arena) {
    if (ctx->call_depth == 0) return NULL;

    /* One line: a help with newlines rendered as broken, repeated lines. */
    Iron_StrBuf sb = iron_strbuf_create(256);
    iron_strbuf_appendf(&sb, "evaluating ");
    for (int i = 0; i < ctx->call_depth; i++) {
        iron_strbuf_appendf(&sb, "%s%s()", i ? " -> " : "", ctx->call_stack[i]);
    }
    const char *result = iron_arena_strdup(arena, sb.data, sb.len);
    if (!result) { /* HARD-09 REPLACE (comptime.c:build_call_trace) */ return NULL; }
    iron_strbuf_free(&sb);
    return result;
}

static void emit_error_help(Iron_ComptimeCtx *ctx, int code, Iron_Span span,
                            const char *message, const char *help) {
    if (ctx->had_error) return;  /* only emit first error per comptime expr */
    ctx->had_error = true;
    const char *trace = build_call_trace(ctx, ctx->arena);
    const char *hint = help;
    if (help && trace) {
        size_t n = strlen(help) + strlen(trace) + 4;
        char *buf = (char *)iron_arena_alloc(ctx->arena, n, 1);
        if (buf) { snprintf(buf, n, "%s (%s)", help, trace); hint = buf; }
    } else if (trace) {
        hint = trace;
    }
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   message, hint);
}

static void emit_error(Iron_ComptimeCtx *ctx, int code, Iron_Span span,
                        const char *message) {
    emit_error_help(ctx, code, span, message, NULL);
}

/* ── Statement evaluator (forward declaration) ───────────────────────────── */

static void eval_stmts(Iron_ComptimeCtx *ctx, Iron_Node **stmts, int count);
static void eval_stmt(Iron_ComptimeCtx *ctx, Iron_Node *node);

/* ── Expression evaluator ────────────────────────────────────────────────── */

/* ── Integer widths ──────────────────────────────────────────────────────
 * Comptime integers are carried in an int64_t; the expression's resolved
 * type gives the width and signedness, so every operation wraps, compares,
 * divides and shifts as the same operation does at run time (manual 2.7):
 * a value of a narrower type is kept sign- or zero-extended from its width,
 * a UInt64 keeps its bit pattern. */

static Iron_Type *ct_type(Iron_Node *n) {
    return n ? ((Iron_ExprNode *)n)->resolved_type : NULL;
}

/* Bits of integer type `t` (64 for anything that is not a narrower
 * integer type) and whether it is unsigned. */
static int ct_int_width(const Iron_Type *t, bool *uns) {
    *uns = false;
    if (!t) return 64;
    switch ((int)t->kind) {
    case IRON_TYPE_INT8:   return 8;
    case IRON_TYPE_INT16:  return 16;
    case IRON_TYPE_INT32:  return 32;
    case IRON_TYPE_UINT8:  *uns = true; return 8;
    case IRON_TYPE_UINT16: *uns = true; return 16;
    case IRON_TYPE_UINT32: *uns = true; return 32;
    case IRON_TYPE_UINT:
    case IRON_TYPE_UINT64: *uns = true; return 64;
    default:               return 64;
    }
}

static bool ct_is_int_type(const Iron_Type *t) {
    return t && iron_type_is_integer(t);
}

/* `v` reduced to the width of `t`: what a C cast to that type gives. */
static int64_t ct_wrap(int64_t v, const Iron_Type *t) {
    bool uns;
    int w = ct_int_width(t, &uns);
    if (w >= 64) return v;
    uint64_t m = (UINT64_C(1) << w) - 1;
    uint64_t x = (uint64_t)v & m;
    if (!uns && (x >> (w - 1))) x |= ~m;
    return (int64_t)x;
}

/* Apply a non-short-circuit binary operator to two evaluated operands.
 * Shared by binary expressions and compound assignment (`x op= v`).
 * `res_t` is the result type (a shift's left operand type), `opnd_t` the
 * operands' type (for comparisons, division and shifts). */
static Iron_ComptimeVal *eval_binary_vals(Iron_ComptimeCtx *ctx, Iron_OpKind op,
                                          Iron_ComptimeVal *lv, Iron_ComptimeVal *rv,
                                          Iron_Node *node, Iron_Type *res_t,
                                          Iron_Type *opnd_t) {
    /* Integer arithmetic */
    if (lv->kind == IRON_CVAL_INT && rv->kind == IRON_CVAL_INT) {
        bool uns;
        (void)ct_int_width(ct_is_int_type(opnd_t) ? opnd_t : res_t, &uns);
        Iron_Type *rt = ct_is_int_type(res_t) ? res_t : NULL;
        uint64_t a = (uint64_t)lv->as_int, b = (uint64_t)rv->as_int;
        switch (op) {
            /* + - * wrap at the type's width, as at run time. */
            case IRON_TOK_PLUS:  return cval_int(ctx, ct_wrap((int64_t)(a + b), rt));
            case IRON_TOK_MINUS: return cval_int(ctx, ct_wrap((int64_t)(a - b), rt));
            case IRON_TOK_STAR:  return cval_int(ctx, ct_wrap((int64_t)(a * b), rt));
            case IRON_TOK_SLASH:
            case IRON_TOK_PERCENT: {
                bool div = op == IRON_TOK_SLASH;
                if (rv->as_int == 0) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                               div ? "comptime: division by zero"
                                   : "comptime: modulo by zero");
                    return cval_null(ctx);
                }
                if (uns) return cval_int(ctx, ct_wrap((int64_t)(div ? a / b : a % b), rt));
                /* INT64_MIN / -1 wraps (and % gives 0), as at run time. */
                if (lv->as_int == INT64_MIN && rv->as_int == -1)
                    return cval_int(ctx, div ? INT64_MIN : 0);
                return cval_int(ctx, ct_wrap(div ? lv->as_int / rv->as_int
                                                 : lv->as_int % rv->as_int, rt));
            }
            case IRON_TOK_EQUALS:     return cval_bool(ctx, a == b);
            case IRON_TOK_NOT_EQUALS: return cval_bool(ctx, a != b);
            case IRON_TOK_LESS:       return cval_bool(ctx, uns ? a <  b : lv->as_int <  rv->as_int);
            case IRON_TOK_GREATER:    return cval_bool(ctx, uns ? a >  b : lv->as_int >  rv->as_int);
            case IRON_TOK_LESS_EQ:    return cval_bool(ctx, uns ? a <= b : lv->as_int <= rv->as_int);
            case IRON_TOK_GREATER_EQ: return cval_bool(ctx, uns ? a >= b : lv->as_int >= rv->as_int);
            case IRON_TOK_AMP:   return cval_int(ctx, ct_wrap((int64_t)(a & b), rt));
            case IRON_TOK_PIPE:  return cval_int(ctx, ct_wrap((int64_t)(a | b), rt));
            case IRON_TOK_CARET: return cval_int(ctx, ct_wrap((int64_t)(a ^ b), rt));
            case IRON_TOK_SHL:
            case IRON_TOK_SHR: {
                /* A shift has the type of its left operand. A count of its
                 * width or more gives 0 for << and the sign (0 or -1) for
                 * >>; a negative count is a panic at run time (manual 2.7). */
                bool luns;
                int w = ct_int_width(rt, &luns);
                bool cuns;
                Iron_Type *ct = (node && node->kind == IRON_NODE_BINARY)
                    ? ct_type(((Iron_BinaryExpr *)node)->right) : NULL;
                (void)ct_int_width(ct, &cuns);
                if (!cuns && rv->as_int < 0) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                               "comptime: shift by a negative count");
                    return cval_null(ctx);
                }
                uint64_t m = w >= 64 ? UINT64_MAX : (UINT64_C(1) << w) - 1;
                if (b >= (uint64_t)w) {
                    if (op == IRON_TOK_SHL || luns) return cval_int(ctx, 0);
                    return cval_int(ctx, lv->as_int < 0 ? -1 : 0);
                }
                if (op == IRON_TOK_SHL) return cval_int(ctx, ct_wrap((int64_t)(a << b), rt));
                if (luns) return cval_int(ctx, (int64_t)((a & m) >> b));
                /* arithmetic: the value is sign-extended from its width */
                return cval_int(ctx, lv->as_int < 0 ? ~(int64_t)(~a >> b)
                                                    : (int64_t)(a >> b));
            }
            /* -Wswitch-enum opt-out: other operators fall through to the
             * generic "unsupported binary operation" diagnostic below. */
            default: break;
        }
    }

    /* Float arithmetic */
    if (lv->kind == IRON_CVAL_FLOAT || rv->kind == IRON_CVAL_FLOAT) {
        double l = (lv->kind == IRON_CVAL_FLOAT) ? lv->as_float : (double)lv->as_int;
        double r = (rv->kind == IRON_CVAL_FLOAT) ? rv->as_float : (double)rv->as_int;
        switch (op) {
            case IRON_TOK_PLUS:       return cval_float(ctx, l + r);
            case IRON_TOK_MINUS:      return cval_float(ctx, l - r);
            case IRON_TOK_STAR:       return cval_float(ctx, l * r);
            case IRON_TOK_SLASH:
                /* IEEE: a float divided by zero is an infinity or NaN at run
                 * time, so it is the same here (manual 8: the evaluation
                 * computes what the call computes at run time). */
                if (lv->kind != IRON_CVAL_FLOAT && rv->kind != IRON_CVAL_FLOAT && r == 0.0) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                               "comptime: division by zero");
                    return cval_null(ctx);
                }
                return cval_float(ctx, l / r);
            case IRON_TOK_PERCENT:
                /* fmod, as at run time (#505): a zero divisor gives NaN. */
                return cval_float(ctx, iron_comptime_fmod(l, r));
            case IRON_TOK_EQUALS:     return cval_bool(ctx, l == r);
            case IRON_TOK_NOT_EQUALS: return cval_bool(ctx, l != r);
            case IRON_TOK_LESS:       return cval_bool(ctx, l < r);
            case IRON_TOK_GREATER:    return cval_bool(ctx, l > r);
            case IRON_TOK_LESS_EQ:    return cval_bool(ctx, l <= r);
            case IRON_TOK_GREATER_EQ: return cval_bool(ctx, l >= r);
            /* -Wswitch-enum opt-out: float-binop handler only supports
             * arithmetic + comparison ops; others fall through. */
            default: break;
        }
    }

    /* Bool comparisons */
    if (lv->kind == IRON_CVAL_BOOL && rv->kind == IRON_CVAL_BOOL) {
        switch (op) {
            case IRON_TOK_EQUALS:     return cval_bool(ctx, lv->as_bool == rv->as_bool);
            case IRON_TOK_NOT_EQUALS: return cval_bool(ctx, lv->as_bool != rv->as_bool);
            /* -Wswitch-enum opt-out: bool-binop handler only supports
             * equality; comparisons and arithmetic on bool fall through. */
            default: break;
        }
    }

    {
        /* String concatenation or comparison and operators on mixed
         * operands are not available at compile time (manual 8): E0231. */
        bool on_strings = lv->kind == IRON_CVAL_STRING || rv->kind == IRON_CVAL_STRING;
        emit_error_help(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                        on_strings
                            ? "comptime: string operators are not available at compile time"
                            : "comptime: this operator is not available at compile time",
                        "compile-time evaluation covers + - * / %, comparisons, "
                        "& | ^ ~ << >>, and, or and not on numbers and booleans");
    }
    return cval_null(ctx);
}

/* fmod without libm (#505). The remainder of two doubles is exact, so it
 * is computed on the significands by binary long division: align the
 * exponents, subtract y's significand while x's is larger, shifting one
 * bit per exponent step, then renormalize. Special cases follow C: a NaN
 * operand, an infinite x or a zero y give NaN; an infinite y or |x| < |y|
 * give x; |x| == |y| gives a zero with x's sign. */
double iron_comptime_fmod(double x, double y) {
    uint64_t ux, uy;
    memcpy(&ux, &x, sizeof ux);
    memcpy(&uy, &y, sizeof uy);
    int ex = (int)(ux >> 52 & 0x7ff);
    int ey = (int)(uy >> 52 & 0x7ff);
    uint64_t sx = ux >> 63;
    uint64_t i;

    if ((uy << 1) == 0 || y != y || ex == 0x7ff) {
        volatile double v = x * y;   /* not folded away: a NaN at run time too */
        return v / v;
    }
    if ((ux << 1) <= (uy << 1)) {
        if ((ux << 1) == (uy << 1)) return 0 * x;
        return x;
    }
    /* Significands with the implicit bit, subnormals normalized (their
     * exponent goes below 1). */
    if (ex == 0) {
        for (i = ux << 12; (i >> 63) == 0; ex--, i <<= 1) {}
        ux <<= -ex + 1;
    } else {
        ux &= UINT64_MAX >> 12;
        ux |= (uint64_t)1 << 52;
    }
    if (ey == 0) {
        for (i = uy << 12; (i >> 63) == 0; ey--, i <<= 1) {}
        uy <<= -ey + 1;
    } else {
        uy &= UINT64_MAX >> 12;
        uy |= (uint64_t)1 << 52;
    }
    for (; ex > ey; ex--) {
        i = ux - uy;
        if ((i >> 63) == 0) {
            if (i == 0) return 0 * x;
            ux = i;
        }
        ux <<= 1;
    }
    i = ux - uy;
    if ((i >> 63) == 0) {
        if (i == 0) return 0 * x;
        ux = i;
    }
    for (; (ux >> 52) == 0; ux <<= 1, ex--) {}
    if (ex > 0) {
        ux -= (uint64_t)1 << 52;
        ux |= (uint64_t)ex << 52;
    } else {
        ux >>= -ex + 1;   /* a subnormal result */
    }
    ux |= sx << 63;
    double r;
    memcpy(&r, &ux, sizeof r);
    return r;
}

/* Map a compound-assignment token to its binary operator. */
static Iron_OpKind comptime_compound_base_op(Iron_OpKind op) {
    switch ((int)op) {
        case IRON_TOK_PLUS_ASSIGN:  return IRON_TOK_PLUS;
        case IRON_TOK_MINUS_ASSIGN: return IRON_TOK_MINUS;
        case IRON_TOK_STAR_ASSIGN:  return IRON_TOK_STAR;
        case IRON_TOK_SLASH_ASSIGN: return IRON_TOK_SLASH;
        case IRON_TOK_SHL_ASSIGN:   return IRON_TOK_SHL;
        case IRON_TOK_SHR_ASSIGN:   return IRON_TOK_SHR;
        case IRON_TOK_AMP_ASSIGN:   return IRON_TOK_AMP;
        case IRON_TOK_PIPE_ASSIGN:  return IRON_TOK_PIPE;
        case IRON_TOK_CARET_ASSIGN: return IRON_TOK_CARET;
        /* -Wswitch-enum opt-out: only compound-assign tokens map. */
        default:                    return op;
    }
}

/* Whether the program declares an anonymous init for the object. */
static bool comptime_object_has_init(Iron_ComptimeCtx *ctx, const char *type_name) {
    if (!ctx->program || !type_name) return false;
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *d = ctx->program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (md->is_init && !md->init_name && md->type_name &&
            strcmp(md->type_name, type_name) == 0)
            return true;
    }
    return false;
}

/* `iron` compiles a package as one file, target/combined.iron, whose
 * `-- @file:` markers name each part relative to the package directory;
 * it passes that directory with --package-root. */
static const char *g_package_root = NULL;

void iron_comptime_set_package_root(const char *dir) {
    g_package_root = (dir && dir[0]) ? dir : NULL;
}

const char *iron_comptime_package_root(void) {
    return g_package_root;
}

/* The directory of the file `node` comes from, into buf ("" for the
 * current directory). */
static void comptime_file_dir(Iron_ComptimeCtx *ctx, Iron_Node *node, char *buf, size_t size) {
    const char *f = node ? node->span.filename : NULL;
    buf[0] = '\0';
    if (!f || !f[0] || f[0] == '<') {
        if (ctx->source_file_dir) snprintf(buf, size, "%s", ctx->source_file_dir);
        return;
    }
    const char *slash = strrchr(f, '/');
    const char *bs = strrchr(f, '\\');
    if (bs > slash) slash = bs;
    int dlen = slash ? (int)(slash - f) : 0;
    bool absolute = f[0] == '/' || f[0] == '\\' ||
                    (f[0] && f[1] == ':' && (f[2] == '/' || f[2] == '\\'));
    if (absolute) {
        snprintf(buf, size, "%.*s", slash == f ? 1 : dlen, f);
    } else if (g_package_root) {
        if (dlen) snprintf(buf, size, "%s/%.*s", g_package_root, dlen, f);
        else snprintf(buf, size, "%s", g_package_root);
    } else if (ctx->source_file_dir && ctx->source_file_dir[0]) {
        /* a single compiled file: its directory, as the driver gives it */
        snprintf(buf, size, "%s", ctx->source_file_dir);
    } else {
        snprintf(buf, size, "%.*s", dlen, f);
    }
}

/* The file read of `read_file(path)`, out of line: its path buffer used to
 * sit in the frame of the recursive evaluator, once per nested call. */
static IRON_COMPTIME_NOINLINE Iron_ComptimeVal *comptime_read_file(Iron_ComptimeCtx *ctx,
                                                                   Iron_Node *node,
                                                                   const char *rel) {
    /* The path is relative to the source file that calls read_file (manual
     * 8): the directory of the call's own file, which in a package build is
     * a file of src/ or vendor/ concatenated into target/combined.iron, not
     * the directory of the compiled unit. */
    char full_path[4096];
    char base[4096];
    comptime_file_dir(ctx, node, base, sizeof(base));
    if (rel[0] == '/' || base[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "%s", rel);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", base, rel);
    }

    FILE *rf = fopen(full_path, "rb");
    if (!rf) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "comptime read_file: cannot open '%s'", full_path);
        const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
        if (!msg_copy) { /* HARD-09 REPLACE (comptime.c:iron_comptime_eval_expr read_file error msg) */ msg_copy = "analyzer error"; }
        emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                   msg_copy);
        return cval_null(ctx);
    }
    if (fseek(rf, 0, SEEK_END) != 0) {
        fclose(rf);
        emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                   "comptime read_file: cannot seek file");
        return cval_null(ctx);
    }
    long fsize = ftell(rf);
    rewind(rf);
    if (fsize < 0) fsize = 0;
    char *fbuf = iron_arena_alloc(ctx->arena, (size_t)fsize + 1,
                                  _Alignof(char));
    if (!fbuf) { /* HARD-09 REPLACE (comptime.c:iron_comptime_eval_expr read_file fbuf) */ return NULL; }
    size_t nread = fread(fbuf, 1, (size_t)fsize, rf);
    fclose(rf);
    fbuf[nread] = '\0';

    ctx->steps++;  /* count read_file as 1 step */
    return cval_string(ctx, fbuf, nread);
}

static IRON_COMPTIME_NOINLINE void comptime_not_callable(Iron_ComptimeCtx *ctx, Iron_Node *node,
                                                        const char *func_name) {
    char msg[256];
    snprintf(msg, sizeof(msg), "comptime: '%s' cannot be called at compile time", func_name);
    emit_error_help(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                    iron_arena_strdup(ctx->arena, msg, strlen(msg)),
                    "compile-time evaluation calls top-level Iron functions");
}

static IRON_COMPTIME_NOINLINE void comptime_unavailable_kind(Iron_ComptimeCtx *ctx,
                                                            Iron_Node *node) {
    /* Name what is not available (manual 8); "expression kind not
     * supported" left the reader to guess which part. */
    const char *what =
        node->kind == IRON_NODE_METHOD_CALL  ? "a method call" :
        node->kind == IRON_NODE_FIELD_ACCESS ? "field access" :
        node->kind == IRON_NODE_SLICE        ? "slicing" :
        node->kind == IRON_NODE_INTERP_STRING ? "string interpolation" :
        node->kind == IRON_NODE_LAMBDA       ? "a lambda" :
        node->kind == IRON_NODE_IS           ? "a type test" : "match";
    char msg[160];
    if (node->kind == IRON_NODE_METHOD_CALL && ((Iron_MethodCallExpr *)node)->method)
        snprintf(msg, sizeof(msg), "comptime: method calls are not available at compile time ('.%s()')",
                 ((Iron_MethodCallExpr *)node)->method);
    else
        snprintf(msg, sizeof(msg), "comptime: %s is not available at compile time", what);
    const char *help =
        (node->kind == IRON_NODE_METHOD_CALL && ((Iron_MethodCallExpr *)node)->method &&
         strcmp(((Iron_MethodCallExpr *)node)->method, "push") == 0)
        ? "build the list with fill(n, v) and assign its elements, xs[i] = v"
        : "compile-time evaluation covers literals, arithmetic, list indexing, "
          "object construction, len / range / fill / read_file and calls to "
          "top-level functions";
    emit_error_help(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                    iron_arena_strdup(ctx->arena, msg, strlen(msg)), help);
}

Iron_ComptimeVal *iron_comptime_eval_expr(Iron_ComptimeCtx *ctx,
                                           Iron_Node *node) {
    if (!node || ctx->had_error) return cval_null(ctx);

    /* Call depth guard: each Iron call nests a dozen C frames of this
     * evaluator, so recursion is bounded (manual 8) well inside the
     * smallest default thread stack (1 MB on Windows). */
    if (ctx->call_depth >= IRON_COMPTIME_MAX_DEPTH) {
        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, node->span,
                   "comptime evaluation nests calls more than 256 deep");
        return cval_null(ctx);
    }

    /* Step counting — tracks total work done across all calls */
    ctx->steps++;
    if (ctx->steps > ctx->step_limit) {
        Iron_Span span = node->span;
        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, span,
                   "comptime evaluation exceeded step limit (1000000 steps)");
        return cval_null(ctx);
    }

    switch ((int)(node->kind)) {

        /* ── Literals ───────────────────────────────────────────────────── */

        case IRON_NODE_INT_LIT: {
            Iron_IntLit *lit = (Iron_IntLit *)node;
            int64_t v = 0;
            if (lit->value) {
                errno = 0;
                v = (int64_t)strtoll(lit->value, NULL, 10);
                /* A UInt64 literal above the largest Int: its bit pattern. */
                if (errno == ERANGE && lit->value[0] != '-')
                    v = (int64_t)strtoull(lit->value, NULL, 10);
            }
            return cval_int(ctx, v);
        }

        case IRON_NODE_FLOAT_LIT: {
            Iron_FloatLit *lit = (Iron_FloatLit *)node;
            double v = 0.0;
            if (lit->value) {
                v = strtod(lit->value, NULL);
            }
            return cval_float(ctx, v);
        }

        case IRON_NODE_BOOL_LIT: {
            Iron_BoolLit *lit = (Iron_BoolLit *)node;
            return cval_bool(ctx, lit->value);
        }

        case IRON_NODE_STRING_LIT: {
            Iron_StringLit *lit = (Iron_StringLit *)node;
            const char *s = lit->value ? lit->value : "";
            return cval_string(ctx, s, lit->len ? lit->len : strlen(s));
        }

        case IRON_NODE_NULL_LIT: {
            return cval_null(ctx);
        }

        /* ── Identifier (variable lookup) ───────────────────────────────── */

        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;
            Iron_ComptimeVal *val = lookup_local(ctx, id->name);
            if (val) return val;
            /* A global (or anything else outside the evaluated function):
             * not available at compile time (manual 8). */
            emit_error_help(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                            "comptime: only parameters and local bindings can be read at compile time",
                            "pass the value as an argument");
            return cval_null(ctx);
        }

        /* ── Binary expression ──────────────────────────────────────────── */

        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;

            /* Short-circuit for logical operators */
            if (bin->op == IRON_TOK_AND) {
                Iron_ComptimeVal *lv = iron_comptime_eval_expr(ctx, bin->left);
                if (ctx->had_error) return cval_null(ctx);
                if (lv->kind == IRON_CVAL_BOOL && !lv->as_bool) {
                    return cval_bool(ctx, false);
                }
                Iron_ComptimeVal *rv = iron_comptime_eval_expr(ctx, bin->right);
                if (ctx->had_error) return cval_null(ctx);
                bool result = (lv->kind == IRON_CVAL_BOOL ? lv->as_bool : false) &&
                              (rv->kind == IRON_CVAL_BOOL ? rv->as_bool : false);
                return cval_bool(ctx, result);
            }
            if (bin->op == IRON_TOK_OR) {
                Iron_ComptimeVal *lv = iron_comptime_eval_expr(ctx, bin->left);
                if (ctx->had_error) return cval_null(ctx);
                if (lv->kind == IRON_CVAL_BOOL && lv->as_bool) {
                    return cval_bool(ctx, true);
                }
                Iron_ComptimeVal *rv = iron_comptime_eval_expr(ctx, bin->right);
                if (ctx->had_error) return cval_null(ctx);
                bool result = (lv->kind == IRON_CVAL_BOOL ? lv->as_bool : false) ||
                              (rv->kind == IRON_CVAL_BOOL ? rv->as_bool : false);
                return cval_bool(ctx, result);
            }

            Iron_ComptimeVal *lv = iron_comptime_eval_expr(ctx, bin->left);
            if (ctx->had_error) return cval_null(ctx);
            Iron_ComptimeVal *rv = iron_comptime_eval_expr(ctx, bin->right);
            if (ctx->had_error) return cval_null(ctx);
            return eval_binary_vals(ctx, bin->op, lv, rv, node, bin->resolved_type,
                                    ct_type(bin->left));
        }

        /* ── Unary expression ───────────────────────────────────────────── */

        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
            Iron_ComptimeVal *operand = iron_comptime_eval_expr(ctx, un->operand);
            if (ctx->had_error) return cval_null(ctx);

            if (un->op == IRON_TOK_TILDE && operand->kind == IRON_CVAL_INT) {
                Iron_Type *ut = ct_is_int_type(un->resolved_type) ? un->resolved_type
                                                                  : ct_type(un->operand);
                return cval_int(ctx, ct_wrap(~operand->as_int, ut));
            }
            if (un->op == IRON_TOK_MINUS) {
                bool neg_uns;
                if (operand->kind == IRON_CVAL_INT &&
                    ct_is_int_type(un->resolved_type) &&
                    ct_int_width(un->resolved_type, &neg_uns) < 64) {
                    return cval_int(ctx, ct_wrap((int64_t)(0u - (uint64_t)operand->as_int),
                                                 un->resolved_type));
                }
                if (operand->kind == IRON_CVAL_INT) {
                    /* FIX-01 rank 15: negation of INT64_MIN is undefined
                     * behavior (C11 6.5.3.3p3) because INT64_MAX cannot
                     * represent -(INT64_MIN) = 9223372036854775808.
                     * Reject it explicitly before the negation. */
                    /* -INT64_MIN wraps to INT64_MIN, as at run time. */
                    return cval_int(ctx, (int64_t)(0u - (uint64_t)operand->as_int));
                }
                if (operand->kind == IRON_CVAL_FLOAT)
                    return cval_float(ctx, -operand->as_float);
            }
            if (un->op == IRON_TOK_NOT) {
                if (operand->kind == IRON_CVAL_BOOL)
                    return cval_bool(ctx, !operand->as_bool);
            }

            emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                       "comptime: this operator is not available at compile time");
            return cval_null(ctx);
        }

        /* ── Function call ──────────────────────────────────────────────── */

        case IRON_NODE_CALL: {
            Iron_CallExpr *call = (Iron_CallExpr *)node;

            /* Get function name from callee ident */
            /* A call through anything but a function name (a value, a
             * generic instance, `Map[K, V]()`) is a construct comptime
             * leaves out (E0231, manual 8), not an evaluation error. */
            if (call->callee->kind != IRON_NODE_IDENT) {
                emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                           "comptime: only named function calls are supported");
                return cval_null(ctx);
            }
            Iron_Ident *callee_id = (Iron_Ident *)call->callee;
            const char *func_name = callee_id->name;

            /* ── len / range / fill builtins ──────────────────────────────── */
            if (strcmp(func_name, "len") == 0 && call->arg_count == 1) {
                Iron_ComptimeVal *v = iron_comptime_eval_expr(ctx, call->args[0]);
                if (ctx->had_error) return cval_null(ctx);
                if (v->kind == IRON_CVAL_ARRAY) return cval_int(ctx, v->as_array.count);
                if (v->kind == IRON_CVAL_STRING) {
                    /* Strings count characters (code points), not bytes. */
                    int64_t n = 0;
                    for (size_t bi = 0; bi < v->as_string.len; bi++)
                        if ((v->as_string.data[bi] & 0xC0) != 0x80) n++;
                    return cval_int(ctx, n);
                }
                emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                           "comptime len: argument must be an array or string");
                return cval_null(ctx);
            }
            if (strcmp(func_name, "range") == 0 && call->arg_count == 1) {
                /* A for loop iterates an Int n as 0..n-1. */
                Iron_ComptimeVal *v = iron_comptime_eval_expr(ctx, call->args[0]);
                if (ctx->had_error) return cval_null(ctx);
                if (v->kind != IRON_CVAL_INT) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                               "comptime range: argument must be an Int");
                    return cval_null(ctx);
                }
                return v;
            }
            if (strcmp(func_name, "fill") == 0 && call->arg_count == 2) {
                Iron_ComptimeVal *n = iron_comptime_eval_expr(ctx, call->args[0]);
                if (ctx->had_error) return cval_null(ctx);
                Iron_ComptimeVal *fv = iron_comptime_eval_expr(ctx, call->args[1]);
                if (ctx->had_error) return cval_null(ctx);
                if (n->kind != IRON_CVAL_INT || n->as_int < 0 ||
                    n->as_int > ctx->step_limit) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                               "comptime fill: count must be a non-negative Int");
                    return cval_null(ctx);
                }
                Iron_ComptimeVal *arr = cval_alloc(ctx, IRON_CVAL_ARRAY);
                arr->as_array.count = (int)n->as_int;
                arr->as_array.elems = iron_arena_alloc(ctx->arena,
                    (size_t)(n->as_int ? n->as_int : 1) * sizeof(Iron_ComptimeVal *),
                    _Alignof(Iron_ComptimeVal *));
                if (!arr->as_array.elems) return cval_null(ctx);
                for (int64_t i = 0; i < n->as_int; i++)
                    arr->as_array.elems[i] = i == 0 ? fv : cval_copy(ctx, fv);
                ctx->steps += n->as_int;
                return arr;
            }

            /* ── read_file builtin ─────────────────────────────────────── */
            if (strcmp(func_name, "read_file") == 0) {
                /* HARD-02: LSP mode forbids any FS read from comptime. Emit a
                 * targeted ERROR diagnostic (code 234) and return null so the
                 * caller keeps the existing comptime AST slot — no divergence
                 * from CLI behaviour is introduced here beyond the gate. */
                if (!comptime_fs_allowed(ctx->mode)) {
                    emit_error(ctx, IRON_ERR_COMPTIME_FS_DISABLED_IN_LSP_MODE,
                               node->span,
                               "comptime read_file: filesystem access is disabled in LSP mode");
                    return cval_null(ctx);
                }
                if (call->arg_count != 1) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                               "comptime read_file: expected exactly 1 argument");
                    return cval_null(ctx);
                }
                Iron_ComptimeVal *path_val = iron_comptime_eval_expr(ctx,
                                                                       call->args[0]);
                if (ctx->had_error) return cval_null(ctx);
                if (path_val->kind != IRON_CVAL_STRING) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                               "comptime read_file: argument must be a string");
                    return cval_null(ctx);
                }

                return comptime_read_file(ctx, node, path_val->as_string.data);
            }

            /* Numeric conversions compute what they compute at run time:
             * integer to integer keeps the low bits of the target width (a C
             * cast), integer to float rounds, float to integer saturates (NaN
             * gives 0, manual 2.9), Float32 rounds to single precision. */
            if (call->is_primitive_cast) {
                Iron_Type *to = call->resolved_type;
                Iron_ComptimeVal *v = call->arg_count == 1
                    ? iron_comptime_eval_expr(ctx, call->args[0]) : NULL;
                if (ctx->had_error) return cval_null(ctx);
                Iron_Type *from = call->arg_count == 1 ? ct_type(call->args[0]) : NULL;
                bool from_uns;
                (void)ct_int_width(from, &from_uns);
                if (v && to && ct_is_int_type(to) && v->kind == IRON_CVAL_INT)
                    return cval_int(ctx, ct_wrap(v->as_int, to));
                if (v && to && ct_is_int_type(to) && v->kind == IRON_CVAL_FLOAT) {
                    bool tuns;
                    int w = ct_int_width(to, &tuns);
                    double f = v->as_float;
                    if (f != f) return cval_int(ctx, 0);
                    if (tuns) {
                        double hi = w >= 64 ? 18446744073709551615.0
                                            : (double)((UINT64_C(1) << w) - 1);
                        if (f <= 0.0) return cval_int(ctx, 0);
                        if (f >= hi) return cval_int(ctx, w >= 64 ? (int64_t)UINT64_MAX
                                                                  : (int64_t)((UINT64_C(1) << w) - 1));
                        return cval_int(ctx, (int64_t)(uint64_t)f);
                    }
                    double lo = w >= 64 ? -9223372036854775808.0 : -(double)(INT64_C(1) << (w - 1));
                    double hi = w >= 64 ? 9223372036854775807.0 : (double)((INT64_C(1) << (w - 1)) - 1);
                    if (f <= lo) return cval_int(ctx, w >= 64 ? INT64_MIN : -(INT64_C(1) << (w - 1)));
                    if (f >= hi) return cval_int(ctx, w >= 64 ? INT64_MAX : (INT64_C(1) << (w - 1)) - 1);
                    return cval_int(ctx, (int64_t)f);
                }
                if (v && to && (to->kind == IRON_TYPE_FLOAT || to->kind == IRON_TYPE_FLOAT64 ||
                                to->kind == IRON_TYPE_FLOAT32) &&
                    (v->kind == IRON_CVAL_INT || v->kind == IRON_CVAL_FLOAT)) {
                    double f = v->kind == IRON_CVAL_FLOAT ? v->as_float
                             : from_uns ? (double)(uint64_t)v->as_int : (double)v->as_int;
                    if (to->kind == IRON_TYPE_FLOAT32) f = (double)(float)f;
                    return cval_float(ctx, f);
                }
                emit_error_help(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                                "comptime: this conversion is not available at compile time",
                                "compile-time conversions cover the integer and float types");
                return cval_null(ctx);
            }

            /* Look up function in global scope */
            Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope, func_name);

            /* `Point(1, 2)`: object construction (manual 8). The value is
             * its fields; an object with an init would have to run it,
             * which compile-time evaluation does not do. */
            if (sym && sym->sym_kind == IRON_SYM_TYPE && sym->decl_node &&
                sym->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
                if (comptime_object_has_init(ctx, od->name) ||
                    od->generic_param_count > 0 || call->arg_count != od->field_count) {
                    emit_error_help(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                                    "comptime: only an object without an init can be built at compile time",
                                    "construct it positionally, one argument per field");
                    return cval_null(ctx);
                }
                Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_STRUCT);
                if (!v) return cval_null(ctx);
                int fc = od->field_count;
                v->as_struct.type_name = od->name;
                v->as_struct.field_count = fc;
                v->as_struct.field_names = iron_arena_alloc(ctx->arena,
                    (size_t)(fc ? fc : 1) * sizeof(const char *), _Alignof(const char *));
                v->as_struct.field_vals = iron_arena_alloc(ctx->arena,
                    (size_t)(fc ? fc : 1) * sizeof(Iron_ComptimeVal *),
                    _Alignof(Iron_ComptimeVal *));
                if (!v->as_struct.field_names || !v->as_struct.field_vals) return cval_null(ctx);
                for (int i = 0; i < fc; i++) {
                    v->as_struct.field_names[i] = ((Iron_Field *)od->fields[i])->name;
                    /* fields are values: a list argument is copied in */
                    v->as_struct.field_vals[i] =
                        cval_copy(ctx, iron_comptime_eval_expr(ctx, call->args[i]));
                    if (ctx->had_error) return cval_null(ctx);
                }
                return v;
            }

            if (!sym || sym->sym_kind != IRON_SYM_FUNCTION) {
                comptime_not_callable(ctx, node, func_name);
                return cval_null(ctx);
            }

            Iron_FuncDecl *fn = (Iron_FuncDecl *)sym->decl_node;
            if (!fn || fn->kind != IRON_NODE_FUNC_DECL || !fn->body) {
                emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                           "comptime: function has no body (cannot evaluate)");
                return cval_null(ctx);
            }

            /* Depth + step check — depth check first prevents stack overflow */
            if (ctx->call_depth >= IRON_COMPTIME_MAX_DEPTH) {
                emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, node->span,
                           "comptime evaluation nests calls more than 256 deep");
                return cval_null(ctx);
            }
            ctx->steps++;
            if (ctx->steps > ctx->step_limit) {
                emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, node->span,
                           "comptime evaluation exceeded step limit (1000000 steps)");
                return cval_null(ctx);
            }

            /* Evaluate arguments before pushing frame */
            Iron_ComptimeVal **arg_vals = NULL;
            for (int i = 0; i < call->arg_count; i++) {
                Iron_ComptimeVal *av = iron_comptime_eval_expr(ctx, call->args[i]);
                if (ctx->had_error) return cval_null(ctx);
                arrput(arg_vals, av);
            }

            /* Push call stack entry for error tracing */
            arrput(ctx->call_stack, func_name);
            arrput(ctx->call_spans, node->span);
            ctx->call_depth++;

            /* Push local frame and bind parameters */
            push_frame(ctx);
            for (int i = 0; i < fn->param_count && i < call->arg_count; i++) {
                Iron_Param *p = (Iron_Param *)fn->params[i];
                bind_local(ctx, p->name, arg_vals[i]);
            }
            arrfree(arg_vals);

            /* Reset return state */
            bool outer_had_return = ctx->had_return;
            Iron_ComptimeVal *outer_return_val = ctx->return_val;
            ctx->had_return  = false;
            ctx->return_val  = NULL;

            /* Evaluate function body */
            eval_stmt(ctx, fn->body);

            /* Collect return value */
            Iron_ComptimeVal *result = ctx->return_val
                                       ? ctx->return_val
                                       : cval_null(ctx);

            /* Restore outer return state */
            ctx->had_return  = outer_had_return;
            ctx->return_val  = outer_return_val;

            /* Pop call frame and call stack */
            pop_frame(ctx);
            arrsetlen(ctx->call_stack, ctx->call_depth - 1);
            arrsetlen(ctx->call_spans, ctx->call_depth - 1);
            ctx->call_depth--;

            return result;
        }

        /* ── Array literal ──────────────────────────────────────────────── */

        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_ARRAY);
            v->as_array.count = al->element_count;
            v->as_array.elems = iron_arena_alloc(ctx->arena,
                (size_t)al->element_count * sizeof(Iron_ComptimeVal *),
                _Alignof(Iron_ComptimeVal *));
            if (!v->as_array.elems) { /* HARD-09 REPLACE (comptime.c:iron_comptime_eval_expr ARRAY_LIT elems) */ return NULL; }
            for (int i = 0; i < al->element_count; i++) {
                v->as_array.elems[i] = iron_comptime_eval_expr(ctx,
                                                                 al->elements[i]);
                if (ctx->had_error) return cval_null(ctx);
            }
            return v;
        }

        /* ── Object construction ────────────────────────────────────────── */

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_STRUCT);
            v->as_struct.type_name = ce->type_name;
            v->as_struct.field_count = ce->arg_count;
            v->as_struct.field_names = iron_arena_alloc(ctx->arena,
                (size_t)ce->arg_count * sizeof(const char *),
                _Alignof(const char *));
            if (!v->as_struct.field_names) { /* HARD-09 REPLACE (comptime.c:iron_comptime_eval_expr CONSTRUCT field_names) */ return NULL; }
            v->as_struct.field_vals = iron_arena_alloc(ctx->arena,
                (size_t)ce->arg_count * sizeof(Iron_ComptimeVal *),
                _Alignof(Iron_ComptimeVal *));
            if (!v->as_struct.field_vals) { /* HARD-09 REPLACE (comptime.c:iron_comptime_eval_expr CONSTRUCT field_vals) */ return NULL; }
            /* Look up field names from global scope */
            Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope,
                                                  ce->type_name);
            for (int i = 0; i < ce->arg_count; i++) {
                if (sym && sym->decl_node &&
                    sym->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                    Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
                    if (i < od->field_count) {
                        Iron_Field *f = (Iron_Field *)od->fields[i];
                        v->as_struct.field_names[i] = f->name;
                    } else {
                        v->as_struct.field_names[i] = "(unknown)";
                    }
                } else {
                    v->as_struct.field_names[i] = "(unknown)";
                }
                v->as_struct.field_vals[i] = iron_comptime_eval_expr(ctx,
                                                                        ce->args[i]);
                if (ctx->had_error) return cval_null(ctx);
            }
            return v;
        }

        /* ── Restriction: heap/rc not allowed ────────────────────────────── */

        case IRON_NODE_HEAP: {
            emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                       "comptime: heap allocation is not allowed in comptime context");
            return cval_null(ctx);
        }

        case IRON_NODE_RC: {
            emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                       "comptime: rc allocation is not allowed in comptime context");
            return cval_null(ctx);
        }

        /* ── Nested comptime (valid but redundant) ───────────────────────── */

        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            return iron_comptime_eval_expr(ctx, ce->inner);
        }

        /* AUDIT-02 #8 fix: previously these expression kinds fell through
         * to the silent default; enumerate them so future enum growth trips
         * -Werror=switch-enum instead of silently misclassifying. */
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ix = (Iron_IndexExpr *)node;
            Iron_ComptimeVal *arr = iron_comptime_eval_expr(ctx, ix->object);
            if (ctx->had_error) return cval_null(ctx);
            Iron_ComptimeVal *idx = iron_comptime_eval_expr(ctx, ix->index);
            if (ctx->had_error) return cval_null(ctx);
            if (arr->kind != IRON_CVAL_ARRAY || idx->kind != IRON_CVAL_INT) {
                emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                           "comptime: only arrays can be indexed in comptime context");
                return cval_null(ctx);
            }
            if (idx->as_int < 0 || idx->as_int >= arr->as_array.count) {
                emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                           "comptime: index out of bounds");
                return cval_null(ctx);
            }
            return arr->as_array.elems[idx->as_int];
        }

        case IRON_NODE_METHOD_CALL:
        case IRON_NODE_FIELD_ACCESS:
        case IRON_NODE_SLICE:
        case IRON_NODE_INTERP_STRING:
        case IRON_NODE_LAMBDA:
        case IRON_NODE_IS:
        case IRON_NODE_MATCH: {
            comptime_unavailable_kind(ctx, node);
            return cval_null(ctx);
        }

        /* -Wswitch-enum opt-out: comptime evaluator only handles expression
         * kinds; every other kind is a construct compile-time evaluation does
         * not cover (E0231, manual 8), named where the reader would not
         * guess it. */
        default:
            emit_error_help(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                            node->kind == IRON_NODE_ENUM_CONSTRUCT
                                ? "comptime: enum values are not available at compile time"
                                : "comptime: this expression is not available at compile time",
                            "compile-time evaluation covers literals, arithmetic, list indexing, "
                            "object construction, len / range / fill / read_file and calls to "
                            "top-level functions");
            return cval_null(ctx);
    }
}

/* ── Statement evaluator ─────────────────────────────────────────────────── */

static void eval_stmt(Iron_ComptimeCtx *ctx, Iron_Node *node) {
    if (!node || ctx->had_error || ctx->had_return) return;
    /* Guard against deep recursion (mirrors the check in eval_expr) */
    if (ctx->call_depth >= IRON_COMPTIME_MAX_DEPTH) {
        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, node->span,
                   "comptime evaluation nests calls more than 256 deep");
        return;
    }

    switch ((int)(node->kind)) {

        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            push_frame(ctx);
            eval_stmts(ctx, blk->stmts, blk->stmt_count);
            pop_frame(ctx);
            break;
        }

        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            Iron_ComptimeVal *val = vd->init
                ? iron_comptime_eval_expr(ctx, vd->init)
                : cval_null(ctx);
            if (!ctx->had_error) {
                bind_local(ctx, vd->name, val);
            }
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            Iron_ComptimeVal *val = vd->init
                ? iron_comptime_eval_expr(ctx, vd->init)
                : cval_null(ctx);
            if (!ctx->had_error) {
                bind_local(ctx, vd->name, val);
            }
            break;
        }

        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            if (as->target->kind == IRON_NODE_INDEX) {
                /* xs[i] = v writes the element in place: arrays share their
                 * storage between bindings, as they do at runtime. */
                Iron_IndexExpr *ix = (Iron_IndexExpr *)as->target;
                Iron_ComptimeVal *arr = iron_comptime_eval_expr(ctx, ix->object);
                if (ctx->had_error) break;
                Iron_ComptimeVal *idx = iron_comptime_eval_expr(ctx, ix->index);
                if (ctx->had_error) break;
                if (arr->kind != IRON_CVAL_ARRAY || idx->kind != IRON_CVAL_INT) {
                    emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, as->target->span,
                               "comptime: only array elements can be assigned by index");
                    break;
                }
                if (idx->as_int < 0 || idx->as_int >= arr->as_array.count) {
                    emit_error(ctx, IRON_ERR_COMPTIME_ERROR, as->target->span,
                               "comptime: index out of bounds");
                    break;
                }
                Iron_ComptimeVal *val = iron_comptime_eval_expr(ctx, as->value);
                if (ctx->had_error) break;
                if (as->op != IRON_TOK_ASSIGN) {
                    val = eval_binary_vals(ctx, comptime_compound_base_op(as->op),
                                           arr->as_array.elems[idx->as_int], val, node,
                                           ct_type(as->target), ct_type(as->target));
                    if (ctx->had_error) break;
                }
                arr->as_array.elems[idx->as_int] = val;
                break;
            }
            if (as->target->kind != IRON_NODE_IDENT) {
                /* Field reads are not supported in comptime, so neither are
                 * field writes. Silently skipping the store made the comptime
                 * result diverge from the runtime one. */
                emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, as->target->span,
                           "comptime: assignment to a field is not supported "
                           "in comptime context");
                break;
            }
            Iron_ComptimeVal *val = iron_comptime_eval_expr(ctx, as->value);
            if (ctx->had_error) break;
            if (as->op != IRON_TOK_ASSIGN) {
                /* `x op= v` is `x = x op v`: the operator was ignored, so
                 * `s += 5` behaved as `s = 5`. */
                Iron_ComptimeVal *cur = iron_comptime_eval_expr(ctx, as->target);
                if (ctx->had_error) break;
                val = eval_binary_vals(ctx, comptime_compound_base_op(as->op),
                                       cur, val, node,
                                       ct_type(as->target), ct_type(as->target));
                if (ctx->had_error) break;
            }
            if (as->target->kind == IRON_NODE_IDENT) {
                Iron_Ident *id = (Iron_Ident *)as->target;
                /* Update binding in the innermost frame that has this name */
                for (int i = ctx->frame_depth - 1; i >= 0; i--) {
                    Iron_ComptimeBinding *frame = ctx->local_frames[i];
                    int idx = shgeti(frame, id->name);
                    if (idx >= 0) {
                        frame[idx].value = val;
                        ctx->local_frames[i] = frame;
                        break;
                    }
                }
            }
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            if (rs->value) {
                ctx->return_val = iron_comptime_eval_expr(ctx, rs->value);
            } else {
                ctx->return_val = cval_null(ctx);
            }
            ctx->had_return = true;
            break;
        }

        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            Iron_ComptimeVal *cond = iron_comptime_eval_expr(ctx, is->condition);
            if (ctx->had_error) break;

            bool taken = (cond->kind == IRON_CVAL_BOOL) ? cond->as_bool :
                         (cond->kind == IRON_CVAL_INT)  ? (cond->as_int != 0) :
                         false;
            if (taken) {
                eval_stmt(ctx, is->body);
            } else {
                /* Check elif branches */
                bool elif_taken = false;
                for (int i = 0; i < is->elif_count && !elif_taken; i++) {
                    Iron_ComptimeVal *ec = iron_comptime_eval_expr(ctx,
                                                                    is->elif_conds[i]);
                    if (ctx->had_error) break;
                    bool etaken = (ec->kind == IRON_CVAL_BOOL) ? ec->as_bool :
                                  (ec->kind == IRON_CVAL_INT)  ? (ec->as_int != 0) :
                                  false;
                    if (etaken) {
                        eval_stmt(ctx, is->elif_bodies[i]);
                        elif_taken = true;
                    }
                }
                if (!elif_taken && is->else_body) {
                    eval_stmt(ctx, is->else_body);
                }
            }
            break;
        }

        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            while (!ctx->had_error && !ctx->had_return) {
                Iron_ComptimeVal *cond = iron_comptime_eval_expr(ctx,
                                                                   ws->condition);
                if (ctx->had_error) break;
                bool keep_going = (cond->kind == IRON_CVAL_BOOL) ? cond->as_bool :
                                  (cond->kind == IRON_CVAL_INT)  ? (cond->as_int != 0) :
                                  false;
                if (!keep_going) break;

                ctx->steps++;
                if (ctx->steps > ctx->step_limit) {
                    emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, ws->span,
                               "comptime evaluation exceeded step limit (1000000 steps)");
                    break;
                }
                eval_stmt(ctx, ws->body);
            }
            break;
        }

        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            Iron_ComptimeVal *iterable = iron_comptime_eval_expr(ctx, fs->iterable);
            if (ctx->had_error) break;

            if (iterable->kind == IRON_CVAL_INT) {
                /* Range: iterate 0..n */
                int64_t n = iterable->as_int;
                for (int64_t i = 0; i < n && !ctx->had_error && !ctx->had_return; i++) {
                    ctx->steps++;
                    if (ctx->steps > ctx->step_limit) {
                        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, fs->span,
                                   "comptime evaluation exceeded step limit (1000000 steps)");
                        break;
                    }
                    push_frame(ctx);
                    bind_local(ctx, fs->var_name, cval_int(ctx, i));
                    eval_stmt(ctx, fs->body);
                    pop_frame(ctx);
                }
            } else if (iterable->kind == IRON_CVAL_ARRAY) {
                for (int i = 0; i < iterable->as_array.count && !ctx->had_error && !ctx->had_return; i++) {
                    ctx->steps++;
                    if (ctx->steps > ctx->step_limit) {
                        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, fs->span,
                                   "comptime evaluation exceeded step limit (1000000 steps)");
                        break;
                    }
                    push_frame(ctx);
                    bind_local(ctx, fs->var_name, iterable->as_array.elems[i]);
                    eval_stmt(ctx, fs->body);
                    pop_frame(ctx);
                }
            }
            break;
        }

        /* -Wswitch-enum opt-out: comptime eval_stmt handles statement kinds
         * explicitly; remaining kinds (mostly expressions used as statements)
         * are evaluated via iron_comptime_eval_expr for their side effects. */
        default:
            /* Treat as expression statement if it can be evaluated */
            iron_comptime_eval_expr(ctx, node);
            break;
    }
}

static void eval_stmts(Iron_ComptimeCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count && !ctx->had_error && !ctx->had_return; i++) {
        eval_stmt(ctx, stmts[i]);
    }
}

/* ── Value to AST conversion ─────────────────────────────────────────────── */

Iron_Node *iron_comptime_val_to_ast(Iron_ComptimeVal *val, Iron_Arena *arena,
                                     Iron_Span span,
                                     struct Iron_Type *resolved_type) {
    if (!val) return NULL;

    switch (val->kind) {

        case IRON_CVAL_INT: {
            Iron_IntLit *lit = iron_arena_alloc(arena, sizeof(Iron_IntLit),
                                                 _Alignof(Iron_IntLit));
            if (!lit) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast IntLit) */ return NULL; }
            lit->span          = span;
            lit->kind          = IRON_NODE_INT_LIT;
            lit->resolved_type = resolved_type;
            /* Convert int64 to string */
            char buf[32];
            snprintf(buf, sizeof(buf), "%" PRId64, val->as_int);
            lit->value = iron_arena_strdup(arena, buf, strlen(buf));
            if (!lit->value) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast IntLit value) */ return NULL; }
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_FLOAT: {
            Iron_FloatLit *lit = iron_arena_alloc(arena, sizeof(Iron_FloatLit),
                                                   _Alignof(Iron_FloatLit));
            if (!lit) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast FloatLit) */ return NULL; }
            lit->span          = span;
            lit->kind          = IRON_NODE_FLOAT_LIT;
            lit->resolved_type = resolved_type;
            char buf[64];
            double fv = val->as_float;
            /* NaN and the infinities in the spelling strtod reads back
             * (the lowering parses the literal; the emitter writes them as
             * 0.0/0.0 and 1.0/0.0). */
            if (fv != fv) snprintf(buf, sizeof(buf), "nan");
            else if (fv > 1.7976931348623157e308) snprintf(buf, sizeof(buf), "inf");
            else if (fv < -1.7976931348623157e308) snprintf(buf, sizeof(buf), "-inf");
            else snprintf(buf, sizeof(buf), "%.17g", fv);
            lit->value = iron_arena_strdup(arena, buf, strlen(buf));
            if (!lit->value) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast FloatLit value) */ return NULL; }
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_BOOL: {
            Iron_BoolLit *lit = iron_arena_alloc(arena, sizeof(Iron_BoolLit),
                                                  _Alignof(Iron_BoolLit));
            if (!lit) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast BoolLit) */ return NULL; }
            lit->span          = span;
            lit->kind          = IRON_NODE_BOOL_LIT;
            lit->resolved_type = resolved_type;
            lit->value         = val->as_bool;
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_STRING: {
            Iron_StringLit *lit = iron_arena_alloc(arena, sizeof(Iron_StringLit),
                                                    _Alignof(Iron_StringLit));
            if (!lit) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast StringLit) */ return NULL; }
            lit->span          = span;
            lit->kind          = IRON_NODE_STRING_LIT;
            lit->resolved_type = resolved_type;
            lit->value = iron_arena_strdup(arena, val->as_string.data,
                                            val->as_string.len);
            lit->len   = val->as_string.len;
            if (!lit->value) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast StringLit value) */ return NULL; }
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_ARRAY: {
            Iron_ArrayLit *al = iron_arena_alloc(arena, sizeof(Iron_ArrayLit),
                                                  _Alignof(Iron_ArrayLit));
            if (!al) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast ArrayLit) */ return NULL; }
            al->span          = span;
            al->kind          = IRON_NODE_ARRAY_LIT;
            al->resolved_type = resolved_type;
            al->type_ann      = NULL;
            al->size          = NULL;
            al->element_count = val->as_array.count;
            /* (An empty list, `fill(0, v)`, has no elements to allocate: the
             * NULL from a zero-sized allocation rejected it as "cannot be
             * kept as a constant".) */
            al->elements = NULL;
            if (val->as_array.count > 0) {
                al->elements = iron_arena_arr_new(arena,
                    (size_t)val->as_array.count, sizeof(Iron_Node *));
                if (!al->elements) { /* HARD-09 REPLACE (comptime.c:iron_comptime_val_to_ast ArrayLit elements) */ return NULL; }
            }
            /* Elements carry the array's element type: with NULL the
             * lowered literal had void elements and emitted invalid C. */
            Iron_Type *elem_type =
                (resolved_type && resolved_type->kind == IRON_TYPE_ARRAY)
                    ? resolved_type->array.elem : NULL;
            for (int i = 0; i < val->as_array.count; i++) {
                al->elements[i] = iron_comptime_val_to_ast(
                    val->as_array.elems[i], arena, span, elem_type);
            }
            return (Iron_Node *)al;
        }

        /* An object built at compile time: the positional construction of
         * its field values (objects without an init only, see CALL). */
        case IRON_CVAL_STRUCT: {
            if (!resolved_type || resolved_type->kind != IRON_TYPE_OBJECT ||
                !resolved_type->object.decl)
                return NULL;
            Iron_ObjectDecl *od = resolved_type->object.decl;
            if (od->field_count != val->as_struct.field_count) return NULL;
            Iron_ConstructExpr *ce = iron_arena_alloc(arena, sizeof(Iron_ConstructExpr),
                                                      _Alignof(Iron_ConstructExpr));
            if (!ce) return NULL;
            memset(ce, 0, sizeof(*ce));
            ce->span          = span;
            ce->kind          = IRON_NODE_CONSTRUCT;
            ce->resolved_type = resolved_type;
            ce->type_name     = od->name;
            ce->arg_count     = od->field_count;
            ce->args = iron_arena_arr_new(arena, (size_t)(od->field_count ? od->field_count : 1),
                                          sizeof(Iron_Node *));
            if (!ce->args) return NULL;
            for (int i = 0; i < od->field_count; i++) {
                Iron_Field *f = (Iron_Field *)od->fields[i];
                ce->args[i] = iron_comptime_val_to_ast(val->as_struct.field_vals[i], arena,
                                                       span, f ? f->resolved_type : NULL);
                if (!ce->args[i]) return NULL;
            }
            return (Iron_Node *)ce;
        }
        case IRON_CVAL_NULL:
            return NULL;
    }
    return NULL; /* unreachable — switch is exhaustive */
}

/* ── AST walker for replacing IRON_NODE_COMPTIME nodes ───────────────────── */

#define IRON_COMPTIME_STACK (64u * 1024u * 1024u)   /* reserved, not committed */


/* Context for the replacement walk */
typedef struct {
    Iron_ComptimeCtx *eval_ctx;
    Iron_Arena        *arena;
} ReplaceCtx;

/* Replace COMPTIME nodes in an array of node pointers. */
static void replace_in_node_array(Iron_Node **nodes, int count,
                                   ReplaceCtx *rctx);
static void replace_in_node(Iron_Node **slot, ReplaceCtx *rctx);

/* FIX-03 / AUDIT-04 §15: SAFETY — `replace_in_node` overwrites the AST slot
 * pointer with a fresh arena-allocated literal node, leaving the PREVIOUS
 * node (the comptime expression that was just evaluated) unreachable but
 * not reclaimed. This is a known bump-allocator characteristic: the arena
 * cannot free individual allocations in place, so every replaced node
 * represents a small arena-waste "puddle". The waste is bounded by the
 * total size of all comptime expressions in the source (typically a few
 * KB per compile) and is reclaimed wholesale when the compilation arena
 * is freed at iron_arena_free. Audit row 15 calls this "benign waste";
 * no code change needed. */
static void replace_in_node(Iron_Node **slot, ReplaceCtx *rctx) {
    if (!slot || !*slot) return;
    Iron_Node *node = *slot;

    if (node->kind == IRON_NODE_COMPTIME) {
        Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
        struct Iron_Type *orig_type = ce->resolved_type;
        Iron_Span orig_span = ce->span;

        /* Reset error state for each comptime expression */
        rctx->eval_ctx->had_error  = false;
        rctx->eval_ctx->had_return = false;
        rctx->eval_ctx->return_val = NULL;
        rctx->eval_ctx->steps      = 0;

        /* Reset call stack */
        arrsetlen(rctx->eval_ctx->call_stack, 0);
        arrsetlen(rctx->eval_ctx->call_spans, 0);
        rctx->eval_ctx->call_depth = 0;

        /* Reset local frames */
        while (rctx->eval_ctx->frame_depth > 0) {
            pop_frame(rctx->eval_ctx);
        }

        Iron_ComptimeVal *val = iron_comptime_eval_expr(rctx->eval_ctx,
                                                         ce->inner);
        if (!rctx->eval_ctx->had_error && val) {
            /* WR-10: the cache write call previously lived here but was
             * paired with a cache read that always discarded its result.
             * Both have been deleted; comptime evaluation is always a
             * fresh in-memory walk. */
            Iron_Node *replacement = iron_comptime_val_to_ast(val, rctx->arena,
                                                               orig_span,
                                                               orig_type);
            if (replacement) {
                *slot = replacement;
                return;
            }
            emit_error_help(rctx->eval_ctx, IRON_ERR_COMPTIME_RESTRICTION, orig_span,
                            "comptime: this result cannot be kept as a constant",
                            "compile-time evaluation produces numbers, booleans, strings, "
                            "lists and objects without an init");
        }
        /* On error: leave node as-is (error already emitted) */
        return;
    }

    /* Recurse into children */
    switch ((int)(node->kind)) {

        case IRON_NODE_PROGRAM: {
            Iron_Program *p = (Iron_Program *)node;
            replace_in_node_array(p->decls, p->decl_count, rctx);
            break;
        }
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *f = (Iron_FuncDecl *)node;
            replace_in_node(&f->body, rctx);
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *m = (Iron_MethodDecl *)node;
            replace_in_node(&m->body, rctx);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            replace_in_node_array(b->stmts, b->stmt_count, rctx);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            replace_in_node(&vd->init, rctx);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            replace_in_node(&vd->init, rctx);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            replace_in_node(&as->value, rctx);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            replace_in_node(&rs->value, rctx);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            replace_in_node(&is->condition, rctx);
            replace_in_node(&is->body, rctx);
            replace_in_node_array(is->elif_conds, is->elif_count, rctx);
            replace_in_node_array(is->elif_bodies, is->elif_count, rctx);
            replace_in_node(&is->else_body, rctx);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            replace_in_node(&ws->condition, rctx);
            replace_in_node(&ws->body, rctx);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            replace_in_node(&fs->iterable, rctx);
            replace_in_node(&fs->body, rctx);
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;
            replace_in_node(&bin->left, rctx);
            replace_in_node(&bin->right, rctx);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
            replace_in_node(&un->operand, rctx);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)node;
            replace_in_node(&c->callee, rctx);
            replace_in_node_array(c->args, c->arg_count, rctx);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            replace_in_node(&mc->object, rctx);
            replace_in_node_array(mc->args, mc->arg_count, rctx);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            replace_in_node(&fa->object, rctx);
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ie = (Iron_IndexExpr *)node;
            replace_in_node(&ie->object, rctx);
            replace_in_node(&ie->index, rctx);
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            replace_in_node_array(al->elements, al->element_count, rctx);
            break;
        }
        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            replace_in_node(&he->inner, rctx);
            break;
        }
        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            replace_in_node(&re->inner, rctx);
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            replace_in_node_array(ce->args, ce->arg_count, rctx);
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            replace_in_node(&ds->expr, rctx);
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            replace_in_node(&fs->expr, rctx);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            replace_in_node(&ss->body, rctx);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            replace_in_node(&ms->subject, rctx);
            replace_in_node_array(ms->cases, ms->case_count, rctx);
            replace_in_node(&ms->else_body, rctx);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            replace_in_node(&mc->body, rctx);
            break;
        }
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            replace_in_node(&ae->handle, rctx);
            break;
        }
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            replace_in_node(&le->body, rctx);
            break;
        }
        /* -Wswitch-enum opt-out: COMPTIME-node replacement walker only recurses
         * into expression-bearing kinds; leaf nodes (literals, IDENT, etc.)
         * have no child slot that can contain IRON_NODE_COMPTIME. */
        default:
            /* Leaf nodes: INT_LIT, FLOAT_LIT, BOOL_LIT, STRING_LIT, IDENT, etc. */
            break;
    }
}

static void replace_in_node_array(Iron_Node **nodes, int count,
                                   ReplaceCtx *rctx) {
    for (int i = 0; i < count; i++) {
        replace_in_node(&nodes[i], rctx);
    }
}

/* ── Main entry point ────────────────────────────────────────────────────── */

typedef struct {
    Iron_Node  **slot;
    ReplaceCtx  *rctx;
} ComptimeRun;

static void *comptime_thread(void *p) {
    ComptimeRun *r = (ComptimeRun *)p;
    replace_in_node(r->slot, r->rctx);
    return NULL;
}

typedef struct {
    Iron_ComptimeCtx *ctx;
    Iron_Node        *expr;
    Iron_ComptimeVal *val;
} ComptimeConstRun;

static void *comptime_const_thread(void *p) {
    ComptimeConstRun *r = (ComptimeConstRun *)p;
    r->val = iron_comptime_eval_expr(r->ctx, r->expr);
    return NULL;
}

bool iron_comptime_eval_int_const(Iron_Program *program, Iron_Scope *global_scope,
                                  Iron_Arena *arena, Iron_Node *expr, int64_t *out) {
    if (!expr || !out) return false;
    Iron_DiagList scratch = iron_diaglist_create();
    Iron_ComptimeCtx c;
    memset(&c, 0, sizeof(c));
    c.arena        = arena;
    c.diags        = &scratch;
    c.global_scope = global_scope;
    c.step_limit   = 1000000;
    c.mode         = IRON_ANALYSIS_MODE_LSP;   /* no file system access */
    c.program      = program;
    ComptimeConstRun run = { &c, expr, NULL };
    /* The same large stack the comptime pass uses for nested calls. */
    pthread_attr_t attr;
    pthread_t th;
    bool threaded = false;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, (size_t)IRON_COMPTIME_STACK) == 0 &&
            pthread_create(&th, &attr, comptime_const_thread, &run) == 0) {
            pthread_join(th, NULL);
            threaded = true;
        }
        pthread_attr_destroy(&attr);
    }
    if (!threaded) run.val = iron_comptime_eval_expr(&c, expr);
    bool ok = !c.had_error && run.val && run.val->kind == IRON_CVAL_INT;
    if (ok) *out = run.val->as_int;
    while (c.frame_depth > 0) pop_frame(&c);
    arrfree(c.call_stack);
    arrfree(c.call_spans);
    arrfree(c.local_frames);
    iron_diaglist_free(&scratch);
    return ok;
}

void iron_comptime_apply(Iron_Program *program, Iron_Scope *global_scope,
                          Iron_Arena *arena, Iron_DiagList *diags,
                          const char *source_file_dir, bool force_comptime,
                          const char *source_text, size_t source_len,
                          IronAnalysisMode mode) {
    Iron_ComptimeCtx eval_ctx;
    eval_ctx.arena           = arena;
    eval_ctx.diags           = diags;
    eval_ctx.global_scope    = global_scope;
    eval_ctx.steps           = 0;
    eval_ctx.step_limit      = 1000000;
    eval_ctx.call_stack      = NULL;
    eval_ctx.call_spans      = NULL;
    eval_ctx.call_depth      = 0;
    eval_ctx.source_file_dir = source_file_dir;
    eval_ctx.source_text     = source_text;
    eval_ctx.source_len      = source_len;
    eval_ctx.had_error       = false;
    eval_ctx.local_frames    = NULL;
    eval_ctx.frame_depth     = 0;
    eval_ctx.had_return      = false;
    eval_ctx.return_val      = NULL;
    eval_ctx.mode            = mode;  /* HARD-02: FS gating */
    eval_ctx.program         = program;

    /* WR-10: the comptime source-hash cache was deleted — read-side was
     * dead code (cached value discarded at the only call site) and
     * write-side had no live reader. Compilation always runs a fresh
     * in-memory comptime walk. force_comptime is kept as part of the
     * public API signature but no longer drives cache bypass. */
    (void)source_text;
    (void)source_len;
    (void)force_comptime;

    ReplaceCtx rctx;
    memset(&rctx, 0, sizeof(rctx));
    rctx.eval_ctx = &eval_ctx;
    rctx.arena    = arena;

    /* The evaluator recurses once per nested Iron call; it runs on a
     * thread whose stack holds the 256 nested calls it allows in every
     * build (a sanitizer build's frames are many times larger: the main
     * thread's stack overflowed at 200 calls, and the language server's
     * worker threads have smaller stacks still). */
    ComptimeRun run = { (Iron_Node **)&program, &rctx };
    pthread_attr_t attr;
    pthread_t th;
    bool threaded = false;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, (size_t)IRON_COMPTIME_STACK) == 0 &&
            pthread_create(&th, &attr, comptime_thread, &run) == 0) {
            pthread_join(th, NULL);
            threaded = true;
        }
        pthread_attr_destroy(&attr);
    }
    if (!threaded) replace_in_node((Iron_Node **)&program, &rctx);

    /* Clean up stb_ds arrays */
    arrfree(eval_ctx.call_stack);
    arrfree(eval_ctx.call_spans);
    arrfree(eval_ctx.local_frames);
}
