/* Phase 2 Plan 04 Task 02 (CORE-09, CORE-10, CORE-12) -- Document store.
 *
 * See document.h for the contract. Key algorithms:
 *
 * Incremental edit (ilsp_document_apply_incremental):
 *   1. Guard: new_version > doc->version (T-02-13 mitigation).
 *   2. Convert range start/end (line, character) to byte offsets via the
 *      line index + UTF module.
 *   3. Guard: 0 <= start_byte <= end_byte <= text_len (T-02-09).
 *   4. Compute delta = new_len - (end_byte - start_byte). If text_len +
 *      delta > text_cap, realloc (doubling).
 *   5. memmove the tail; memcpy the new text into the gap.
 *   6. Update text_len, rebuild line_starts[], bump version, recompute
 *      SHA-256.
 *
 * All three guard paths log to stderr and return false WITHOUT mutating
 * state. The logging is deliberately stderr-only in Plan 04 -- the
 * structured JSON-line logger lands in Plan 06 (CORE-21). */
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/sha256.h"
#include "lsp/store/utf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ──────────────────────────────────────────────── */

/* Locate the byte range of line N within text. Returns (start, end_ex).
 * end_ex is the exclusive end (i.e., the byte index of the '\n' if there
 * is one, or text_len for the last line). */
static void line_range_bytes(const IronLsp_Document *doc, uint32_t line,
                              size_t *out_start, size_t *out_end_ex) {
    size_t start = ilsp_byte_of_line(&doc->line_idx, line);
    size_t end;
    ptrdiff_t nlines = doc->line_idx.starts
                       ? (ptrdiff_t)(line + 1)
                       : 0;
    /* Next line's start = end of this line + 1 (past the '\n'). */
    if (nlines < (ptrdiff_t)(sizeof(uint32_t) * 0 + 0) /* noop */) {
        end = doc->text_len;
    }
    {
        /* Is there a line (N+1)? Then end_ex = byte_of_line(N+1) - 1 (the
         * '\n' is at that byte - 1). Otherwise end = text_len. */
        size_t next = ilsp_byte_of_line(&doc->line_idx, line + 1);
        if (next > start) {
            /* next points at first byte AFTER the '\n'. The '\n' is at
             * next - 1. The line content (excluding '\n') ends at next - 1. */
            end = next - 1;
        } else {
            end = doc->text_len;
        }
    }
    *out_start  = start;
    *out_end_ex = end;
}

