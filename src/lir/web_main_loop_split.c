/* web_main_loop_split.c — WEB-EMIT-01..04 LIR pass implementation.
 *
 * See web_main_loop_split.h for the public contract. This file is metadata-
 * only: it populates fn->web_frame_captures and emits diagnostics, but it
 * does NOT rewrite any LIR instructions. Phase 6's emit_web.c consumes the
 * metadata to synthesize the frame callback + state struct at C emission.
 *
 * The dominator and natural-loop helpers are copied (with `ws_` prefix) from
 * src/lir/lir_optimize.c where they are static. Duplication avoids exposing
 * internal optimizer helpers through lir_optimize.h, which would ripple
 * through every other pass that includes it.
 */

#include "lir/web_main_loop_split.h"
#include "lir/lir_optimize.h"  /* IronLIR_DomEntry, IronLIR_LoopInfo, IronLIR_LoopMemberEntry types */
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* ── Internal block lookup ────────────────────────────────────────────────── */

static IronLIR_Block *ws_find_block(IronLIR_Func *fn, IronLIR_BlockId id) {
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) return fn->blocks[i];
    }
    return NULL;
}

/* ── Reverse post-order (RPO) ─────────────────────────────────────────────── */

/* Build reverse post-order (RPO) of blocks by DFS from entry (fn->blocks[0]).
 * Returns a stb_ds array of BlockIds in RPO order. Caller must arrfree(). */
static IronLIR_BlockId *ws_build_rpo(IronLIR_Func *fn) {
    if (fn->block_count == 0) return NULL;

    /* Visited set: BlockId -> bool */
    struct { IronLIR_BlockId key; bool value; } *visited = NULL;
    /* Post-order accumulator (we'll reverse it) */
    IronLIR_BlockId *postorder = NULL;

    /* Iterative DFS using an explicit stack */
    typedef struct { IronLIR_BlockId bid; int succ_idx; } ws_StackFrame;
    ws_StackFrame *stack = NULL;

    IronLIR_BlockId entry_id = fn->blocks[0]->id;
    ws_StackFrame sf; sf.bid = entry_id; sf.succ_idx = 0;
    arrput(stack, sf);
    hmput(visited, entry_id, true);

    while (arrlen(stack) > 0) {
        ws_StackFrame *top = &stack[arrlen(stack) - 1];
        IronLIR_Block *blk = ws_find_block(fn, top->bid);
        if (!blk || top->succ_idx >= (int)arrlen(blk->succs)) {
            /* All successors visited — emit this block to post-order */
            arrput(postorder, top->bid);
            arrpop(stack);
        } else {
            IronLIR_BlockId succ_id = blk->succs[top->succ_idx++];
            if (hmgeti(visited, succ_id) < 0) {
                hmput(visited, succ_id, true);
                ws_StackFrame nsf; nsf.bid = succ_id; nsf.succ_idx = 0;
                arrput(stack, nsf);
            }
        }
    }

    /* Reverse post-order = reverse of postorder array */
    IronLIR_BlockId *rpo = NULL;
    for (int i = (int)arrlen(postorder) - 1; i >= 0; i--) {
        arrput(rpo, postorder[i]);
    }

    hmfree(visited);
    arrfree(postorder);
    arrfree(stack);
    return rpo;
}

/* ── Dominator tree (copied from lir_optimize.c with ws_ prefix) ──────────── */

/* Build dominator tree using the iterative algorithm.
 * Returns a stb_ds hashmap: BlockId -> idom BlockId.
 * Caller must hmfree() the result. */
