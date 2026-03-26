#ifndef IRON_COMPTIME_H
#define IRON_COMPTIME_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Comptime value kinds ─────────────────────────────────────────────────── */

typedef enum {
    IRON_CVAL_INT,
    IRON_CVAL_FLOAT,
    IRON_CVAL_BOOL,
    IRON_CVAL_STRING,
    IRON_CVAL_ARRAY,
    IRON_CVAL_STRUCT,
    IRON_CVAL_NULL
} Iron_ComptimeValKind;

/* ── Comptime value (tagged union) ───────────────────────────────────────── */

typedef struct Iron_ComptimeVal {
    Iron_ComptimeValKind kind;
    union {
        int64_t  as_int;
        double   as_float;
        bool     as_bool;
        struct {
            const char *data;
            size_t      len;
        } as_string;
        struct {
            struct Iron_ComptimeVal **elems;
            int                      count;
        } as_array;
        struct {
            const char             **field_names;
            struct Iron_ComptimeVal **field_vals;
            int                      field_count;
            const char              *type_name;
        } as_struct;
    };
} Iron_ComptimeVal;

/* ── Local variable binding for comptime evaluation ──────────────────────── */

typedef struct {
    char              *key;    /* stb_ds string key */
    Iron_ComptimeVal  *value;
} Iron_ComptimeBinding;

/* ── Comptime evaluation context ─────────────────────────────────────────── */

typedef struct {
    Iron_Arena          *arena;
    Iron_DiagList       *diags;
    Iron_Scope          *global_scope;
    int                  steps;
    int                  step_limit;        /* 1,000,000 default */
    const char         **call_stack;        /* stb_ds array of func names */
    Iron_Span           *call_spans;        /* stb_ds array of call site spans */
    int                  call_depth;
    const char          *source_file_dir;   /* dir of .iron file for read_file resolution */
    const char          *source_text;       /* full source text for cache key computation */
    size_t               source_len;        /* length of source_text */
    bool                 had_error;
    /* Local variable bindings stack: array of stb_ds hashmaps */
    Iron_ComptimeBinding **local_frames;    /* stb_ds array of frames */
    int                   frame_depth;
    /* Return value signaling */
    bool                  had_return;
    Iron_ComptimeVal     *return_val;
} Iron_ComptimeCtx;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Evaluate a single expression node in comptime context.
 * Returns NULL on error (ctx->had_error will be true). */
Iron_ComptimeVal *iron_comptime_eval_expr(Iron_ComptimeCtx *ctx, Iron_Node *node);

/* Convert a comptime value back to a literal AST node suitable for codegen.
 * The resolved_type is copied from the original COMPTIME node so codegen
 * type mapping works correctly. */
Iron_Node *iron_comptime_val_to_ast(Iron_ComptimeVal *val, Iron_Arena *arena,
                                     Iron_Span span, struct Iron_Type *resolved_type);

/* Walk the program AST and replace all IRON_NODE_COMPTIME nodes with their
 * evaluated literal equivalents.  Called from iron_analyze() after typecheck,
 * before returning to the caller (and before codegen runs).
 *
 * force_comptime:  if true, bypass cache and re-evaluate all comptime exprs.
 * source_file_dir: used for read_file() calls (NULL is fine for basic comptime).
 * source_text/len: full source text used as cache key (NULL skips caching). */
void iron_comptime_apply(Iron_Program *program, Iron_Scope *global_scope,
                          Iron_Arena *arena, Iron_DiagList *diags,
                          const char *source_file_dir, bool force_comptime,
                          const char *source_text, size_t source_len);

#endif /* IRON_COMPTIME_H */
