/* emit_web.h — WEB-EMIT-05/06/07/08 emitter for --target=web.
 *
 * Produces C source for the web target: emits #include <emscripten/emscripten.h>,
 * a frame-state struct for the main-loop function (driven by Phase 5's
 * fn->web_frame_captures), a frame callback that rewrites captured-local
 * accesses through a state struct pointer, a main() wrapper that allocates
 * the state and calls emscripten_set_main_loop_arg(frame_cb, state, 0, 0),
 * and a shutdown path inside the frame callback invoking CloseWindow(),
 * iron_runtime_shutdown(), free(state), and emscripten_cancel_main_loop().
 *
 * Signature mirrors iron_lir_emit_c() byte-for-byte so the dispatch in
 * src/cli/build.c (Plan 03) is a single if/else on opts.target.
 *
 * Returns arena-allocated C string. NULL on error.
 */
#ifndef IRON_LIR_EMIT_WEB_H
#define IRON_LIR_EMIT_WEB_H

#include "lir/lir.h"
#include "lir/lir_optimize.h"
#include "analyzer/iface_collect.h"

const char *emit_web_module(IronLIR_Module *module, Iron_Arena *arena,
                            Iron_DiagList *diags,
                            IronLIR_OptimizeInfo *opt_info,
                            Iron_IfaceRegistry *iface_reg,
                            bool warn_fusion_break,
                            bool report_compression);

#endif /* IRON_LIR_EMIT_WEB_H */
