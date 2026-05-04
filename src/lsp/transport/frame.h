#ifndef IRON_LSP_TRANSPORT_FRAME_H
#define IRON_LSP_TRANSPORT_FRAME_H

/* Phase 2 Plan 02 Task 01 -- Content-Length frame parser.
 *
 * Pure state machine over a caller-provided byte stream. No global state,
 * no thread primitives -- the reader thread (Task 03) drives this parser
 * one chunk at a time. Each parser owns its own heap buffer. Oversize
 * messages (> ILSP_FRAME_MAX_MESSAGE_SIZE) are rejected before allocation,
 * which is the T-02-02 DoS mitigation.
 *
 * LSP 3.17 specifies CRLF header terminators; we also accept LF-only for
 * compatibility with hand-rolled clients. No other leniences are granted.
 */

#include <stddef.h>
#include <stdbool.h>

/* T-02-02 mitigation: reject Content-Length > 10 MB before any allocation. */
#define ILSP_FRAME_MAX_MESSAGE_SIZE (10 * 1024 * 1024)

typedef enum IronLsp_FrameState {
    ILSP_FRAME_STATE_HEADERS,
    ILSP_FRAME_STATE_BODY
} IronLsp_FrameState;

typedef struct IronLsp_FrameParser {
    IronLsp_FrameState state;
    char  *buf;
    size_t buf_len;
    size_t buf_cap;
    size_t content_length;
    size_t body_start;     /* offset within buf of body's first byte */
    bool   have_content_length;
} IronLsp_FrameParser;

typedef enum IronLsp_FrameResult {
    ILSP_FRAME_RESULT_NEED_MORE,
    ILSP_FRAME_RESULT_COMPLETE,
    ILSP_FRAME_RESULT_ERROR_OVERSIZED,
    ILSP_FRAME_RESULT_ERROR_MALFORMED
} IronLsp_FrameResult;

void ilsp_frame_init(IronLsp_FrameParser *p);
void ilsp_frame_destroy(IronLsp_FrameParser *p);

/* Feed in_len bytes (in may be NULL iff in_len == 0). If a complete
 * message is available, returns ILSP_FRAME_RESULT_COMPLETE with *out_body
 * and *out_len set to pointers into p->buf. The caller MUST call
 * ilsp_frame_consume() before the next ilsp_frame_feed() to advance past
 * the completed message. On NEED_MORE/ERROR, *out_body and *out_len are
 * not written to. */
IronLsp_FrameResult ilsp_frame_feed(IronLsp_FrameParser *p,
                                    const char *in, size_t in_len,
                                    const char **out_body, size_t *out_len);

/* Shift completed message out of the internal buffer so the next feed
 * (or the residual bytes of the previous feed) sees the next message. */
void ilsp_frame_consume(IronLsp_FrameParser *p);

#endif /* IRON_LSP_TRANSPORT_FRAME_H */
