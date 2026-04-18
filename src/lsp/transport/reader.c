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

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* read(2) */

#include "runtime/iron_runtime.h"  /* IRON_THREAD_* */
#include "lsp/transport/frame.h"
#include "lsp/obs/log.h"           /* Plan 06: EOF/ferror logging */

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
        ilsp_log(ILSP_LOG_ERROR, "reader-frame-error",
                 "frame error %d; aborting reader", (int)fr);
        atomic_store(&r->shutdown, true);
        return;
    }
}

/* Read up to `cap` bytes into `chunk` from the reader's source. Returns
 * >0 byte count on success, 0 on EOF, -1 on error.
 *
 * Implementation note (Plan 06): `fread(chunk, 1, cap, FILE*)` BLOCKS
 * on a blocking FILE* until it has accumulated `cap` bytes OR hits
 * EOF/error -- that's fine for an fmemopen'd buffer (EOF arrives
 * immediately) but catastrophic on a real pipe: the LSP client writes
 * ~300 bytes per request, fread asks for 4096, and the thread sits
 * forever. We take the underlying fd and call read(2) directly which
 * returns as soon as any bytes arrive; fmemopen/open_memstream
 * streams don't expose a valid fd from fileno(), so we fall back to
 * fread in that case. */
static ssize_t reader_pull(IronLsp_Reader *r, char *chunk, size_t cap) {
    int fd = fileno(r->source);
    if (fd >= 0) {
        ssize_t rn;
        do {
            rn = read(fd, chunk, cap);
        } while (rn < 0 && errno == EINTR);
        return rn;
    }
    /* In-memory FILE*: cheat via fread -- fmemopen/open_memstream hit
     * EOF once the internal buffer is drained, so fread will not block. */
    size_t fn = fread(chunk, 1, cap, r->source);
    if (fn > 0) return (ssize_t)fn;
    if (feof(r->source))   return 0;
    if (ferror(r->source)) return -1;
    return 0;
}

static void *reader_thread_main(void *arg) {
    IronLsp_Reader *r = (IronLsp_Reader *)arg;
    char chunk[ILSP_READER_CHUNK_SIZE];

    while (!atomic_load(&r->shutdown)) {
        ssize_t n = reader_pull(r, chunk, sizeof(chunk));
        if (n > 0) {
            const char *body = NULL;
            size_t len = 0;
            IronLsp_FrameResult fr =
                ilsp_frame_feed(&r->parser, chunk, (size_t)n, &body, &len);
            if (fr == ILSP_FRAME_RESULT_COMPLETE) {
                if (r->on_message) r->on_message(r->ctx, body, len);
                ilsp_frame_consume(&r->parser);
                reader_pump_remaining(r);
            } else if (fr == ILSP_FRAME_RESULT_NEED_MORE) {
                /* Keep reading. */
            } else {
                ilsp_log(ILSP_LOG_ERROR, "reader-frame-error",
                         "frame error %d; aborting reader", (int)fr);
                atomic_store(&r->shutdown, true);
                break;
            }
            continue;
        }
        /* n <= 0: Plan 06 (CORE-19). EOF on stdin means the parent
         * editor closed our input; main.c joins writer + workers and
         * exits. A real read error (-1) is handled the same way. */
        if (n == 0) {
            ilsp_log(ILSP_LOG_INFO, "reader-eof",
                     "stdin closed; initiating shutdown");
        } else {
            ilsp_log(ILSP_LOG_ERROR, "reader-error",
                     "stdin read error (errno=%d); initiating shutdown",
                     errno);
        }
        atomic_store(&r->shutdown, true);
        break;
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
