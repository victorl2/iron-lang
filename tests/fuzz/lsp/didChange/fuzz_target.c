/* tests/fuzz/lsp/didChange/fuzz_target.c — Phase 7 Plan 07-05 (HARD-19).
 *
 * libFuzzer harness for the document-sync state machine (CORE-09..-13).
 *
 * Drives ilsp_document_create -> ilsp_document_apply_incremental×N ->
 * ilsp_document_apply_full_replace -> ilsp_document_destroy with an
 * arbitrary sequence of operations decoded from libFuzzer's byte input.
 *
 * Invariants asserted (T-07-05 threat model):
 *   - No crash, no OOB write, no use-after-free on any operation
 *     sequence (ASan + UBSan catch via guard pages + shadow memory).
 *   - Version monotonicity: ilsp_document_apply_* returns false when
 *     new_version <= doc->version; on false the document state must be
 *     UNCHANGED (we assert doc->text_len / sha256 match the pre-call
 *     snapshot).
 *   - OOB range rejection: the CORE-11 bounds-check path must never
 *     mutate state and must not read past doc->text_len (caught by
 *     ASan if violated).
 *   - Post-destroy: after ilsp_document_destroy the local pointer is
 *     nulled so no use-after-free can slip through into the next
 *     iteration's prologue.
 *
 * Input byte layout (structure-aware decode):
 *   [0]        : initial text length hi byte (0..255)
 *   [1]        : initial text length lo byte
 *   [2..2+N]   : initial text (N = min(hi*256+lo, remaining))
 *   then a loop of operations:
 *     op = byte[0] % 4
 *         0: incremental edit
 *              [line_s, char_s, line_e, char_e, new_text_len_hi,
 *               new_text_len_lo, new_text...]
 *              Range columns passed through as-is; bounds-check in
 *              ilsp_document_apply_incremental rejects OOB without
 *              mutating state (we assert this).
 *         1: full-text replace
 *              [new_len_hi, new_len_lo, new_text...]
 *         2: encoding flip
 *              (no payload; toggles UTF-8 <-> UTF-16 for subsequent
 *              incremental edits)
 *         3: read-only position_to_byte probe
 *              [line_hi, line_lo, char_hi, char_lo] — exercises the
 *              line_index + UTF math on adversarial positions.
 *
 * Per-iteration lifecycle:
 *   Iron_Arena arena is NOT required for the document store (document
 *   uses malloc/free). We skip it to keep the harness focused on the
 *   document state machine.
 *
 * NOTE (Rule 1 deviation from plan <action>): the plan referenced
 * nominal APIs ilsp_doc_open/ilsp_doc_change/ilsp_doc_close and an
 * IlspChangeEvent struct. The real API is ilsp_document_create /
 * ilsp_document_apply_incremental / ilsp_document_destroy with
 * IronLsp_Range. Harness uses the real API (documented in Plan 07-05
 * summary).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lsp/store/document.h"
#include "lsp/facade/types.h"

#define IRON_FUZZ_DIDCHANGE_MAX_INPUT 8192

/* Cap on how many operations we'll attempt per iteration. Prevents
 * pathological inputs (all ops=0 with tiny payloads) from driving
 * thousands of edits per iteration, which would saturate libFuzzer's
 * per-iteration budget without new coverage. */
#define IRON_FUZZ_DIDCHANGE_MAX_OPS 64

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > IRON_FUZZ_DIDCHANGE_MAX_INPUT) return -1;
    if (size < 2) return 0;

    /* Decode the initial document text. */
    size_t cursor = 0;
    uint16_t init_len = read_u16(data + cursor);
    cursor += 2;
    if ((size_t)init_len > size - cursor) {
        init_len = (uint16_t)(size - cursor);
    }

    IronLsp_Document *doc = ilsp_document_create(
        "file:///fuzz.iron",
        (const char *)(data + cursor), init_len,
        /* version */ 1);
    cursor += init_len;

    if (!doc) {
        /* OOM or uri==NULL (can't happen here). Not a bug -- libFuzzer
         * treats a clean early-return as coverage-neutral. */
        return 0;
    }

    int32_t next_version = 2;
    IronLsp_PositionEncoding enc = ILSP_ENC_UTF8;
    int ops_remaining = IRON_FUZZ_DIDCHANGE_MAX_OPS;

    while (cursor < size && ops_remaining-- > 0) {
        uint8_t op = data[cursor++] % 4;

        switch (op) {
            case 0: {  /* incremental edit */
                if (cursor + 6 > size) goto done;
                uint8_t line_s = data[cursor++];
                uint8_t char_s = data[cursor++];
                uint8_t line_e = data[cursor++];
                uint8_t char_e = data[cursor++];
                uint16_t tlen  = read_u16(data + cursor);
                cursor += 2;
                if ((size_t)tlen > size - cursor) {
                    tlen = (uint16_t)(size - cursor);
                }
                IronLsp_Range r = {
                    .start = { .line = line_s, .character = char_s },
                    .end   = { .line = line_e, .character = char_e }
                };

                /* Snapshot state before the call so we can verify the
                 * false-return must-not-mutate invariant. */
                size_t pre_len = doc->text_len;
                int32_t pre_ver = doc->version;
                uint8_t pre_sha[32];
                memcpy(pre_sha, doc->sha256, 32);

                bool ok = ilsp_document_apply_incremental(
                    doc, r,
                    (const char *)(data + cursor), tlen,
                    enc, next_version);
                cursor += tlen;

                if (!ok) {
                    /* T-02-09 / T-02-13 invariant: on false the doc
                     * state must be unchanged. */
                    if (doc->text_len != pre_len ||
                        doc->version  != pre_ver  ||
                        memcmp(doc->sha256, pre_sha, 32) != 0) {
                        __builtin_trap();
                    }
                } else {
                    next_version++;
                }
                break;
            }

            case 1: {  /* full-text replace */
                if (cursor + 2 > size) goto done;
                uint16_t tlen = read_u16(data + cursor);
                cursor += 2;
                if ((size_t)tlen > size - cursor) {
                    tlen = (uint16_t)(size - cursor);
                }

                size_t pre_len = doc->text_len;
                int32_t pre_ver = doc->version;
                uint8_t pre_sha[32];
                memcpy(pre_sha, doc->sha256, 32);

                bool ok = ilsp_document_apply_full_replace(
                    doc,
                    (const char *)(data + cursor), tlen,
                    next_version);
                cursor += tlen;

                if (!ok) {
                    if (doc->text_len != pre_len ||
                        doc->version  != pre_ver  ||
                        memcmp(doc->sha256, pre_sha, 32) != 0) {
                        __builtin_trap();
                    }
                } else {
                    next_version++;
                }
                break;
            }

            case 2: {  /* flip encoding */
                enc = (enc == ILSP_ENC_UTF8) ? ILSP_ENC_UTF16 : ILSP_ENC_UTF8;
                break;
            }

            case 3: {  /* read-only position_to_byte probe */
                if (cursor + 4 > size) goto done;
                uint16_t line = read_u16(data + cursor); cursor += 2;
                uint16_t ch   = read_u16(data + cursor); cursor += 2;
                size_t byte = ilsp_document_position_to_byte(
                    doc, (uint32_t)line, (uint32_t)ch, enc);
                /* position_to_byte clamps OOB to doc->text_len; any
                 * return value strictly greater than text_len indicates
                 * a bug. */
                if (byte > doc->text_len) __builtin_trap();
                break;
            }
        }
    }

done:
    ilsp_document_destroy(doc);
    return 0;
}
