/* Phase 2 Plan 02 Task 03 -- stdin reader thread.
 *
 * Single-threaded pump: read bytes from `source`, feed them to the framer,
 * dispatch on each COMPLETE. Shutdown paths:
 *   1. source reaches EOF (parent editor closed stdin) -- normal exit.
 *   2. source errors -- log to stderr, exit (Plan 06 adds proper log sink).
 *   3. Explicit shutdown flag -- exit at the next loop iteration.
 *
 * Framing errors (malformed headers, oversize) are fatal for the stream
 * (the Content-Length boundary is gone so we cannot recover). We log and
 * exit the thread; the server process remains up so the writer can flush
 * in-flight responses before a clean shutdown. */
#include "lsp/transport/reader.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/iron_runtime.h"  /* IRON_THREAD_* */
#include "lsp/transport/frame.h"

#define ILSP_READER_CHUNK_SIZE 4096

struct IronLsp_Reader {
    FILE                *source;
    IronLsp_OnMessage    on_message;
    void                *ctx;
    IronLsp_FrameParser  parser;
    iron_thread_t        thread;
    bool                 started;
    atomic_bool          shutdown;
};

static void reader_pump_remaining(IronLsp_Reader *r) {
    /* After ingesting a chunk, try to drain all complete messages that are
     * now available in the framer buffer. */
    for (;;) {
        const char *body = NULL;
        size_t len = 0;
        IronLsp_FrameResult fr =
            ilsp_frame_feed(&r->parser, NULL, 0, &body, &len);
        if (fr == ILSP_FRAME_RESULT_COMPLETE) {
            if (r->on_message) r->on_message(r->ctx, body, len);
            ilsp_frame_consume(&r->parser);
            continue;
        }
        if (fr == ILSP_FRAME_RESULT_NEED_MORE) return;
        /* Framing error: fatal -- log and exit pump. */
        fprintf(stderr, "ironls: frame error %d -- aborting reader\n",
                (int)fr);
        atomic_store(&r->shutdown, true);
        return;
    }
}

static void *reader_thread_main(void *arg) {
    IronLsp_Reader *r = (IronLsp_Reader *)arg;
    char chunk[ILSP_READER_CHUNK_SIZE];

    while (!atomic_load(&r->shutdown)) {
        size_t n = fread(chunk, 1, sizeof(chunk), r->source);
        if (n > 0) {
            const char *body = NULL;
            size_t len = 0;
            IronLsp_FrameResult fr =
                ilsp_frame_feed(&r->parser, chunk, n, &body, &len);
            if (fr == ILSP_FRAME_RESULT_COMPLETE) {
                if (r->on_message) r->on_message(r->ctx, body, len);
                ilsp_frame_consume(&r->parser);
                reader_pump_remaining(r);
            } else if (fr == ILSP_FRAME_RESULT_NEED_MORE) {
                /* Keep reading. */
            } else {
                fprintf(stderr, "ironls: frame error %d -- aborting reader\n",
                        (int)fr);
                break;
            }
        }
        if (n < sizeof(chunk)) {
            /* Short read: either EOF, error, or non-blocking would-block.
             * For a blocking FILE* (stdin, fmemopen), feof/ferror are the
             * definitive signals. */
            if (feof(r->source) || ferror(r->source)) break;
        }
    }
    return NULL;
}

IronLsp_Reader *ilsp_reader_create(FILE *source,
                                   IronLsp_OnMessage on_message,
                                   void *ctx) {
    IronLsp_Reader *r = (IronLsp_Reader *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->source     = source;
    r->on_message = on_message;
    r->ctx        = ctx;
    r->started    = false;
    atomic_store(&r->shutdown, false);
    ilsp_frame_init(&r->parser);
    return r;
}

void ilsp_reader_destroy(IronLsp_Reader *r) {
    if (!r) return;
    ilsp_frame_destroy(&r->parser);
    free(r);
}

void ilsp_reader_start(IronLsp_Reader *r) {
    if (!r || r->started) return;
    r->started = true;
    IRON_THREAD_CREATE(r->thread, reader_thread_main, r);
}

void ilsp_reader_shutdown(IronLsp_Reader *r) {
    if (!r) return;
    atomic_store(&r->shutdown, true);
}

void ilsp_reader_join(IronLsp_Reader *r) {
    if (!r || !r->started) return;
    IRON_THREAD_JOIN(r->thread);
    r->started = false;
}
