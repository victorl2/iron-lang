/* iron-lsp entry point.
 *
 * Phase 2 Plan 01 shipped a banner-and-exit stub so the CMake target
 * existed. Phase 2 Plan 03 fleshes out main() to construct the shared
 * IronLsp_Server struct (dispatcher, lifecycle FSM, cancel registry,
 * dyn-register scaffold) so the pieces from Plans 02+03 compose into a
 * single startable singleton. The stdin reader thread + main dispatch
 * loop are deliberately NOT started here -- that wiring lands in
 * Plan 06 along with SIGPIPE handling and XDG log-sink setup. For now
 * the binary still exits cleanly after the banner so the Plan 01
 * smoke tests continue to pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lsp/server/server.h"
#include "lsp/server/dispatch.h"
#include "lsp/server/lifecycle.h"
#include "lsp/server/cancel.h"
#include "lsp/server/dyn_register.h"
#include "lsp/transport/writer.h"
#include "lsp/facade/types.h"

#ifndef IRON_VERSION_STRING
#define IRON_VERSION_STRING "0.0.0"
#endif
#ifndef IRON_GIT_HASH
#define IRON_GIT_HASH "unknown"
#endif
#ifndef IRON_BUILD_DATE
#define IRON_BUILD_DATE "unknown"
#endif
#ifndef IRON_BINARY_NAME
#define IRON_BINARY_NAME "ironls"
#endif

extern IronLsp_DynRegister *ilsp_dyn_register_create(void);
extern void                 ilsp_dyn_register_destroy(IronLsp_DynRegister *r);

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 ||
                      strcmp(argv[1], "-v") == 0)) {
        printf("%s %s (%s, %s)\n",
               IRON_BINARY_NAME,
               IRON_VERSION_STRING,
               IRON_GIT_HASH,
               IRON_BUILD_DATE);
        return 0;
    }

    fprintf(stderr,
            "%s: iron language server (LSP 3.17)\n",
            IRON_BINARY_NAME);

    /* Phase 2 Plan 03 -- construct the server singleton so the
     * dispatcher + lifecycle + cancel registry are wired and greppable
     * at runtime. The reader/writer threads are NOT started here (Plan
     * 06 wires those). */
    IronLsp_Server server;
    memset(&server, 0, sizeof(server));
    server.lifecycle         = ILSP_LIFECYCLE_UNINIT;
    server.writer            = ilsp_writer_create(stdout);
    server.reader            = NULL;   /* Plan 06 */
    server.cancels           = ilsp_cancel_registry_create();
    server.dyn_reg           = ilsp_dyn_register_create();
    server.position_encoding = ILSP_ENC_UTF16;
    atomic_store(&server.next_request_id, 1);

    fprintf(stderr,
            "%s: dispatcher loaded (%zu handlers)\n",
            IRON_BINARY_NAME, ilsp_handler_table_size);
    fprintf(stderr,
            "%s: Phase 2 Plan 03 -- reader loop arrives in Plan 06\n",
            IRON_BINARY_NAME);

    /* Tear down cleanly so valgrind/ASan stay green. */
    ilsp_writer_destroy(server.writer);
    ilsp_cancel_registry_destroy(server.cancels);
    ilsp_dyn_register_destroy(server.dyn_reg);
    return 0;
}
