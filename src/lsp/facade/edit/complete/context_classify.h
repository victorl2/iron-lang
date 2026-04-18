#ifndef ILSP_COMPLETION_CONTEXT_CLASSIFY_H
#define ILSP_COMPLETION_CONTEXT_CLASSIFY_H

/* Phase 4 Plan 04-02 Task 01 (EDIT-06, D-01, D-16) -- completion context
 * classifier.
 *
 * Given a document + byte offset (cursor position pre-converted from LSP
 * Position via src/lsp/store/utf.c), return one of seven enum values
 * describing what kind of completion should be produced. The 6-bucket
 * builder (buckets.c) consumes the enum to decide which buckets to emit
 * and which to short-circuit.
 *
 * The classifier is PURE: it walks the document's UTF-8 byte buffer
 * backward from `byte_offset` without mutating, allocating, or touching
 * the analyzer. It only consults byte classification (isspace/isalnum)
 * and the generated ILSP_COMPLETION_KEYWORDS list for keyword detection.
 *
 * Trigger-character mapping per D-16:
 *   '.' -> MEMBER_AFTER_DOT
 *   ':' after ':' -> QUALIFIED_AFTER_COLON  (the Iron form is `Mod::foo`)
 *   ':' after a decl/param ident -> TYPE_POSITION
 *   '/' (after `import`) -> IMPORT_PATH (import-path continuation)
 *   word char after statement terminator -> STATEMENT_HEAD
 *
 * Fallback: ILSP_CCTX_EXPR_HEAD. */

#include <stddef.h>

struct IronLsp_Document;

typedef enum {
    ILSP_CCTX_EXPR_HEAD = 0,
    ILSP_CCTX_MEMBER_AFTER_DOT,
    ILSP_CCTX_QUALIFIED_AFTER_COLON,
    ILSP_CCTX_IMPORT_PATH,
    ILSP_CCTX_TYPE_POSITION,
    ILSP_CCTX_PATTERN_POSITION,
    ILSP_CCTX_STATEMENT_HEAD,
} IronLsp_CompletionContext;

/* Classify the completion context at (doc, byte_offset). Walks backward
 * from byte_offset over the document buffer examining tokens / keywords
 * / trigger characters. Never mutates doc. Never allocates. Returns the
 * classified context enum. */
IronLsp_CompletionContext ilsp_completion_context_classify(
    const struct IronLsp_Document *doc, size_t byte_offset);

/* Test seam: classify a raw UTF-8 buffer without requiring a full
 * IronLsp_Document. Used by tests/unit/test_completion_context_classify.c
 * so unit tests do not need to allocate the full document struct. The
 * buffer is NOT required to be NUL-terminated; `len` bounds the walk. */
IronLsp_CompletionContext ilsp_completion_context_classify_buf(
    const char *buf, size_t len, size_t byte_offset);

#endif /* ILSP_COMPLETION_CONTEXT_CLASSIFY_H */