static IronLIR_DomEntry *ws_build_domtree(IronLIR_Func *fn) {
    if (fn->block_count == 0) return NULL;

    IronLIR_DomEntry *idom = NULL;
    IronLIR_BlockId entry_id = fn->blocks[0]->id;

    /* Build RPO for traversal order */
    IronLIR_BlockId *rpo = ws_build_rpo(fn);
    int rpo_len = (int)arrlen(rpo);
    if (rpo_len == 0) { arrfree(rpo); return NULL; }

    /* Build RPO index map: BlockId -> position in RPO (for intersection walk) */
    struct { IronLIR_BlockId key; int value; } *rpo_idx = NULL;
    for (int i = 0; i < rpo_len; i++) {
        hmput(rpo_idx, rpo[i], i);
    }

    /* Initialize: idom[entry] = entry, all others undefined */
    hmput(idom, entry_id, entry_id);

    bool changed = true;
    while (changed) {
        changed = false;
        /* Process blocks in RPO order (skip entry) */
        for (int ri = 1; ri < rpo_len; ri++) {
            IronLIR_BlockId bid = rpo[ri];
            IronLIR_Block *blk  = ws_find_block(fn, bid);
            if (!blk) continue;

            /* Find first processed predecessor */
            IronLIR_BlockId new_idom = 0;
            bool found_first = false;

            for (int pi = 0; pi < (int)arrlen(blk->preds); pi++) {
                IronLIR_BlockId pred = blk->preds[pi];
                if (hmgeti(idom, pred) >= 0) {
                    /* This predecessor has been processed */
                    if (!found_first) {
                        new_idom = pred;
                        found_first = true;
                    } else {
                        /* Intersection: walk up from both until they meet */
                        IronLIR_BlockId a = new_idom;
                        IronLIR_BlockId b = pred;
                        while (a != b) {
                            /* Walk the one with higher RPO index (further from entry) up */
                            ptrdiff_t ai = hmgeti(rpo_idx, a);
                            ptrdiff_t bi = hmgeti(rpo_idx, b);
                            int a_pos = (ai >= 0) ? rpo_idx[ai].value : rpo_len;
                            int b_pos = (bi >= 0) ? rpo_idx[bi].value : rpo_len;
                            while (a_pos > b_pos) {
                                ptrdiff_t idom_idx = hmgeti(idom, a);
                                if (idom_idx < 0) break;
                                a = idom[idom_idx].value;
                                ptrdiff_t ai2 = hmgeti(rpo_idx, a);
                                a_pos = (ai2 >= 0) ? rpo_idx[ai2].value : rpo_len;
                            }
                            while (b_pos > a_pos) {
                                ptrdiff_t idom_idx = hmgeti(idom, b);
                                if (idom_idx < 0) break;
                                b = idom[idom_idx].value;
                                ptrdiff_t bi2 = hmgeti(rpo_idx, b);
                                b_pos = (bi2 >= 0) ? rpo_idx[bi2].value : rpo_len;
                            }
                        }
                        new_idom = a;
                    }
                }
            }

            if (found_first) {
                ptrdiff_t cur_idx = hmgeti(idom, bid);
                if (cur_idx < 0 || idom[cur_idx].value != new_idom) {
                    hmput(idom, bid, new_idom);
                    changed = true;
                }
            }
        }
    }

    hmfree(rpo_idx);
    arrfree(rpo);
    return idom;
}

/* Returns true if block 'a' dominates block 'b' in the given idom tree. */
static bool ws_dominates(IronLIR_DomEntry *idom, IronLIR_BlockId a, IronLIR_BlockId b) {
    IronLIR_BlockId cur = b;
    int limit = 1000; /* cycle guard */
    while (limit-- > 0) {
        if (cur == a) return true;
        ptrdiff_t idx = hmgeti(idom, cur);
        if (idx < 0) return false;
        IronLIR_BlockId parent = idom[idx].value;
        if (parent == cur) return (cur == a); /* reached root */
        cur = parent;
    }
    return false;
}

/* ── Natural loop builder (copied from lir_optimize.c with ws_ prefix) ────── */

/* Build loop info for all natural loops in a function.
 * Returns a stb_ds array of IronLIR_LoopInfo structs.
 * Caller must free: for each loop, hmfree(loop.body_blocks); then arrfree(loops). */
