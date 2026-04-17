#ifndef IRON_LSP_SERVER_DISPATCH_H
#define IRON_LSP_SERVER_DISPATCH_H

#include <stddef.h>

/* Dispatch-layer skeleton. Plan 02-03 (lifecycle + cancel + caps) fills
 * in the dispatcher struct and method-routing table. */

struct IronLsp_Dispatcher;

typedef void (*IronLsp_HandlerFn)(void *ctx,
                                  const char *params_json,
                                  size_t len);

#endif /* IRON_LSP_SERVER_DISPATCH_H */
