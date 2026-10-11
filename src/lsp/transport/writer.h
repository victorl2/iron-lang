#ifndef IRON_LSP_TRANSPORT_WRITER_H
#define IRON_LSP_TRANSPORT_WRITER_H

/* Writer queue and drop policy (#589).
 *
 * The writer thread pulls items off a ring buffer and serializes them to a
 * caller-provided FILE* (stdout in production, an open_memstream buffer in
 * tests). Responses are never dropped and notifications never lost: a
 * textDocument/publishDiagnostics replaces a queued one for the same
 * document (OK_DROPPED_NOTIFICATION), and past ILSP_WRITER_QUEUE_CAPACITY
 * items the ring grows instead of dropping them. Only logs are shed then:
 * the oldest log goes (OK_DROPPED_LOG), or a new log is refused with
 * FULL_DROPPED when no log is queued (FULL_DROPPED also on out of memory).
 *
 * The writer takes ownership of `body` on a non-FULL_DROPPED return. On
 * FULL_DROPPED, the writer has free()'d the body (caller MUST NOT use). */

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

#include "lsp/transport/types.h"

#define ILSP_WRITER_QUEUE_CAPACITY 256

typedef struct IronLsp_Writer IronLsp_Writer;

typedef enum IronLsp_EnqueueResult {
    ILSP_ENQUEUE_OK,
    ILSP_ENQUEUE_OK_DROPPED_LOG,
    ILSP_ENQUEUE_OK_DROPPED_NOTIFICATION,
    ILSP_ENQUEUE_FULL_DROPPED
} IronLsp_EnqueueResult;

IronLsp_Writer *ilsp_writer_create(FILE *sink);
void            ilsp_writer_destroy(IronLsp_Writer *w);

/* Spawn the writer thread. After start(), all enqueues will be drained
 * asynchronously by the thread. Without start(), callers can use
 * ilsp_writer_drain_one synchronously (primarily for tests). */
void ilsp_writer_start(IronLsp_Writer *w);

/* Signal the writer thread to drain its queue and exit. */
void ilsp_writer_shutdown(IronLsp_Writer *w);

/* Join the writer thread. Must be called after shutdown(). */
void ilsp_writer_join(IronLsp_Writer *w);

/* Enqueue an outbound message. The writer takes ownership of `body` on
 * any non-FULL_DROPPED result (including OK_DROPPED_LOG / OK_DROPPED_N).
 * On FULL_DROPPED, `body` is free()d by this call. */
IronLsp_EnqueueResult ilsp_writer_enqueue(IronLsp_Writer *w,
                                          IronLsp_Priority prio,
                                          char *body, size_t len);

/* Synchronously dequeue and write one item on the calling thread. Returns
 * false if the queue is empty. Used by tests; never called from a thread
 * that has also called ilsp_writer_start() on the same writer. */
bool ilsp_writer_drain_one(IronLsp_Writer *w);

#endif /* IRON_LSP_TRANSPORT_WRITER_H */