static IronLIR_LoopInfo *ws_build_loop_info(IronLIR_Func *fn, IronLIR_DomEntry *idom,
                                             int *loop_count_out) {
    IronLIR_LoopInfo *loops = NULL;
    *loop_count_out = 0;
    if (!idom || fn->block_count == 0) return NULL;

    /* Find all back edges: b -> s where s dominates b */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int si = 0; si < (int)arrlen(blk->succs); si++) {
            IronLIR_BlockId succ_id = blk->succs[si];
            /* Check if succ dominates blk -> back edge */
            if (!ws_dominates(idom, succ_id, blk->id)) continue;

            /* Found back edge: blk is latch, succ_id is header */
            IronLIR_LoopInfo loop;
            memset(&loop, 0, sizeof(loop));
            loop.header    = succ_id;
            loop.latch     = blk->id;
            loop.preheader = 0;
            loop.body_blocks = NULL;
            loop.indvar_alloca = IRON_LIR_VALUE_INVALID;
            loop.indvar_step   = IRON_LIR_VALUE_INVALID;
            loop.indvar_init   = IRON_LIR_VALUE_INVALID;
            loop.parent = NULL;

            /* Collect loop body: all blocks from which latch is reachable
             * without going through the header. Start from latch, walk preds. */
            hmput(loop.body_blocks, succ_id, true);   /* header is in the loop */
            hmput(loop.body_blocks, blk->id, true);   /* latch is in the loop */

            /* Worklist: blocks to process */
            IronLIR_BlockId *worklist = NULL;
            arrput(worklist, blk->id);

            while (arrlen(worklist) > 0) {
                IronLIR_BlockId cur = worklist[arrlen(worklist) - 1];
                arrpop(worklist);
                IronLIR_Block *cur_blk = ws_find_block(fn, cur);
                if (!cur_blk) continue;

                for (int pi = 0; pi < (int)arrlen(cur_blk->preds); pi++) {
                    IronLIR_BlockId pred_id = cur_blk->preds[pi];
                    if (hmgeti(loop.body_blocks, pred_id) < 0) {
                        hmput(loop.body_blocks, pred_id, true);
                        if (pred_id != succ_id) { /* don't go above header */
                            arrput(worklist, pred_id);
                        }
                    }
                }
            }
            arrfree(worklist);

            arrput(loops, loop);
            (*loop_count_out)++;
        }
    }

    /* Build nested loop tree: if header H1 of loop L1 is in body of loop L2,
     * then L1.parent = &L2 */
    for (int i = 0; i < *loop_count_out; i++) {
        for (int j = 0; j < *loop_count_out; j++) {
            if (i == j) continue;
            /* Is loops[i].header in loops[j].body_blocks? */
            if (hmgeti(loops[j].body_blocks, loops[i].header) >= 0 &&
                loops[i].header != loops[j].header) {
                /* loops[i] is nested inside loops[j] */
                if (!loops[i].parent) {
                    loops[i].parent = &loops[j];
                }
            }
        }
    }

    return loops;
}

/* Free loop array and all body_block maps. */
static void ws_free_loops(IronLIR_LoopInfo *loops, int count) {
    for (int i = 0; i < count; i++) {
        hmfree(loops[i].body_blocks);
    }
    arrfree(loops);
}

/* ── Predicate helpers ────────────────────────────────────────────────────── */

/* True iff `instr` is a CALL to the function named `name` with the given arg count. */
static bool ws_is_call_to(const IronLIR_Instr *instr, const char *name, int expected_arg_count) {
    if (!instr || instr->kind != IRON_LIR_CALL) return false;
    if (!instr->call.func_decl) return false;
    if (!instr->call.func_decl->name) return false;
    if (strcmp(instr->call.func_decl->name, name) != 0) return false;
    if (instr->call.arg_count != expected_arg_count) return false;
    return true;
}

/* Scan every instruction in every block of `fn` and return true iff any is a
 * direct call to InitWindow (3 args). */
static bool ws_function_calls_init_window(IronLIR_Func *fn) {
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            if (ws_is_call_to(blk->instrs[ii], "InitWindow", 3)) return true;
        }
    }
    return false;
}

/* Resolve a ValueId to its defining Instr via fn->value_table. Returns NULL
 * on out-of-range or missing. */
