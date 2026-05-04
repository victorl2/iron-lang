#ifndef ILSP_COMPLETION_BUCKETS_H
#define ILSP_COMPLETION_BUCKETS_H

/* Phase 4 Plan 04-02 Task 02 (EDIT-01, EDIT-02, D-01, D-03) -- 6-bucket
 * completion candidate builder.
 *
 * Bucket ordering per D-01 (also encoded in sortText):
 *   1. Local scope
 *   2. Top-level same-file
 *   3. Same-module (already imported)
 *   4. Stdlib (importable)
 *   5. Deps (importable)
 *   6. Keywords
 *
 * Special contexts (per D-01):
 *   - MEMBER_AFTER_DOT          -> only fields + methods of receiver type
 *   - IMPORT_PATH               -> only module names from stdlib + deps
 *   - QUALIFIED_AFTER_COLON     -> only module members
 *   - TYPE_POSITION             -> only type decls (Object/Interface/Enum + primitives)
 *   - PATTERN_POSITION          -> only enum variants + primitives + _ wildcard
 *
 * Hard limits (T-4-2 DoS mitigation):
 *   - cap at 128 candidates after fuzzy sort
 *   - cancel polled at START of each bucket and every 64 items inside the
 *     inner fuzzy-match loop
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lsp/facade/edit/complete/auto_import.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "parser/ast.h"      /* Iron_Program typedef (anonymous struct); Iron_Node */
#include "util/arena.h"      /* Iron_Arena typedef (anonymous struct) */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ILSP_COMPLETION_BUCKET_LOCAL        = 1,
    ILSP_COMPLETION_BUCKET_TOP_LEVEL    = 2,
    ILSP_COMPLETION_BUCKET_IMPORTED     = 3,
    ILSP_COMPLETION_BUCKET_STDLIB       = 4,
    ILSP_COMPLETION_BUCKET_DEPS         = 5,
    ILSP_COMPLETION_BUCKET_KEYWORDS     = 6,
} IronLsp_CompletionBucket;

/* Candidate item emitted by the bucket builder. All string fields are
 * arena-owned (the caller's arena passed into _build). */
typedef struct {
    const char  *label;
    const char  *insert_text;
    int          kind;                /* LSP CompletionItemKind 1..25 */
    int          bucket;              /* IronLsp_CompletionBucket */
    double       fuzzy_score;         /* higher = better */
    const char  *detail;
    const char  *canonical_path;
    const char  *name_path;
    uint64_t     content_hash;
    bool         is_extern;
    bool         needs_auto_import;   /* buckets 4+5 = true */
    /* Plan 04-03 Task 03: snippet + auto-import wiring. */
    int          insert_text_format;  /* LSP InsertTextFormat: 1=PlainText, 2=Snippet */
    const IronLsp_AutoImportEdit *additional_text_edit; /* arena-owned; NULLable */
    /* Optional AST node pointer for snippet metadata extraction (param
     * names on func calls, field names on object literals, etc.).
     * May be NULL for candidates that originate from a bucket with no
     * backing decl (e.g. keyword bucket). */
    const Iron_Node *decl_node;
} IronLsp_CompletionCandidate;

/* Opaque forward decls for types that ARE struct-named. */
struct IronLsp_Server;
struct IronLsp_Document;

/* Build the candidate list. The caller owns the output array (backed by
 * the provided arena). Polls `cancel` at bucket boundaries and every 64
 * items inside the fuzzy-scoring loop. */
void ilsp_complete_buckets_build(struct IronLsp_Server             *server,
                                   struct IronLsp_Document           *doc,
                                   Iron_Program                      *program,
                                   size_t                             cursor_byte_offset,
                                   IronLsp_CompletionContext          ctx,
                                   const char                        *query_prefix,
                                   _Atomic bool                      *cancel,
                                   Iron_Arena                        *arena,
                                   IronLsp_CompletionCandidate      **out_cands,
                                   size_t                            *out_n);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_COMPLETION_BUCKETS_H */
