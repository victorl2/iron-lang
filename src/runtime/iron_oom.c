/* iron_oom.c — Out-of-memory abort helper (FIX-01, Phase 67)
 *
 * Canonical abort path for unrecoverable OOM in contexts with no error
 * channel: runtime macros (IRON_LIST/MAP/SET _push/_put/_add/_clone/
 * _create_with_capacity), generated C code from emit_c.c (HEAP_ALLOC /
 * RC_ALLOC / closure env / parallel-for ctx / boxed ADT), and
 * compiler-internal allocation paths where malloc failure is fatal.
 *
 * Declaration lives in src/diagnostics/diagnostics.h adjacent to iron_ice
 * so the diagnostics surface exposes a single "how we die on an internal
 * fault" API. The definition lives here (inside iron_runtime) rather than
 * in diagnostics.c because:
 *
 *   1. iron_runtime is the lowest link layer — every generated user
 *      binary, every runtime unit test, and the compiler itself all
 *      link against it. Placing the definition here means every
 *      downstream consumer resolves the symbol with no extra wiring.
 *
 *   2. diagnostics.c has transitive dependencies on parser/ast.h and
 *      stb_ds, which would pull the entire compiler surface into the
 *      runtime link layer if we put the definition there.
 *
 *   3. Per 67-CONTEXT.md FIX-01 locked decision, OOM is handled via an
 *      abort helper (not via a fallible error channel), so the impl is
 *      trivial and does not need the full iron_diag_emit plumbing.
 *
 * Do NOT conflate with iron_ice (src/diagnostics/diagnostics.c). iron_ice
 * reports compiler bugs / unreachable paths; iron_oom_abort reports a
 * legitimate runtime resource-exhaustion fault. Downstream telemetry can
 * distinguish the two by the distinct stderr prefixes.
 */

#include "diagnostics/diagnostics.h"

#include <stdio.h>
#include <stdlib.h>

void iron_oom_abort(const char *where) {
    fprintf(stderr, "iron: out of memory at %s\n",
            where ? where : "<unknown>");
    fflush(stderr);
    abort();
}
