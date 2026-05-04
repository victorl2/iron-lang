#ifndef IRON_LSP_TRANSPORT_WRITER_H
#define IRON_LSP_TRANSPORT_WRITER_H

/* Phase 2 Plan 02 Task 03 -- Bounded writer queue + drop policy.
 *
 * The writer thread pulls items off a fixed-capacity (256) ring buffer and
 * serializes them to a caller-provided FILE* (stdout in production, an
 * open_memstream buffer in tests). Drop policy (T-02-04 DoS mitigation):
 * when the queue is full, enqueueing drops the oldest ILSP_PRIO_LOG item;
 * if none, the oldest ILSP_PRIO_NOTIFICATION; if the queue is all
 * responses, the new item is rejected with FULL_DROPPED so the caller can
 * decide to retry or surface an internalError.
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
