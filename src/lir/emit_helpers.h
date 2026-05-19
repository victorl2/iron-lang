/* emit_helpers.h -- Shared helpers and EmitCtx for the Iron C emitter.
 *
 * This header defines the central EmitCtx struct and declares utility
 * functions shared across all emitter sub-modules (emit_c.c, and future
 * emit_structs.c, emit_split.c, emit_fusion.c).
 *
 * Every sub-module includes this header for context and shared utilities.
 */

#ifndef IRON_EMIT_HELPERS_H
#define IRON_EMIT_HELPERS_H

#include "lir/lir.h"
#include "lir/lir_optimize.h"
#include "lir/layout_analysis.h"
#include "lir/value_range.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "analyzer/iface_collect.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"

/* ── Loop fusion chain types (Phase 49) ──────────────────────────────────── */

typedef struct {
    IronLIR_ValueId call_vid;      /* ValueId of the CALL instruction */
    const char *method;            /* "map", "filter", "reduce", "forEach", "sum" */
    IronLIR_ValueId self_arg;      /* ValueId of the collection input */
    IronLIR_ValueId *lambda_args;  /* stb_ds array of closure argument ValueIds */
    int lambda_arg_count;
    IronLIR_ValueId init_arg;      /* reduce init arg (IRON_LIR_VALUE_INVALID if N/A) */
} FusionChainNode;

typedef struct {
    FusionChainNode *nodes;        /* stb_ds array, in execution order */
    int node_count;
    IronLIR_ValueId source;        /* original collection input to the chain */
    bool is_split;                 /* true if source is a split collection */
    const char *sp_iface;          /* interface name if split, NULL otherwise */
} FusionChain;

/* ── EmitCtx -- central emitter context ──────────────────────────────────── */