static IronLIR_Instr *ws_value_def(IronLIR_Func *fn, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return NULL;
    if ((ptrdiff_t)vid >= arrlen(fn->value_table)) return NULL;
    return fn->value_table[vid];
}

/* True iff `header_blk`'s terminator is a BRANCH whose condition traces to
 * NOT(CALL(WindowShouldClose, 0 args)).
 *
 * Structural match (no alternate spellings allowed — compound conds, equality
 * tests, variable-captured predicates all return false, producing a
 * IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP at the call site). */
static bool ws_header_is_canonical(IronLIR_Func *fn, IronLIR_Block *header_blk) {
    if (!header_blk || header_blk->instr_count == 0) return false;
    IronLIR_Instr *term = header_blk->instrs[header_blk->instr_count - 1];
    if (!term || term->kind != IRON_LIR_BRANCH) return false;

    IronLIR_Instr *cond_def = ws_value_def(fn, term->branch.cond);
    if (!cond_def || cond_def->kind != IRON_LIR_NOT) return false;

    IronLIR_Instr *inner = ws_value_def(fn, cond_def->unop.operand);
    if (!inner) return false;
    return ws_is_call_to(inner, "WindowShouldClose", 0);
}

/* ── Capture-set computation ──────────────────────────────────────────────── */

/* Record of an alloca's defining block (computed once per function and
 * reused across capture scans).
 * Keyed by IronLIR_ValueId (the alloca's result value id). */
typedef struct { IronLIR_ValueId key; IronLIR_BlockId value; } ws_AllocaBlockEntry;

/* Build a map: alloca value id -> defining block id. Walks every block and
 * records every IRON_LIR_ALLOCA. Caller must hmfree the returned map. */
static ws_AllocaBlockEntry *ws_build_alloca_block_map(IronLIR_Func *fn) {
    ws_AllocaBlockEntry *map = NULL;
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *ins = blk->instrs[ii];
            if (ins && ins->kind == IRON_LIR_ALLOCA) {
                hmput(map, ins->id, blk->id);
            }
        }
    }
    return map;
}

/* Capture-set entry used internally before arena-copying into Iron_CaptureEntry. */
typedef struct {
    IronLIR_ValueId alloca_id;
    const char     *name_hint;
    Iron_Type      *alloc_type;
    bool            is_mutable;
} ws_CaptureTmp;

/* Helper: is `blk_id` in the loop's body_blocks set? */
static bool ws_block_in_loop(IronLIR_LoopInfo *loop, IronLIR_BlockId blk_id) {
    return hmgeti(loop->body_blocks, blk_id) >= 0;
}

/* Walk every LOAD and STORE inside the loop body. For each, resolve the ptr
 * operand to its defining alloca. If the alloca's defining block is OUTSIDE
 * the loop body, record it as a captured local. Mark `is_mutable = true` iff
 * any STORE inside the loop body targets that alloca.
 *
 * Returns a stb_ds array of ws_CaptureTmp; caller arrfree's it after copying
 * into fn->web_frame_captures. */
