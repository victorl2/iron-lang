#ifndef IRON_LSP_FACADE_NAV_COMMON_H
#define IRON_LSP_FACADE_NAV_COMMON_H

/* Phase 3 Plan 03 Task 02 -- shared helpers for the NAV facade TUs.
 *
 * Every facade/nav endpoint TU needs:
 *   1. Parse LSP `Position` out of the JSON params.
 *   2. Resolve the document's canonical-path form of a file:// URI
 *      (for workspace_index lookups when the target is a closed file).
 *   3. Build a LocationLink object (with fallback to Location when the
 *      client did not advertise definitionLinkSupport).
 *   4. Convert an Iron_Span from any file to an LSP Range by materialising
 *      a transient IronLsp_Document-alike with the file's text + line
 *      index. In Plan 03 all targets are either (a) the current open
 *      document or (b) a workspace-index entry — both already carry a
 *      line index.
 *
 * The helpers are intentionally header-only where possible so the
 * NAV TUs don't add another translation unit per endpoint. */

#include "lsp/facade/types.h"          /* IronLsp_Position, Range, Encoding */
#include "lsp/store/document.h"        /* IronLsp_Document */
#include "lsp/store/line_index.h"      /* IronLsp_LineIndex */
#include "lsp/store/workspace_index.h" /* IronLsp_IndexEntry */
#include "diagnostics/diagnostics.h"   /* Iron_Span */
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── LocationLink ─────────────────────────────────────────────────── */

typedef struct IronLsp_LocationLink {
    IronLsp_Range  origin_selection_range;
    const char    *target_uri;              /* arena-owned */
    IronLsp_Range  target_range;
    IronLsp_Range  target_selection_range;
} IronLsp_LocationLink;

/* ── JSON parameter parsing ──────────────────────────────────────── */

/* Extract { line, character } from `params.position`. Returns true on
 * success. On failure `*out` is unchanged. */
bool ilsp_nav_parse_position(yyjson_val *params, IronLsp_Position *out);

/* Extract the `params.textDocument.uri` string. Returns NULL if absent. */
const char *ilsp_nav_parse_uri(yyjson_val *params);

/* Stringify a JSON-RPC id (int / uint / str / null). malloc'd; caller
 * frees. NULL is returned only for a NULL/null id. */
char *ilsp_nav_stringify_id(yyjson_val *id);

/* ── Span -> Range over any file ─────────────────────────────────── */

/* Convert an Iron_Span to an LSP Range using the given text + line
 * index + encoding. This is the cross-file analogue of
 * ilsp_span_to_lsp_range (which takes a full IronLsp_Document). */
IronLsp_Range ilsp_nav_span_to_range_via_lineidx(
    Iron_Span                       span,
    const char                     *text,
    size_t                          text_len,
    const IronLsp_LineIndex        *idx,
    IronLsp_PositionEncoding        enc);

/* Given a workspace-index entry (closed-file snapshot), convert an
 * Iron_Span to an LSP Range using the entry's line index + source. */
IronLsp_Range ilsp_nav_entry_span_to_range(
    const IronLsp_IndexEntry       *entry,
    Iron_Span                       span,
    IronLsp_PositionEncoding        enc);

/* ── URI / path helpers ──────────────────────────────────────────── */

/* Turn a canonical filesystem path into a file:// URI (arena-allocated). */
const char *ilsp_nav_path_to_uri(const char *canonical_path, Iron_Arena *arena);

/* Turn a file:// URI into a canonical path. Returns arena-owned string,
 * or NULL on malformed input. Does NOT call realpath(3) -- callers that
 * need realpath normalization should invoke it separately. */
const char *ilsp_nav_uri_to_path(const char *uri, Iron_Arena *arena);

/* ── LocationLink -> JSON ────────────────────────────────────────── */

/* Build a JSON object for a single LocationLink. When
 * `client_supports_link` is false, emits the legacy Location shape
 * `{ uri, range }` using target_range instead. */
yyjson_mut_val *ilsp_nav_build_location_link_json(
    yyjson_mut_doc             *doc,
    const IronLsp_LocationLink *link,
    bool                        client_supports_link);

/* Build an array of LocationLinks (or Locations on fallback). */
yyjson_mut_val *ilsp_nav_build_location_link_array(
    yyjson_mut_doc                  *doc,
    const IronLsp_LocationLink      *links,
    size_t                           n,
    bool                             client_supports_link);

/* ── Response envelope ───────────────────────────────────────────── */

/* Build a full `{ jsonrpc, id, result }` response body where `result`
 * is the provided value. Caller passes an arena-bound doc. */
yyjson_mut_val *ilsp_nav_wrap_response(yyjson_mut_doc   *doc,
                                        yyjson_val       *request_id,
                                        yyjson_mut_val   *result_val);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_COMMON_H */