size_t ilsp_document_position_to_byte(const IronLsp_Document  *doc,
                                       uint32_t                 line,
                                       uint32_t                 character,
                                       IronLsp_PositionEncoding enc) {
    if (!doc) return 0;
    size_t line_start, line_end;
    line_range_bytes(doc, line, &line_start, &line_end);
    if (line_start > doc->text_len) return doc->text_len;
    size_t line_len = (line_end > line_start) ? (line_end - line_start) : 0;
    const char *line_ptr = doc->text + line_start;

    size_t col_byte;
    if (enc == ILSP_ENC_UTF8) {
        col_byte = ilsp_utf8_column_to_utf8_byte(line_ptr, line_len, character);
    } else {
        col_byte = ilsp_utf16_column_to_utf8_byte(line_ptr, line_len, character);
    }
    if (col_byte > line_len) col_byte = line_len;
    return line_start + col_byte;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

IronLsp_Document *ilsp_document_create(const char *uri,
                                        const char *text, size_t len,
                                        int32_t     version) {
    if (!uri) return NULL;
    IronLsp_Document *doc = (IronLsp_Document *)calloc(1, sizeof(*doc));
    if (!doc) return NULL;

    doc->uri = strdup(uri);
    if (!doc->uri) { free(doc); return NULL; }

    /* Allocate buffer with 64-byte slack so small inserts don't thrash
     * realloc. Minimum cap is 64 bytes (covers the common "empty doc"
     * case without a free-on-every-keystroke pattern). The +1 beyond
     * text_len holds a trailing NUL so consumers that treat doc->text
     * as a C-string (iron_lexer_create and the compiler frontend, which
     * take `const char *` without a length) never read past the end. */
    size_t cap = (len > 0 ? len : 1) * 2 + 64;
    doc->text = (char *)malloc(cap);
    if (!doc->text) { free(doc->uri); free(doc); return NULL; }
    if (len > 0) memcpy(doc->text, text, len);
    doc->text[len] = '\0';
    doc->text_len = len;
    doc->text_cap = cap;

    doc->version = version;

    ilsp_line_index_init(&doc->line_idx);
    ilsp_line_index_rebuild(&doc->line_idx, doc->text, doc->text_len);

    ilsp_sha256((const uint8_t *)doc->text, doc->text_len, doc->sha256);

    /* Plan 05: worker + mailbox + SIGABRT boundary fields.
     * calloc above zeroed everything; explicitly init the atomics to be
     * portable across atomic_bool implementations that require atomic_init. */
    doc->mailbox        = NULL;
    doc->worker_started = false;
    doc->abort_count    = 0;
    atomic_init(&doc->quarantined, false);
    atomic_init(&doc->shutdown,    false);

    return doc;
}

void ilsp_document_destroy(IronLsp_Document *doc) {
    if (!doc) return;
    if (doc->uri)  free(doc->uri);
    if (doc->text) free(doc->text);
    ilsp_line_index_destroy(&doc->line_idx);
    free(doc);
}

/* Grow the text buffer to at least `need_cap` bytes. Returns false on OOM. */
static bool grow_cap(IronLsp_Document *doc, size_t need_cap) {
    if (doc->text_cap >= need_cap) return true;
    size_t new_cap = doc->text_cap > 0 ? doc->text_cap : 64;
    while (new_cap < need_cap) {
        if (new_cap > (SIZE_MAX >> 1)) return false;   /* overflow guard */
        new_cap *= 2;
    }
    char *p = (char *)realloc(doc->text, new_cap);
    if (!p) return false;
    doc->text     = p;
    doc->text_cap = new_cap;
    return true;
}

bool ilsp_document_apply_full_replace(IronLsp_Document *doc,
                                       const char *new_text, size_t new_len,
                                       int32_t      new_version) {
    if (!doc) return false;
    if (new_version <= doc->version) {
        fprintf(stderr,
                "ironls: document_apply_full_replace: version regression "
                "(current=%d, incoming=%d) -- edit rejected\n",
                doc->version, new_version);
        return false;
    }
    /* +1 so the trailing NUL always fits (see ilsp_document_create). */
    if (!grow_cap(doc, new_len + 1)) return false;
    if (new_len > 0 && new_text) memcpy(doc->text, new_text, new_len);
    doc->text[new_len] = '\0';
    doc->text_len = new_len;
    doc->version  = new_version;
    ilsp_line_index_rebuild(&doc->line_idx, doc->text, doc->text_len);
    ilsp_sha256((const uint8_t *)doc->text, doc->text_len, doc->sha256);
    return true;
}

bool ilsp_document_apply_incremental(IronLsp_Document        *doc,
                                      IronLsp_Range            range,
                                      const char              *new_text,
                                      size_t                   new_len,
                                      IronLsp_PositionEncoding enc,
                                      int32_t                  new_version) {
    if (!doc) return false;
    if (new_version <= doc->version) {
        fprintf(stderr,
                "ironls: document_apply_incremental: version regression "
                "(current=%d, incoming=%d) -- edit rejected\n",
                doc->version, new_version);
        return false;
    }

    size_t start_byte = ilsp_document_position_to_byte(
        doc, range.start.line, range.start.character, enc);
    size_t end_byte   = ilsp_document_position_to_byte(
        doc, range.end.line,   range.end.character,   enc);

    /* Bounds check (T-02-09). */
    if (start_byte > end_byte || end_byte > doc->text_len) {
        fprintf(stderr,
                "ironls: document_apply_incremental: out-of-bounds range "
                "start=%zu end=%zu text_len=%zu -- edit rejected\n",
                start_byte, end_byte, doc->text_len);
        return false;
    }

    size_t old_span = end_byte - start_byte;
    /* delta is signed; compute via separate branches to avoid UB. */
    size_t final_len;
    if (new_len >= old_span) {
        final_len = doc->text_len + (new_len - old_span);
    } else {
        final_len = doc->text_len - (old_span - new_len);
    }

    /* +1 so the trailing NUL always fits (see ilsp_document_create). */
    if (!grow_cap(doc, final_len + 1)) return false;

    /* memmove the tail (from end_byte to the new position post-gap). */
    size_t tail_len = doc->text_len - end_byte;
    if (tail_len > 0) {
        memmove(doc->text + start_byte + new_len,
                doc->text + end_byte,
                tail_len);
    }
    /* Splice new text into the gap. */
    if (new_len > 0 && new_text) {
        memcpy(doc->text + start_byte, new_text, new_len);
    }

    doc->text[final_len] = '\0';
    doc->text_len = final_len;
    doc->version  = new_version;

    ilsp_line_index_rebuild(&doc->line_idx, doc->text, doc->text_len);
    ilsp_sha256((const uint8_t *)doc->text, doc->text_len, doc->sha256);

    return true;
}