static ws_CaptureTmp *ws_compute_loop_captures(IronLIR_Func *fn,
                                                IronLIR_LoopInfo *loop,
                                                ws_AllocaBlockEntry *alloca_blocks) {
    /* dedup map: alloca value id -> index in out[] */
    struct { IronLIR_ValueId key; int value; } *seen = NULL;
    ws_CaptureTmp *out = NULL;

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        if (!ws_block_in_loop(loop, blk->id)) continue;

        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *ins = blk->instrs[ii];
            if (!ins) continue;

            IronLIR_ValueId ptr_vid = IRON_LIR_VALUE_INVALID;
            bool is_store = false;
            if (ins->kind == IRON_LIR_LOAD)  ptr_vid = ins->load.ptr;
            else if (ins->kind == IRON_LIR_STORE) { ptr_vid = ins->store.ptr; is_store = true; }
            else continue;

            IronLIR_Instr *def = ws_value_def(fn, ptr_vid);
            if (!def || def->kind != IRON_LIR_ALLOCA) continue;

            /* Is the alloca's defining block INSIDE the loop? If so, it's a
             * local-to-the-loop — not a capture. */
            ptrdiff_t idx = hmgeti(alloca_blocks, def->id);
            if (idx < 0) continue; /* not in map — defensive */
            IronLIR_BlockId alloca_blk = alloca_blocks[idx].value;
            if (ws_block_in_loop(loop, alloca_blk)) continue;

            /* Outer-scope alloca — this is a capture. */
            ptrdiff_t seen_idx = hmgeti(seen, def->id);
            if (seen_idx < 0) {
                ws_CaptureTmp tmp;
                tmp.alloca_id  = def->id;
                tmp.name_hint  = def->alloca.name_hint ? def->alloca.name_hint : "(anon)";
                tmp.alloc_type = def->alloca.alloc_type;
                tmp.is_mutable = is_store;
                int new_idx = (int)arrlen(out);
                arrpush(out, tmp);
                hmput(seen, def->id, new_idx);
            } else if (is_store) {
                int existing = seen[seen_idx].value;
                out[existing].is_mutable = true;
            }
        }
    }

    hmfree(seen);
    return out;
}

/* ── Error emission ───────────────────────────────────────────────────────── */

/* Emit one of the Phase 5 errors with a uniform message template. */
static void ws_emit_err(Iron_DiagList *diags, Iron_Arena *arena,
                        int code, Iron_Span span,
                        const char *fn_name, const char *canonical_fix) {
    char msg[512];
    const char *reason =
        code == IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS
            ? "--target=web supports exactly one top-level while(!WindowShouldClose()) per function"
        : code == IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP
            ? "--target=web requires the canonical while(!WindowShouldClose()) shape; the loop does not match (expected while-header with negated WindowShouldClose() call)"
        : code == IRON_ERR_WEB_NESTED_MAIN_LOOP
            ? "--target=web does not support nested main loops; web builds use Emscripten's cooperative scheduler which requires a single top-level frame loop"
        : code == IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION
            ? "--target=web requires the main loop in the function that calls InitWindow()"
        : "--target=web: non-canonical main loop shape";

    snprintf(msg, sizeof(msg),
             "%s (in function `%s`)\n  canonical form: %s",
             reason,
             fn_name ? fn_name : "(anon)",
             canonical_fix);

    char *msg_copy = iron_arena_strdup(arena, msg, strlen(msg));
    if (!msg_copy) iron_oom_abort("web_main_loop_split.c:ws_emit_err msg");
    iron_diag_emit(diags, arena, IRON_DIAG_ERROR, code, span,
                   msg_copy,
                   NULL);
}

/* Look up the span of a block's first instruction, for error reporting. */
static Iron_Span ws_block_span(IronLIR_Block *blk) {
    Iron_Span z;
    memset(&z, 0, sizeof(z));
    if (!blk || blk->instr_count == 0) return z;
    return blk->instrs[0]->span;
}

/* ── Per-function driver ──────────────────────────────────────────────────── */

/* Process one function. Emits errors into `diags` and, on success, populates
 * fn->web_frame_captures + fn->web_frame_capture_count.
 *
 * Decision table (per CONTEXT.md + planner quality gate):
 *   0 natural loops                                         -> no-op (non-interactive)
 *   1+ canonical candidates (at depth 0) + 0 non-canonical  -> transform (pick the one; error if >1)
 *   0 canonical candidates + 1+ natural loops               -> NON_CANONICAL error on first loop
 *   any canonical candidate whose parent != NULL            -> NESTED error on that loop
 *   2+ canonical candidates at depth 0                      -> MULTIPLE error on the second one
 */
