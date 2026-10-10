#ifndef IRON_LSP_FACADE_SEMANTIC_H
#define IRON_LSP_FACADE_SEMANTIC_H

/* textDocument/semanticTokens/full and textDocument/inlayHint (#311). */

#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct IronLsp_Server;
struct IronLsp_Document;

/* Adds `legend` and `full` to the semanticTokensProvider capability. */
void ilsp_semantic_tokens_legend(yyjson_mut_doc *d, yyjson_mut_val *provider);

/* The document's tokens in LSP's relative 5-integer encoding, allocated
 * in `arena`; *out_n is the number of integers. */
uint32_t *ilsp_facade_semantic_tokens(struct IronLsp_Server *server,
                                      struct IronLsp_Document *doc,
                                      _Atomic bool *cancel, Iron_Arena *arena,
                                      size_t *out_n);

typedef struct {
    size_t      off;        /* byte offset the hint follows */
    uint32_t    line;       /* LSP position (client encoding) */
    uint32_t    character;
    const char *label;      /* ": Int", or "w:" before an argument */
    int         kind;       /* LSP InlayHintKind: 1 type, 2 parameter */
} IronLsp_InlayHint;

/* Type hints for bindings without a written type, and parameter names
 * before the arguments of a call, on 0-based lines
 * first_line0 ..= last_line0, allocated in `arena`. */
IronLsp_InlayHint *ilsp_facade_inlay_hints(struct IronLsp_Server *server,
                                           struct IronLsp_Document *doc,
                                           uint32_t first_line0, uint32_t last_line0,
                                           _Atomic bool *cancel, Iron_Arena *arena,
                                           size_t *out_n);

void ilsp_handle_text_document_semantic_tokens_full(struct IronLsp_Server *s,
                                                    struct yyjson_doc     *doc,
                                                    Iron_Arena            *arena);
void ilsp_handle_text_document_inlay_hint(struct IronLsp_Server *s,
                                          struct yyjson_doc     *doc,
                                          Iron_Arena            *arena);

/* Apply an `inlayHints` settings object (`{"parameterNames": false,
 * "bindingTypes": true}`); a key that is absent or not a bool leaves its
 * setting as it is. Returns true when a setting changed. */
bool ilsp_inlay_apply_settings(struct IronLsp_Server *s, yyjson_val *inlay_hints);

/* workspace/didChangeConfiguration: reads settings.iron.inlayHints and,
 * when a setting changed and the client supports it, sends
 * workspace/inlayHint/refresh so the editor asks for hints again. */
void ilsp_handle_workspace_did_change_configuration(struct IronLsp_Server *s,
                                                    struct yyjson_doc     *doc,
                                                    Iron_Arena            *arena);

#endif /* IRON_LSP_FACADE_SEMANTIC_H */
