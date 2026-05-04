#ifndef IRON_LSP_H
#define IRON_LSP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Umbrella header for iron-lsp. Kept thin intentionally:
 * only forward declarations that every subsystem may pull in.
 * Phase 2 Plan 01 skeleton — populated incrementally by later plans
 * (02-02 transport, 02-03 dispatch, 02-04 document store, etc.). */

struct IronLsp_Document;
struct IronLsp_Dispatcher;
struct IronLsp_FrameParser;

#endif /* IRON_LSP_H */