typedef struct {
    /* Core context: arena, diagnostics, IR module */
    Iron_Arena    *arena;
    Iron_DiagList *diags;
    IronLIR_Module *module;

    /* Output sections -- emitted in order to produce final C source */
    Iron_StrBuf    includes;
    Iron_StrBuf    forward_decls;
    Iron_StrBuf    struct_bodies;
    Iron_StrBuf    enum_defs;
    Iron_StrBuf    global_consts;
    Iron_StrBuf    prototypes;
    Iron_StrBuf    lifted_funcs;
    Iron_StrBuf    implementations;
    Iron_StrBuf    main_wrapper;

    /* General emission state */
    char        **emitted_optionals;             /* stb_ds string array */
    char        **emitted_tuples;                /* Phase 59 01d: stb_ds string array of tuple mangled names */
    /* Phase 23 VEC-01: per-(T, N) Iron_BVec_T_N typedef dedup.
     * Parallel to emitted_tuples; same arrput/arrlen/strcmp shape. */
    char        **emitted_bvecs;
    /* Phase 24 DROP-01/06 (Plan 24-02): per-type drop/copy synthesis dedup.
     * Parallel to emitted_bvecs; same arrput/arrlen/strcmp shape. */
    char        **emitted_drops;
    char        **emitted_copies;

    /* Phase 24 DROP-05 (Plan 24-03): partial-init cleanup instrumentation.
     * in_init_method: true when currently emitting an init method body; set
     *   by emit_func_body when the function name ends with "_init".
     * init_cleanup_counter: monotonic counter for unique _iron_cleanup_N
     *   entry names per function; reset to 0 at each function entry. */
    bool in_init_method;
    int  init_cleanup_counter;

    /* Phase 24 DROP-04 (Plan 24-03): drop-method panic trap.
     * in_drop_method: true when currently emitting a user drop body (LIR
     *   function whose name ends with "_drop"). The function prologue sets
     *   iron_in_destructor=true and the RETURN handler clears it, so that
     *   any iron_panic_* call inside the drop body re-routes to
     *   iron_panic_destructor_aborted. The LIR drop name is the original
     *   lowercased type name (e.g., "bomb_drop" for "Bomb"). */
    bool in_drop_method;
    /* Pointer to the type-name portion of fn->name for drop methods
     * (the part before "_drop"), for use in the epilogue emit. */
    const char *drop_method_type_name;
    struct { char *key; bool value; } *mono_registry; /* stb_ds string map */
    int           next_type_tag;                 /* starts at 1 */
    int           indent;

    /* Optimization info from iron_lir_optimize() -- carries array_param_modes,
     * revoked_fill_ids, and per-function tracking maps (stack_array_ids,
     * heap_array_ids, escaped_heap_ids) that are reset per function. */
    IronLIR_OptimizeInfo *opt_info;

    /* Read-only parameter alias tracking: maps alloca ValueId -> param ValueId.
     * For parameters that are never modified (only read), we skip the
     * alloca+store+load chain and reference the parameter value directly. */
    struct { IronLIR_ValueId key; IronLIR_ValueId value; } *param_alias_ids;  /* stb_ds hashmap */

    /* Expression inlining: per-function maps built in emit_func_body pre-scan.
     * inline_eligible: ValueId -> true (values to skip and reconstruct at use site).
     * value_block: ValueId -> BlockId (for block-boundary enforcement).
     * current_block_id: set before each emit_instr call. */
    IronLIR_InlineEligEntry *inline_eligible;  /* per-function inline eligibility map */
    IronLIR_ValueBlockEntry *value_block;      /* per-function value->block map */
    IronLIR_BlockId          current_block_id; /* set before each emit_instr call */

    /* Backward-referenced values hoisted to function entry (type _vN;).
     * At the definition site, emit assignment without type prefix. */
    struct { IronLIR_ValueId key; bool value; } *phi_hoisted;

    /* Per-lifted-function capture alias map: maps alloca ValueId -> capture index.
     * Built at the start of emit_func_body for functions with capture_count > 0.
     * Used to redirect ALLOCA/LOAD/STORE for captured variables to env field accesses. */
    struct { IronLIR_ValueId key; int value; } *capture_alias_map;
    /* Current function's capture metadata (pointer into fn->capture_metadata) */
    Iron_CaptureEntry *current_captures;
    int                current_capture_count;

    /* Per-function map of ALLOCA ids whose type is a recursive ADT enum
     * with at least one boxed payload field.  Populated in the pre-scan;
     * consulted at RETURN sites to emit _free() calls for non-returned locals. */
    struct { IronLIR_ValueId key; Iron_Type *value; } *adt_boxed_allocas;

    /* Interface implementor registry for tagged union dispatch */
    Iron_IfaceRegistry *iface_reg;

    /* Split collection tracking -- maps ValueId to interface name.
     * When a ValueId is in this map, it's a split collection (Iron_SplitList_<Iface>)
     * instead of a standard array (Iron_List_<Iface>). */
    struct { IronLIR_ValueId key; const char *value; } *split_collection_ids;

    /* Dead field elimination -- field access analysis results (Phase 48) */
    LayoutAnalysis layout;

    /* Value range compression (Phase 50) */
    ValueRangeAnalysis value_range;
    bool report_compression;  /* --report-compression: show narrowed fields */

    /* Split loop direct-access context (Phase 48) */
    struct {
        IronLIR_ValueId get_index_vid;   /* loop var vid from GET_INDEX */
        IronLIR_ValueId iterable_vid;    /* the split collection vid */
        const char *lower_name;          /* lowercase impl type for array field */
        bool is_reduced;                 /* true = using reduced storage struct */
        bool is_soa;                     /* Phase 48-02: true = SoA layout for this type */
    } split_loop_ctx;
    bool in_split_loop;

    /* Map of type names that use reduced storage (type_name -> true) (Phase 48) */
    struct { char *key; bool value; } *reduced_storage_types;

    /* Map of "iface_mangled:type_name" -> true for SoA types (Phase 48-02) */
    struct { char *key; bool value; } *soa_types;

    /* Map of "iface:type" -> true for variants stored via pointer indirection (Phase 48-03) */
    struct { char *key; bool value; } *indirect_variants;

    /* Map of collection ValueId -> layout override (1=soa, 2=aos) (Phase 48-03) */
    struct { IronLIR_ValueId key; int value; } *layout_overrides;

    /* Map of collection ValueId -> true for unordered collections (Phase 48-03) */
    struct { IronLIR_ValueId key; bool value; } *unordered_collections;

    /* Fusion chain detection (Phase 49) */
    bool warn_fusion_break;  /* --warn-fusion-break: emit diagnostic at chain break points */
    FusionChain *fusion_chains;        /* stb_ds array of detected chains */
    struct { IronLIR_ValueId key; int value; } *fusion_chain_member;
        /* maps call_vid -> chain_index; positive = chain idx */
    struct { IronLIR_ValueId key; int value; } *fusion_chain_position;
        /* maps call_vid -> position within its chain (0 = first, N-1 = terminal) */

    /* Monomorphic collection tracking (Phase 49) */
    struct { IronLIR_ValueId key; const char *value; } *monomorphic_collections;
        /* maps collection ValueId -> sole concrete type name (e.g., "Circle") */
        /* only populated when a split collection has exactly one type pushed */

    /* Specialization registry (Phase 49) */
    struct { char *key; const char *value; } *specialization_registry;
        /* maps "func_name:concrete_type" -> emitted C function name */
        /* prevents duplicate function body emission for same specialization */
} EmitCtx;