static void ws_process_function(IronLIR_Func *fn, Iron_Arena *arena, Iron_DiagList *diags) {
    if (fn->is_extern) return;
    if (!ws_function_calls_init_window(fn)) return;

    /* Dominator tree + natural loops. */
    IronLIR_DomEntry *idom = ws_build_domtree(fn);
    int loop_count = 0;
    IronLIR_LoopInfo *loops = ws_build_loop_info(fn, idom, &loop_count);

    if (loop_count == 0) {
        /* InitWindow present but no natural loops. User wrote a setup-and-exit
         * program. No transform, no error. */
        goto cleanup;
    }

    /* First pass: classify every loop. */
    int canonical_count = 0;
    int first_canonical_idx = -1;
    int first_nested_canonical_idx = -1;
    int first_non_canonical_idx = -1;

    for (int li = 0; li < loop_count; li++) {
        IronLIR_LoopInfo *loop = &loops[li];
        IronLIR_Block *hdr = ws_find_block(fn, loop->header);
        bool canonical = ws_header_is_canonical(fn, hdr);
        if (canonical) {
            if (loop->parent != NULL) {
                if (first_nested_canonical_idx < 0) first_nested_canonical_idx = li;
            } else {
                canonical_count++;
                if (first_canonical_idx < 0) first_canonical_idx = li;
            }
        } else {
            if (first_non_canonical_idx < 0) first_non_canonical_idx = li;
        }
    }

    /* Error priority (stop-on-first for clearest UX):
     *   nested canonical > multiple canonical > non-canonical with no canonical
     * (A non-canonical loop alongside exactly one canonical top-level loop is
     * NOT an error — users may legitimately have auxiliary while loops like
     * event-drain loops inside the frame body. The frame-body loops are all
     * nested INSIDE the canonical candidate and will have parent != NULL, so
     * they never enter the non-canonical top-level classification above.)
     */
    if (first_nested_canonical_idx >= 0) {
        IronLIR_LoopInfo *loop = &loops[first_nested_canonical_idx];
        IronLIR_Block *hdr = ws_find_block(fn, loop->header);
        ws_emit_err(diags, arena, IRON_ERR_WEB_NESTED_MAIN_LOOP,
                    ws_block_span(hdr), fn->name,
                    "move the while(!WindowShouldClose()) to the top level of the function body");
        goto cleanup;
    }

    if (canonical_count > 1) {
        /* Point at the SECOND canonical candidate so users see the duplicate. */
        int second_idx = -1;
        int seen = 0;
        for (int li = 0; li < loop_count; li++) {
            if (loops[li].parent != NULL) continue;
            IronLIR_Block *hdr = ws_find_block(fn, loops[li].header);
            if (ws_header_is_canonical(fn, hdr)) {
                seen++;
                if (seen == 2) { second_idx = li; break; }
            }
        }
        if (second_idx < 0) second_idx = first_canonical_idx; /* defensive */
        IronLIR_Block *hdr = ws_find_block(fn, loops[second_idx].header);
        ws_emit_err(diags, arena, IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS,
                    ws_block_span(hdr), fn->name,
                    "use a state enum inside one while(!WindowShouldClose())");
        goto cleanup;
    }

    if (canonical_count == 0) {
        /* There are natural loops but none match the canonical shape. Point
         * at the first non-canonical loop. */
        int idx = first_non_canonical_idx >= 0 ? first_non_canonical_idx : 0;
        IronLIR_Block *hdr = ws_find_block(fn, loops[idx].header);
        ws_emit_err(diags, arena, IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP,
                    ws_block_span(hdr), fn->name,
                    "while (!WindowShouldClose()) { /* frame body */ }");
        goto cleanup;
    }

    /* Exactly one canonical top-level candidate: compute captures and store. */
    {
        IronLIR_LoopInfo *canon = &loops[first_canonical_idx];
        ws_AllocaBlockEntry *alloca_blocks = ws_build_alloca_block_map(fn);
        ws_CaptureTmp *tmps = ws_compute_loop_captures(fn, canon, alloca_blocks);
        int cap_count = (int)arrlen(tmps);

        if (cap_count > 0) {
            /* Arena-allocate the Iron_CaptureEntry array (Phase 6 reads this
             * after Plan 02 ends, so it must outlive the pass — use the module
             * arena passed in from the pipeline caller). */
            Iron_CaptureEntry *entries = (Iron_CaptureEntry *)iron_arena_alloc(
                arena, sizeof(Iron_CaptureEntry) * (size_t)cap_count, _Alignof(Iron_CaptureEntry));
            if (!entries) iron_oom_abort("web_main_loop_split.c:ws_process_function entries");
            for (int i = 0; i < cap_count; i++) {
                entries[i].name       = iron_arena_strdup(arena, tmps[i].name_hint, strlen(tmps[i].name_hint));
                if (!entries[i].name) iron_oom_abort("web_main_loop_split.c:ws_process_function entry_name");
                entries[i].type       = tmps[i].alloc_type;
                entries[i].is_mutable = tmps[i].is_mutable;
            }
            fn->web_frame_captures      = entries;
            fn->web_frame_capture_count = cap_count;
        } else {
            /* Zero captures — still a valid canonical loop (user wrote a purely
             * pass-through frame body that reads nothing from outer scope). Leave
             * the pointer NULL / count 0 (matches memset-zeroed defaults). */
            fn->web_frame_captures      = NULL;
            fn->web_frame_capture_count = 0;
        }

        arrfree(tmps);
        hmfree(alloca_blocks);
    }

cleanup:
    ws_free_loops(loops, loop_count);
    hmfree(idom);
}

