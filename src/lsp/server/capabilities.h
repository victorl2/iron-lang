#ifndef IRON_LSP_SERVER_CAPABILITIES_H
#define IRON_LSP_SERVER_CAPABILITIES_H

/* Phase 2 Plan 03 Task 02 -- ServerCapabilities builder derived from the
 * compile-time handler registry. Every capability advertised has a
 * registered handler (PITFALLS.md Pitfall 15 mitigation; CORE-06).
 *
 * positionEncoding negotiation (CORE-07): prefer UTF-8 when the client
 * offers it; otherwise UTF-16 (LSP 3.17 spec default). */

#include "vendor/yyjson/yyjson.h"
#include "lsp/facade/types.h"

/* Build a ServerCapabilities JSON object.
 *   - Iterates ilsp_handler_table; for each entry with capability != NULL,
 *     sets the corresponding field to true (or a shaped sub-object where
 *     the spec requires one, e.g. textDocumentSync).
 *   - Always sets "positionEncoding" to the negotiated enum.
 * Returned value is a child of `out_doc`; caller retains ownership of the
 * doc. */
yyjson_mut_val *ilsp_capabilities_build(yyjson_mut_doc           *out_doc,
                                        IronLsp_PositionEncoding  negotiated_enc);

/* Read client's general.positionEncodings (nullable) and pick the
 * server's preference:
 *   - UTF-8 if the array contains "utf-8" (preferred)
 *   - UTF-16 if the array contains only "utf-16"
 *   - UTF-16 if the array is missing/empty (spec default) */
IronLsp_PositionEncoding ilsp_capabilities_negotiate_position_encoding(yyjson_val *client_caps);

/* Convenience: canonical spec string for the negotiated encoding. */
const char *ilsp_position_encoding_str(IronLsp_PositionEncoding e);

#endif /* IRON_LSP_SERVER_CAPABILITIES_H */