/* ── Name mangling ───────────────────────────────────────────────────────── */

const char *emit_mangle_name(const char *name, Iron_Arena *arena);
const char *emit_object_type_name(const char *name, EmitCtx *ctx);
const char *emit_mangle_func_name(const char *name, Iron_Arena *arena);
const char *emit_resolve_func_c_name(EmitCtx *ctx, const char *ir_name);
const char *emit_sanitize_label(const char *label, Iron_Arena *arena);

/* ── Type mapping ────────────────────────────────────────────────────────── */

const char *emit_type_to_c(const Iron_Type *t, EmitCtx *ctx);
const char *emit_annotation_to_c(const char *name, EmitCtx *ctx);
const char *emit_optional_struct_name(const Iron_Type *inner, EmitCtx *ctx);
void emit_ensure_optional(EmitCtx *ctx, const Iron_Type *inner);

/* Phase 59 01d: synthesise a C typedef for a tuple type on demand.
 * Dedupes via ctx->emitted_tuples. Nested tuples recurse so inner
 * typedefs land first. No-op for non-tuple input. */
void emit_ensure_tuple(EmitCtx *ctx, const Iron_Type *tuple_ty);

/* Phase 23 VEC-01: synthesize a C typedef for a bounded vector type on demand.
 * Emits `typedef struct { uint32_t len; T data[N]; } Iron_BVec_T_N;` into
 * ctx->struct_bodies.  Dedupes via ctx->emitted_bvecs (same arrput/strcmp shape
 * as emitted_tuples).  Recurses for nested-bvec elements so inner typedefs land
 * first (Pitfall 3 mitigation).  No-op for non-bounded or non-array input. */
void emit_ensure_bvec(EmitCtx *ctx, const Iron_Type *bvec_ty);

/* Phase 24 DROP-01 (Plan 24-02): synthesize a static destructor function for an
 * object type. Emits `static void <TypeName>_drop(<TypeName> *self) { ... }`
 * into ctx->lifted_funcs (Pitfall 3: NOT struct_bodies). Dedupes via
 * ctx->emitted_drops. Recurses for field types that have drop blocks (so inner
 * destructors land before the outer one — forward-reference safe).
 * The user drop body is lowered inline; field destructors run in REVERSE
 * declaration order (Pitfall 6 + DROP-02). */
void emit_ensure_drop(EmitCtx *ctx, const char *obj_c_name,
                      struct Iron_ObjectDecl *od);

