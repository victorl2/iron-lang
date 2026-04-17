#ifndef IRON_LSP_STORE_DOCUMENT_H
#define IRON_LSP_STORE_DOCUMENT_H

#include <stdint.h>
#include <stddef.h>

/* Document-store skeleton. Plan 02-04 (document store + UTF + watched)
 * fills in IronLsp_Document with buffer, line_starts[], version, URI,
 * and the SHA-256 integrity hash field. Plan 02-05 attaches the
 * per-document sigsetjmp crash boundary. */

struct IronLsp_Document;

#endif /* IRON_LSP_STORE_DOCUMENT_H */
