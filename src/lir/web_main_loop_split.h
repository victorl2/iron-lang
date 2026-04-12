/* web_main_loop_split.h — WEB-EMIT-01..04 LIR pass.
 *
 * Detects the canonical `while (!WindowShouldClose()) { body }` shape inside
 * any function that also contains a direct call to `InitWindow()` when the
 * build target is IRON_TARGET_WEB, and populates the target function's
 * `web_frame_captures` / `web_frame_capture_count` metadata fields with the
 * outer-scope locals that the loop body reads or writes (capture-by-reference
 * semantics at Phase 6 emit time).
 *
 * On IRON_TARGET_NATIVE this pass is a zero-cost early return.
 *
 * Non-canonical shapes emit one of four errors from diagnostics.h:
 *   IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS       (700) — >=2 canonical candidates
 *   IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP   (701) — for/compound/wrong-cond
 *   IRON_ERR_WEB_NESTED_MAIN_LOOP          (702) — canonical inside a parent loop
 *   IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION  (703) — canonical in non-InitWindow fn
 *
 * Pipeline slot (Plan 03): called from src/cli/build.c between
 * iron_lir_optimize and iron_lir_emit_c, with opts.target supplied by the
 * caller. The current Phase 2 iron_build_web stub does NOT call this pass —
 * Phase 6's emit_web.c rewrite of iron_build_web will wire it in on the
 * --target=web path. For now, on --target=native the native pipeline call
 * is a no-op early return, and on --target=web the stub build_web.c bypasses
 * the LIR pipeline entirely, so Plan 03's verification is unit-test driven.
 *
 * This pass is metadata-only: it does NOT rewrite any LIR instructions.
 * Phase 6 reads `fn->web_frame_captures` at emit time and applies the
 * ALLOCA -> state->field rewrite using the existing capture_alias_map
 * machinery in emit_c.c.
 */
#ifndef IRON_LIR_WEB_MAIN_LOOP_SPLIT_H
#define IRON_LIR_WEB_MAIN_LOOP_SPLIT_H

#include "lir/lir.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "cli/build.h"

void iron_lir_web_main_loop_split(IronLIR_Module *module,
                                  Iron_Arena      *arena,
                                  Iron_DiagList   *diags,
                                  IronBuildTarget  target);

#endif /* IRON_LIR_WEB_MAIN_LOOP_SPLIT_H */
