/* Phase 2 Plan 02 Task 01 -- Content-Length frame parser.
 *
 * State machine: HEADERS -> BODY. On BODY complete, returns a pointer into
 * the internal buffer; ilsp_frame_consume shifts the buffer to reset for
 * the next message. No thread primitives, no global state -- the reader
 * thread (Task 03) is the sole user. */
#include "lsp/transport/frame.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ILSP_FRAME_INITIAL_CAPACITY 4096

void ilsp_frame_init(IronLsp_FrameParser *p) {
    if (!p) return;
    p->state               = ILSP_FRAME_STATE_HEADERS;
    p->buf                 = (char *)malloc(ILSP_FRAME_INITIAL_CAPACITY);
    p->buf_len             = 0;
    p->buf_cap             = p->buf ? ILSP_FRAME_INITIAL_CAPACITY : 0;
    p->content_length      = 0;
    p->body_start          = 0;
    p->have_content_length = false;
}

void ilsp_frame_destroy(IronLsp_FrameParser *p) {
    if (!p) return;
    free(p->buf);
    p->buf                 = NULL;
    p->buf_len             = 0;
    p->buf_cap             = 0;
    p->state               = ILSP_FRAME_STATE_HEADERS;
    p->content_length      = 0;
    p->body_start          = 0;
    p->have_content_length = false;
}

/* Grow buf to at least `needed` bytes by doubling. Returns false on malloc
 * failure. */
static bool frame_reserve(IronLsp_FrameParser *p, size_t needed) {
    if (needed <= p->buf_cap) return true;
    size_t new_cap = p->buf_cap ? p->buf_cap : ILSP_FRAME_INITIAL_CAPACITY;
    while (new_cap < needed) {
        /* Guard against overflow on pathological growth. */
        if (new_cap > (SIZE_MAX / 2)) { new_cap = needed; break; }
        new_cap *= 2;
    }
    char *nb = (char *)realloc(p->buf, new_cap);
    if (!nb) return false;
    p->buf     = nb;
    p->buf_cap = new_cap;
    return true;
}

/* Case-insensitive ASCII prefix match: does `line` (length `line_len`)
 * start with `prefix`? prefix is a null-terminated ASCII literal. */
static bool header_name_matches(const char *line, size_t line_len,
                                const char *prefix) {
    size_t pl = strlen(prefix);
    if (line_len < pl) return false;
    for (size_t i = 0; i < pl; i++) {
        unsigned char a = (unsigned char)line[i];
        unsigned char b = (unsigned char)prefix[i];
        if (tolower(a) != tolower(b)) return false;
    }
    return true;
}

/* Find end of the next header line in p->buf starting at `from`. Sets
 * *line_end to the offset of the first byte of the terminator (\r or \n),
 * *next_start to the offset of the first byte past the terminator. Returns
 * true if a full line (ending in \r\n or \n) was found within buf_len. */
static bool find_line_end(const IronLsp_FrameParser *p, size_t from,
                          size_t *line_end, size_t *next_start) {
    for (size_t i = from; i < p->buf_len; i++) {
        char c = p->buf[i];
        if (c == '\r') {
            if (i + 1 < p->buf_len && p->buf[i + 1] == '\n') {
                *line_end   = i;
                *next_start = i + 2;
                return true;
            }
            /* Stand-alone CR without LF: not a valid header terminator.
             * We could reject as malformed, but LSP framers never emit a
             * bare CR; treat as NEED_MORE (maybe \n is about to arrive). */
            return false;
        } else if (c == '\n') {
            /* LF-only lenience (hand-rolled clients). */
            *line_end   = i;
            *next_start = i + 1;
            return true;
        }
    }
    return false;
}

/* Parse the headers block starting at offset 0 of p->buf. Returns:
 *   -  0 on success; state transitioned to BODY and body_start is set.
 *   - -1 on NEED_MORE (not enough bytes yet).
 *   - -2 on ERROR_OVERSIZED (Content-Length > ILSP_FRAME_MAX_MESSAGE_SIZE).
 *   - -3 on ERROR_MALFORMED (blank line with no Content-Length, or a header
 *        with no colon, or a Content-Length with a non-numeric value). */
