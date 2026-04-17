#ifndef IRON_LSP_TRANSPORT_READER_H
#define IRON_LSP_TRANSPORT_READER_H

/* Phase 2 Plan 02 Task 03 -- stdin reader thread.
 *
 * The reader owns an IronLsp_FrameParser and pulls bytes off a FILE* source
 * (stdin in production, fmemopen'd buffer in tests). On each COMPLETE
 * frame it invokes a caller-provided on_message callback on the reader
 * thread. Shutdown is either:
 *   - Natural: the source reaches feof (parent editor closed stdin).
 *   - Explicit: ilsp_reader_shutdown sets the atomic flag and the thread
 *     exits at its next read boundary.
 *
 * Dispatch (Plan 03) layers on top of this -- the on_message callback
 * will hand the body to the JSON parser and route to a handler. */

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct IronLsp_Reader IronLsp_Reader;

typedef void (*IronLsp_OnMessage)(void *ctx, const char *body, size_t len);

IronLsp_Reader *ilsp_reader_create(FILE *source,
                                   IronLsp_OnMessage on_message,
                                   void *ctx);
void            ilsp_reader_destroy(IronLsp_Reader *r);

/* Spawn the reader thread. */
void ilsp_reader_start(IronLsp_Reader *r);

/* Signal the reader thread to stop at its next read boundary. */
void ilsp_reader_shutdown(IronLsp_Reader *r);

/* Join the reader thread. Returns once the thread has exited (either
 * because of feof on the source or explicit shutdown). */
void ilsp_reader_join(IronLsp_Reader *r);

#endif /* IRON_LSP_TRANSPORT_READER_H */