/* Phase 24 DROP-01 (Plan 24-02): returns true if the object type od has a
 * compiled drop method in the LIR module. Methods are LIR top-level functions
 * (Plan 86 layout), NOT stored on Iron_ObjectDecl.
 * Used by emit_c.c and emit_helpers.c to gate drop synthesis. */
bool od_has_drop_lir(EmitCtx *ctx, struct Iron_ObjectDecl *od);

/* Phase 24 DROP-06 (Plan 24-02): synthesize a shallow copy function for an
 * object type. Emits `static void <TypeName>_copy(<TypeName> *dest, const
 * <TypeName> *src) { ... }` into ctx->lifted_funcs. Dedupes via
 * ctx->emitted_copies. No-op when od->is_nocopy (Pitfall 5).
 * Per-field copy hooks call <FieldType>_copy for fields whose
 * Iron_Type.has_user_copy_transitive is true (cached by typecheck). */
void emit_ensure_copy(EmitCtx *ctx, const char *obj_c_name,
                      struct Iron_ObjectDecl *od);

/* ── Emit utilities ──────────────────────────────────────────────────────── */

void emit_indent(Iron_StrBuf *sb, int level);
void emit_val(Iron_StrBuf *sb, IronLIR_ValueId id);

/* ── Value helpers ───────────────────────────────────────────────────────── */

bool emit_type_is_pointer(const Iron_Type *t);
bool emit_val_is_heap_ptr(IronLIR_Func *fn, IronLIR_ValueId vid);
bool emit_val_is_heap_fat_ptr(IronLIR_Func *fn, IronLIR_ValueId vid);
/* Phase 21: Returns true when the value is ANY Iron_FatPtr at runtime:
 * IRON_LIR_HEAP_ALLOC (heap binding) or IRON_LIR_ADDR_OF (pointer to heap/stack).
 * Used at field-access sites to select the ((T *)_vN.addr)->field form. */
bool emit_val_is_any_fat_ptr(IronLIR_Func *fn, IronLIR_ValueId vid);
/* Phase 21: Return the C pointee-type string for any Iron_FatPtr value.
 * For HEAP_ALLOC: returns emit_type_to_c(instr->type, ctx).
 * For ADDR_OF targeting HEAP_ALLOC: returns the heap alloc's C type.
 * Returns NULL if not a fat ptr. */
const char *emit_fat_ptr_pointee_type_c(IronLIR_Func *fn, IronLIR_ValueId vid, EmitCtx *ctx);
bool emit_val_is_type_ref(IronLIR_Func *fn, IronLIR_ValueId vid);
Iron_Type *emit_get_value_type(IronLIR_Func *fn, IronLIR_ValueId vid);
IronLIR_Func *emit_find_ir_func(EmitCtx *ctx, const char *ir_name);
ArrayParamMode emit_get_array_param_mode(EmitCtx *ctx, const char *func_name, int param_index);
const char *emit_make_block_label(IronLIR_BlockId id, const char *raw_label, Iron_Arena *arena);
const char *emit_resolve_label(IronLIR_Func *fn, IronLIR_BlockId id, Iron_Arena *arena);

/* ── Cleanup ─────────────────────────────────────────────────────────────── */

void emit_ctx_cleanup(EmitCtx *ctx);

/* ── Function + instruction emission (shared with emit_web.c, Phase 6) ──
 *
 * These three helpers are defined in src/lir/emit_c.c. They were static
 * until Phase 6 plan 01 promoted them so that src/lir/emit_web.c can
 * reuse the native emitter's function-body + per-instruction dispatch
 * path for every non-main-loop function and for the frame-callback
 * body of the main-loop function. Zero behavioral change in emit_c.c.
 */
void emit_func_signature(Iron_StrBuf *sb, IronLIR_Func *fn,
                         EmitCtx *ctx, bool with_newline);
void emit_func_body(EmitCtx *ctx, IronLIR_Func *fn);
void emit_instr(Iron_StrBuf *sb, IronLIR_Instr *instr,
                IronLIR_Func *fn, EmitCtx *ctx);

#endif /* IRON_EMIT_HELPERS_H */
