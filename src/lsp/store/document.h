#ifndef IRON_LSP_STORE_DOCUMENT_H
#define IRON_LSP_STORE_DOCUMENT_H

/* Phase 2 Plan 04 Task 02 (CORE-09, CORE-10, CORE-12) -- Document store.
 *
 * Per-document state: flat UTF-8 buffer + line_starts[] index + SHA-256
 * integrity hash + URI + version. The buffer is malloc'd / realloc'd --
 * this is the deliberate arena exception documented in CLAUDE.md (the
 * arena allocator has no realloc-in-place or free-individual semantics,
 * and documents outlive a single request).
 *
 * Lifecycle:
 *   - ilsp_document_create: malloc doc struct, copy initial text, build
 *     line index, compute SHA-256, store version.
 *   - ilsp_document_apply_full_replace: drop buffer, copy new text,
 *     rebuild index, bump version.
 *   - ilsp_document_apply_incremental: convert LSP Range to byte offsets,
 *     memmove tail + memcpy new text, grow cap if needed, rebuild index,
 *     bump version.
 *   - ilsp_document_destroy: free buffer + line index + uri + doc.
 *
 * Thread discipline: a given doc is mutated ONLY on the main dispatcher
 * thread (Plan 05 will add per-doc mailbox + worker thread). No locks
 * on the buffer in Plan 04. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lsp/store/line_index.h"
#include "lsp/facade/types.h"   /* IronLsp_PositionEncoding, IronLsp_Range */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_Document {
    char              *uri;          /* malloc'd copy of the client's URI. */
    int32_t            version;      /* last applied textDocument version. */

    char              *text;         /* malloc'd + realloc'd UTF-8 buffer. */
    size_t             text_len;     /* live byte count (not including cap slack). */
    size_t             text_cap;     /* allocated capacity. */

    IronLsp_LineIndex  line_idx;     /* rebuilt on every edit. */

    uint8_t            sha256[32];   /* hash of text[0..text_len]. */
} IronLsp_Document;

/* Create a new document. Copies `text` into a fresh malloc'd buffer.
 * Returns NULL on OOM. */
IronLsp_Document *ilsp_document_create(const char *uri,
                                        const char *text, size_t len,
                                        int32_t     version);

/* Destroy: frees uri, text, line index, and the struct itself. */
void ilsp_document_destroy(IronLsp_Document *doc);

/* Apply a full-text replace. Bumps version after success. Returns
 * false if new_version is not strictly greater than the current version
 * (version monotonicity guard; T-02-13 mitigation). */
bool ilsp_document_apply_full_replace(IronLsp_Document *doc,
                                       const char *new_text, size_t new_len,
                                       int32_t      new_version);

/* Apply an incremental range replacement. `range` columns are in the
 * negotiated encoding `enc`. Returns false on version regression
 * (T-02-13) or out-of-bounds range (T-02-09). On false, the document
 * state is unchanged. */
bool ilsp_document_apply_incremental(IronLsp_Document        *doc,
                                      IronLsp_Range            range,
                                      const char              *new_text,
                                      size_t                   new_len,
                                      IronLsp_PositionEncoding enc,
                                      int32_t                  new_version);

/* Utility: convert an LSP Position to a byte offset within doc->text.
 * Exposed for test harnesses + facade callers. Returns doc->text_len on
 * out-of-bounds. */
size_t ilsp_document_position_to_byte(const IronLsp_Document  *doc,
                                       uint32_t                 line,
                                       uint32_t                 character,
                                       IronLsp_PositionEncoding enc);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_DOCUMENT_H */
