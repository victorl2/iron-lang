/* tests/fuzz/lsp/frame/fuzz_target.c — Phase 7 Plan 07-05 (HARD-19).
 *
 * libFuzzer harness for the LSP Content-Length framer (CORE-02).
 *
 * Strategy: feed libFuzzer's arbitrary bytes into the stateful framer in
 * up to four random-size chunks per iteration, so both single-shot and
 * multi-chunk parse paths get exercised. After each feed we check:
 *
 *   - return value is one of the 4 enumerator values (no undefined
 *     behaviour produced an "int" masquerading as IronLsp_FrameResult);
 *   - on COMPLETE, out_body/out_len are internally consistent and the
 *     reported body length does not exceed the 10 MB CORE-02 cap.
 *
 * Per-iteration lifecycle mirrors tests/fuzz/fuzz_parser.c Pitfall 4:
 * fresh parser per call; ilsp_frame_destroy releases the internal malloc'd
 * buffer so RSS stays flat across millions of iterations.
 *
 * The framer does its own malloc+realloc for the internal buffer, so
 * this target does NOT need an Iron_Arena. It pairs with fuzz_lsp_json
 * (which does exercise the arena path).
 *
 * NOTE: The plan's <action> block referenced a hypothetical
 * `ilsp_frame_parse(data, size, &frame, &err)` single-shot API. The real
 * API is stateful (ilsp_frame_init / ilsp_frame_feed / ilsp_frame_consume
 * / ilsp_frame_destroy) so the harness adapts to the production shape
 * (Rule 1 deviation — documented in Plan 07-05 summary).
 */

#include <stddef.h>
#include <stdint.h>

#include "lsp/transport/frame.h"

/* Input-size cap mirrors the 10 MB CORE-02 frame cap plus a small amount
 * of header slack. Inputs larger than this almost-always hit libFuzzer's
 * timeout rather than surfacing real bugs. */
#define IRON_FUZZ_FRAME_MAX_INPUT (11UL * 1024UL * 1024UL)

/* Upper bound on the body length we'll let the framer claim to have
 * parsed successfully (CORE-02 cap). */
#define IRON_FUZZ_FRAME_MAX_BODY (10UL * 1024UL * 1024UL)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > IRON_FUZZ_FRAME_MAX_INPUT) {
        return -1;  /* reject oversized inputs (defense in depth). */
    }

    IronLsp_FrameParser parser;
    ilsp_frame_init(&parser);

    /* Feed the input in up to 4 chunks: this forces the framer's state
     * machine to handle header/body boundaries at arbitrary byte offsets,
     * which is the bug surface we care about. Chunk sizes derive from
     * the input itself (no external RNG — fuzz determinism requires that
     * the same input always drives the same code path). */
    size_t cursor = 0;
    int    chunk_budget = 4;

    while (cursor < size && chunk_budget-- > 0) {
        size_t remaining = size - cursor;
        /* Chunk size derived from the first remaining byte, bounded by
         * remaining bytes. At least 1 byte per chunk to ensure progress. */
        size_t chunk = 1;
        if (remaining > 1) {
            chunk = ((size_t)data[cursor] + 1u);
            if (chunk > remaining) chunk = remaining;
        }

        const char *out_body = NULL;
        size_t      out_len  = 0;
        IronLsp_FrameResult r = ilsp_frame_feed(
            &parser, (const char *)(data + cursor), chunk,
            &out_body, &out_len);

        /* The result must be one of the four enumerated values. Any
         * other value means the state machine produced garbage -- that
         * is a bug libFuzzer should capture as a crash. */
        switch (r) {
            case ILSP_FRAME_RESULT_NEED_MORE:
            case ILSP_FRAME_RESULT_ERROR_OVERSIZED:
            case ILSP_FRAME_RESULT_ERROR_MALFORMED:
                break;
            case ILSP_FRAME_RESULT_COMPLETE:
                /* On COMPLETE, the body pointer must be non-NULL and the
                 * claimed length must not exceed the CORE-02 cap. */
                if (out_body == NULL) __builtin_trap();
                if (out_len > IRON_FUZZ_FRAME_MAX_BODY) __builtin_trap();
                /* Drain and continue so a single input can exercise
                 * multiple frame boundaries (batched LSP traffic). */
                ilsp_frame_consume(&parser);
                break;
            default:
                __builtin_trap();
        }

        /* Stop feeding once the framer has hit an error: production
         * callers close the connection on malformed/oversized input, so
         * we model that here (keeps the fuzzer from exploring invalid
         * post-error states that the production framer will never see). */
        if (r == ILSP_FRAME_RESULT_ERROR_OVERSIZED ||
            r == ILSP_FRAME_RESULT_ERROR_MALFORMED) {
            break;
        }

        cursor += chunk;
    }

    ilsp_frame_destroy(&parser);
    return 0;  /* libFuzzer may add this input to the corpus on new coverage. */
}