/* ── Module-level orphan canonical loop check ────────────────────────────── */

/* After processing all init-window functions, do a second pass to detect
 * canonical loops in functions that lack InitWindow. A function that has NO
 * InitWindow but DOES contain a canonical while-loop is suspicious — the user
 * likely put the main loop in the wrong function. */
static void ws_check_orphan_canonical_loops(IronLIR_Module *module,
                                             Iron_Arena *arena,
                                             Iron_DiagList *diags) {
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern) continue;
        if (ws_function_calls_init_window(fn)) continue; /* already processed */

        /* Quick path: does this function contain ANY call to WindowShouldClose?
         * If not, it cannot contain a canonical loop. Skip the expensive
         * dominator analysis. */
        bool has_wsc_call = false;
        for (int bi = 0; bi < fn->block_count && !has_wsc_call; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                if (ws_is_call_to(blk->instrs[ii], "WindowShouldClose", 0)) {
                    has_wsc_call = true;
                    break;
                }
            }
        }
        if (!has_wsc_call) continue;

        /* There's a WSC call. Check whether any natural loop's header uses it
         * canonically — if so, emit WRONG_FUNCTION. */
        IronLIR_DomEntry *idom = ws_build_domtree(fn);
        int loop_count = 0;
        IronLIR_LoopInfo *loops = ws_build_loop_info(fn, idom, &loop_count);

        for (int li = 0; li < loop_count; li++) {
            IronLIR_Block *hdr = ws_find_block(fn, loops[li].header);
            if (ws_header_is_canonical(fn, hdr)) {
                ws_emit_err(diags, arena, IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION,
                            ws_block_span(hdr), fn->name,
                            "move while(!WindowShouldClose()) into the function that calls InitWindow()");
                break; /* one error per function is enough */
            }
        }

        ws_free_loops(loops, loop_count);
        hmfree(idom);
    }
}

/* ── Public entry point ───────────────────────────────────────────────────── */

void iron_lir_web_main_loop_split(IronLIR_Module *module,
                                  Iron_Arena      *arena,
                                  Iron_DiagList   *diags,
                                  IronBuildTarget  target) {
    if (target != IRON_TARGET_WEB) return;
    if (!module || module->func_count == 0) return;
    if (!arena || !diags) return;

    /* First pass: process every init-window function. */
    for (int fi = 0; fi < module->func_count; fi++) {
        ws_process_function(module->funcs[fi], arena, diags);
        /* Stop-on-first-error semantics match web_await_check: once any
         * function emits an error, don't process later functions (the diag
         * stream is still valid; build.c will bail on error_count > 0). */
        if (diags->error_count > 0) return;
    }

    /* Second pass: orphan canonical loops in non-InitWindow functions. */
    ws_check_orphan_canonical_loops(module, arena, diags);
}