static int frame_parse_headers(IronLsp_FrameParser *p) {
    size_t cursor = 0;
    while (cursor < p->buf_len) {
        size_t line_end = 0, next_start = 0;
        if (!find_line_end(p, cursor, &line_end, &next_start)) {
            return -1;  /* NEED_MORE: partial header */
        }
        size_t line_len = line_end - cursor;
        if (line_len == 0) {
            /* Blank line -> end of headers. */
            if (!p->have_content_length) return -3;  /* MALFORMED */
            p->state      = ILSP_FRAME_STATE_BODY;
            p->body_start = next_start;
            return 0;
        }

        /* Parse "Header-Name: value" -- locate the colon. */
        const char *line = p->buf + cursor;
        size_t colon = 0;
        bool found_colon = false;
        for (size_t i = 0; i < line_len; i++) {
            if (line[i] == ':') { colon = i; found_colon = true; break; }
        }
        if (!found_colon) return -3;  /* MALFORMED */

        if (header_name_matches(line, colon, "Content-Length")) {
            /* Skip colon and leading whitespace, parse integer. */
            size_t vstart = colon + 1;
            while (vstart < line_len &&
                   (line[vstart] == ' ' || line[vstart] == '\t')) {
                vstart++;
            }
            /* Copy value into a NUL-terminated temp buffer for strtoul. */
            char tmp[32];
            size_t vlen = line_len - vstart;
            if (vlen == 0 || vlen >= sizeof(tmp)) return -3;  /* MALFORMED */
            memcpy(tmp, line + vstart, vlen);
            tmp[vlen] = '\0';
            char *endp = NULL;
            unsigned long long v = strtoull(tmp, &endp, 10);
            if (endp == tmp) return -3;  /* MALFORMED: no digits */
            /* Allow trailing whitespace in the value. */
            while (endp && *endp) {
                if (*endp != ' ' && *endp != '\t') return -3;
                endp++;
            }
            if (v > (unsigned long long)ILSP_FRAME_MAX_MESSAGE_SIZE) {
                return -2;  /* OVERSIZED (T-02-02) */
            }
            p->content_length      = (size_t)v;
            p->have_content_length = true;
        }
        /* else: ignore other headers (Content-Type, etc.). */

        cursor = next_start;
    }
    return -1;  /* NEED_MORE: ran off end without seeing blank line */
}

IronLsp_FrameResult ilsp_frame_feed(IronLsp_FrameParser *p,
                                    const char *in, size_t in_len,
                                    const char **out_body, size_t *out_len) {
    if (!p) return ILSP_FRAME_RESULT_ERROR_MALFORMED;

    /* Append incoming bytes to the internal buffer. */
    if (in_len > 0 && in != NULL) {
        if (!frame_reserve(p, p->buf_len + in_len)) {
            return ILSP_FRAME_RESULT_ERROR_MALFORMED;  /* OOM: treat as error */
        }
        memcpy(p->buf + p->buf_len, in, in_len);
        p->buf_len += in_len;
    }

    if (p->state == ILSP_FRAME_STATE_HEADERS) {
        int rc = frame_parse_headers(p);
        if (rc == -1) return ILSP_FRAME_RESULT_NEED_MORE;
        if (rc == -2) return ILSP_FRAME_RESULT_ERROR_OVERSIZED;
        if (rc == -3) return ILSP_FRAME_RESULT_ERROR_MALFORMED;
        /* rc == 0: fall through to BODY check. */
    }

    if (p->state == ILSP_FRAME_STATE_BODY) {
        size_t have = p->buf_len - p->body_start;
        if (have >= p->content_length) {
            /* Even for zero-length body, out_body should be non-NULL.
             * p->buf + p->body_start is a stable pointer into the buffer. */
            *out_body = p->buf + p->body_start;
            *out_len  = p->content_length;
            return ILSP_FRAME_RESULT_COMPLETE;
        }
        return ILSP_FRAME_RESULT_NEED_MORE;
    }

    return ILSP_FRAME_RESULT_NEED_MORE;
}

void ilsp_frame_consume(IronLsp_FrameParser *p) {
    if (!p || p->state != ILSP_FRAME_STATE_BODY) return;
    size_t consumed = p->body_start + p->content_length;
    if (consumed > p->buf_len) consumed = p->buf_len;  /* defensive */
    size_t remaining = p->buf_len - consumed;
    if (remaining > 0) {
        memmove(p->buf, p->buf + consumed, remaining);
    }
    p->buf_len             = remaining;
    p->state               = ILSP_FRAME_STATE_HEADERS;
    p->content_length      = 0;
    p->body_start          = 0;
    p->have_content_length = false;
}
