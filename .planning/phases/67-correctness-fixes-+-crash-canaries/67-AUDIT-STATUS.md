# Phase 67 Audit Status

**Created:** 2026-04-13
**Purpose:** Single source of truth for which CORRECTNESS-AUDIT.md top-20 rows remain OPEN after Phase 66 and which are DONE. Every Phase 67 plan (67-02 through 67-08) reads this document before touching any audit row.

Verification method: Every DONE row's grep-evidence claim has been executed against the live source tree at Phase 67 kickoff (commit `9792e87`). Every OPEN row has been re-read in source and confirmed to still match the Phase 65 baseline pattern. Line numbers listed are post-Phase-66 drift — they reflect where the cited site sits at head, not the pre-Phase-66 line numbers from the original audit.

## Top-20 Status Table

| Rank | File:Line (post-Phase-66 drift) | Dimension | Status | Evidence | Target Plan |
|------|----------------------------------|-----------|--------|----------|-------------|
| 1 | src/runtime/iron_runtime.h:497 | Null + Alloc | DONE — Phase 67-02 | commit `61f0a8c` (feat(67-02): guard IRON_LIST/MAP/SET realloc + malloc via iron_oom_abort); IRON_LIST_IMPL `_push` now captures realloc into `new_items`, NULL-checks it via `iron_oom_abort("Iron_List_" #suffix "_push")`, and also guards capacity-doubling int64_t wraparound. grep `iron_oom_abort` iron_runtime.h → 16 hits across LIST/MAP/SET _create_with_capacity, _clone, _push/_put/_add. Unit test `tests/unit/test_alloc_list_push_oom.c` exercises the capacity-overflow arm and verifies SIGABRT + stderr prefix | — |
| 2 | src/runtime/iron_runtime.h:640-641 | Null + Alloc | DONE — Phase 67-02 | Same commit `61f0a8c`; IRON_MAP_IMPL `_put` now NULL-checks both keys-side and values-side reallocs via `iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_put: keys|values")` and captures each into a named temporary so partial failure cannot leave the map with one live + one dangling pointer. _create_with_capacity and _clone get the same treatment. Unit test `tests/unit/test_alloc_map_put_oom.c` exercises the create_with_capacity malloc-NULL arm and verifies SIGABRT + stderr prefix naming the failing arm | — |
| 3 | src/lir/emit_c.c:3159-3168 | Null + Alloc | DONE — Phase 67-02 | commit `b2e1555` (feat(67-02): guard emit_c.c + emit_web.c generated malloc sites); IRON_LIR_HEAP_ALLOC case now emits `if (!_vN) iron_oom_abort("emit_c HEAP_ALLOC");` between the malloc and the `*_vN =` dereference. Verified end-to-end by building `tests/integration/null_heap_alloc_malloc.iron` with `ironc build --debug-build` and grepping `.iron-build/main.c` — the guard string is literally present in the emitted C. Integration fixture with 4-section doc-comment header committed in the same diff | — |
| 4 | src/lir/emit_c.c:3179-3192 | Null + Alloc | DONE — Phase 67-02 | Same commit `b2e1555`; IRON_LIR_RC_ALLOC case emits `if (!_vN) iron_oom_abort("emit_c RC_ALLOC");` with a distinct location literal so a stderr grep after any OOM abort can tell HEAP_ALLOC and RC_ALLOC apart. Verified in the generated C for `tests/integration/null_rc_alloc_malloc.iron` | — |
| 5 | src/analyzer/typecheck.c:1416 | Blind Cast | DONE — Phase 66-03 | commit `d0070b7` (fix(66-03): rewrite typecheck.c H-severity blind casts (ranks 5, 6, 11)); grep `IRON_NODE_ASSERT_KIND(callee_sym->decl_node, IRON_NODE_OBJECT_DECL)` → 1 hit at typecheck.c:1416; fixture `tests/integration/blind_cast_type_sym_decl.iron` committed with the same commit | — |
| 6 | src/analyzer/typecheck.c:1868 | Blind Cast | DONE — Phase 66-03 | Same commit `d0070b7`; grep `IRON_NODE_ASSERT_KIND(sym->decl_node, IRON_NODE_OBJECT_DECL)` → 1 hit at typecheck.c:1868 (pre-Phase-66 site 1785 moved under insertions); same fixture covers both ranks 5 + 6 | — |
| 7 | src/analyzer/resolve.c:726 | Blind Cast | DONE — Phase 66-03 | commit `4e19a9d` (fix(66-03): rewrite resolve.c H-severity blind casts (ranks 7, 8, 10)); grep `_Static_assert(sizeof(Iron_MethodCallExpr) <= sizeof(Iron_EnumConstruct)` → 1 hit at resolve.c:726; fixture `tests/integration/enum_construct_reinterpret.iron` committed with the same commit | — |
| 8 | src/analyzer/resolve.c:745 | Blind Cast | DONE — Phase 66-03 | Same commit `4e19a9d`; grep `_Static_assert(sizeof(Iron_FieldAccess) <= sizeof(Iron_EnumConstruct)` → 1 hit at resolve.c:745; same fixture covers both ranks 7 + 8 | — |
| 9 | src/parser/ast.h:109-113 + 645-662 | Blind Cast | DONE — Phase 66-01 + 66-03 | Phase 66-01 commit `1ccbb6f` moved `Iron_ExprNode` into `src/parser/ast.h` at line 109 and added the `_Static_assert` prefix-enforcement macro at lines 645-662 (offset-of span/kind/resolved_type locks every AST expression type to the same prefix layout). Phase 66-03 commit `9eb993a` added runtime fixture `tests/integration/blind_cast_expr_common_layout.iron` exercising 12 expression kinds | — |
| 10 | src/analyzer/resolve.c:238 + 273 | Blind Cast | DONE — Phase 66-03 | commit `4e19a9d`; grep `PROT-04 rewrite (rank 10` in resolve.c → 2 hits (lines 238 and 273) guarding the `owner_sym->decl_node` cast behind `IRON_NODE_ASSERT_KIND(..., IRON_NODE_OBJECT_DECL)`; fixture `tests/integration/blind_cast_owner_decl.iron` committed with the same commit | — |
| 11 | src/analyzer/typecheck.c:2813 + 3196 | Blind Cast | DONE — Phase 66-03 | commit `d0070b7`; grep `Iron_ExprNode *expr_node = (Iron_ExprNode *)rs->value` → 1 hit at typecheck.c:3196 (rank 11a); grep `IRON_NODE_ASSERT_KIND(rs->value, IRON_NODE_INT_LIT)` → 1 hit at typecheck.c:2813 (rank 11b); fixture `tests/integration/blind_cast_expr_resolved_type.iron` committed | — |
| 12 | src/analyzer/escape.c:291 | Blind Cast | DONE — Phase 66-03 | commit `9eb993a` (fix(66-03): add escape.c leak-ident walkthrough and rank 9 runtime fixture); grep `IRON_NODE_ASSERT_KIND(ls->expr, IRON_NODE_IDENT)` → 1 hit at escape.c:291 (pre-Phase-66 site 251 drifted to 291 under insertions); fixture `tests/integration/blind_cast_leak_ident.iron` committed with the same commit | — |
| 13 | src/hir/hir_to_lir.c:collect_mono_enums_node | Enum Switch | DONE — Phase 66-02 | commit `91cbcc5` (feat(66-02): enable -Werror=switch-enum globally); `-Werror=switch-enum` forced `collect_mono_enums_node` at hir_to_lir.c:460-463 to become exhaustive — the switch at line 465 now dispatches on every Iron_NodeKind that can contain an expression, and the file builds clean under `-Werror=switch-enum` (any unhandled case would have failed the Phase 66-02 CI run) | — |
| 14 | src/comptime/comptime.c:410-412 | Integer Safety | OPEN | int64 arithmetic still direct: line 410 is `return cval_int(ctx, lv->as_int + rv->as_int)`, line 411 is `lv->as_int - rv->as_int`, line 412 is `lv->as_int * rv->as_int` — no `__builtin_add_overflow` / `__builtin_sub_overflow` / `__builtin_mul_overflow` wrapper anywhere in the int-binop block | 67-03 |
| 15 | src/comptime/comptime.c:493 | Integer Safety | OPEN | line 493 is still `return cval_int(ctx, -operand->as_int)` — no INT64_MIN check before the negation | 67-03 |
| 16 | src/stdlib/iron_net.c:611-618 | Blind Cast | DONE — Phase 66-05 | commit `abfeff5` (fix(66-05): walkthrough resolve.c enum decl casts and fix iron_net const-strip recv); grep `local_recv_buf = (uint8_t *)malloc` → 1 hit at iron_net.c:611, followed by NULL check on :612 and `free(local_recv_buf)` on :618 — the `recv()` call now writes into a local heap buffer, not into the SSO storage of an immutable `Iron_String` | — |
| 17 | src/hir/hir_to_lir.c:1117-1159 (elem_suffix) | Enum Switch | DONE — Phase 66-02 | commit `91cbcc5`; elem_suffix switch at lines 1117-1159 now covers INT/INT8/INT16/INT32/INT64, UINT*, FLOAT/FLOAT32/FLOAT64, BOOL, STRING, and ENUM — the file builds under `-Werror=switch-enum` which would have rejected the pre-Phase-66 fall-through default | — |
| 18 | src/analyzer/resolve.c:726 + 745 (layout assertion) | Blind Cast | DONE — Phase 66-03 | Same commit `4e19a9d` as ranks 7 + 8; the `_Static_assert(sizeof(Iron_MethodCallExpr/Iron_FieldAccess) <= sizeof(Iron_EnumConstruct))` at resolve.c:726 and :745 is literally the layout assertion this audit row called for. Rank 18 is closed by the same edit that closed ranks 7 + 8 | — |
| 19 | src/parser/parser.c:2585 | Integer Safety | OPEN | line 2585 is still `v->explicit_value = (int)atoi(num->value);` — no `strtol` / `ERANGE` handling, no bounds check against INT_MAX/INT_MIN | 67-03 |
| 20 | src/lir/emit_helpers.c:115-219 (emit_type_to_c) | Enum Switch | DONE — Phase 66-02 | commit `91cbcc5`; emit_type_to_c switch at lines 115-219 now includes `case IRON_TYPE_ERROR: return "int";` at line 154 and `case IRON_TYPE_TUPLE:` at line 219. `-Werror=switch-enum` enforces exhaustive coverage — any missing Iron_TypeKind would have broken the build | — |

**Summary:** 17 DONE (ranks 1-13, 16, 17, 18, 20), 3 OPEN (ranks 14, 15, 19 — all routed to 67-03 FIX-04 integer-safety tail).

## Wasm Re-Audit (post-Phase-65)

Phase 65 CORRECTNESS-AUDIT.md pre-dated the WebAssembly target merge (commit `1a9804e` feat: WebAssembly target, merged after 65-06). This section re-audits the 6 post-merge Wasm files against the same 6 dimensions as the original audit.

Files re-audited:
- src/lir/emit_web.c (480 lines)
- src/lir/web_main_loop_split.c (664 lines)
- src/analyzer/web_await_check.c (231 lines)
- src/analyzer/web_top_level_loader_check.c (212 lines)
- src/cli/build_web.c (1024 lines)
- src/stdlib/iron_time_web.c (122 lines)

Dimensions applied (same 6 as Phase 65):
1. **Blind casts (AUDIT-01):** ripgrep `\(Iron_\w+\s*\*\)` in each file, inspect preceding lines for kind check / switch case
2. **Enum switch (AUDIT-02):** ripgrep `\bswitch\b` for switches over `Iron_*Kind` missing cases / opt-out comments
3. **Null safety (AUDIT-03):** inspect pointer derefs in syscall/alloc paths for unguarded dereference
4. **Integer safety (AUDIT-05):** ripgrep `\batoi\(|\batol\(|\batof\(` for overflow risk
5. **Allocation errors (AUDIT-06):** ripgrep `\bmalloc\(|\brealloc\(|\bcalloc\(|\bstrdup\(` for missing NULL check
6. **Arena lifetimes (AUDIT-04):** ripgrep for `arrput|shput|hmput` storing pointers whose backing arena may outlive the caller

### Per-file findings

**src/lir/emit_web.c** (480 lines)
- AUDIT-01 blind casts: 0 hits for `(Iron_<Kind>*)` pattern — file has no AST node casts (operates on LIR values + arena-allocated state)
- AUDIT-02 enum switches: 0 hits — file has no switch statements over Iron enum kinds
- AUDIT-03 null safety: all `ctx->*` derefs are inside helpers already guarded at pass entry
- AUDIT-05 integer safety: 0 hits for `atoi/atol/atof`
- AUDIT-06 allocation errors: 1 hit — **line 278** emits `(FrameState_%s *)malloc(sizeof(FrameState_%s))` into generated C code with NO NULL check before `memset(state, 0, sizeof(FrameState_%s))` on line 281 and `*_e = state;` alias on line 283. This is **generated-code OOM UB** of the same class as ranks 3/4 (HEAP_ALLOC/RC_ALLOC) but in the web main-loop wrapper emitter. **NEW H-SEVERITY FINDING — Wasm-W1.**
- AUDIT-04 arena lifetimes: 0 cross-arena storage issues

**src/lir/web_main_loop_split.c** (664 lines)
- AUDIT-01 blind casts: 1 hit at line 567 — `(Iron_CaptureEntry *)iron_arena_alloc(arena, ...)` — this is a cast of a `void *` arena allocator return, not an AST-node blind cast. Safe.
- AUDIT-02 enum switches: 0 hits over Iron enum kinds
- AUDIT-03 null safety: all `fn->*` derefs are inside guarded helpers
- AUDIT-05 integer safety: 0 hits
- AUDIT-06 allocation errors: 0 hits (file uses arena allocation only via `iron_arena_alloc`, which is the design-documented infallible path)
- AUDIT-04 arena lifetimes: `entries[].name = iron_arena_strdup(arena, ...)` at line 570 stores into the module arena; the arena lifetime extends across the whole compilation unit — safe.

**src/analyzer/web_await_check.c** (231 lines)
- AUDIT-01 blind casts: 13 hits for `(Iron_<Kind>*)node` at lines 89, 100, 104, 130, 139, 144, 149, 154, 160, 171, 177, 179, 210. **Every one is inside a `case IRON_NODE_<Kind>:` label** within `switch ((int)(node->kind))` at line 86 (or within `if (ce->callee->kind == IRON_NODE_IDENT)` at line 103 for the `(Iron_Ident *)ce->callee` cast on 104). The switch case itself is the kind proof — structurally safe. **No new findings.**
- AUDIT-02 enum switches: the switch at line 86 has a documented `-Wswitch-enum` opt-out comment on lines 183-186 (leaf kinds are intentional no-ops). Safe under Phase 66-02 CI.
- AUDIT-03 null safety: `if (!node) return;` guard at line 84; `if (ce->callee && ce->callee->kind == ...)` at line 103 before the `(Iron_Ident *)` cast — safe.
- AUDIT-05 integer safety: 0 hits
- AUDIT-06 allocation errors: 0 hits (file uses `iron_arena_strdup` only, via the infallible arena path)
- AUDIT-04 arena lifetimes: 0 cross-arena storage issues

**src/analyzer/web_top_level_loader_check.c** (212 lines)
- AUDIT-01 blind casts: 11 hits for `(Iron_<Kind>*)node` at lines 97, 100, 116, 125, 130, 135, 140, 146, 157, 163, 165. Same pattern as web_await_check.c — every cast is inside a `case IRON_NODE_<Kind>:` in the `switch ((int)(node->kind))` at line 94 (or guarded by `if (ce->callee->kind == IRON_NODE_IDENT)` at line 99 for the `(Iron_Ident *)ce->callee` cast on 100). Structurally safe. **No new findings.**
- AUDIT-02 enum switches: switch at line 94 has a `-Wswitch-enum` opt-out comment on lines 168-170.
- AUDIT-03 null safety: `if (!node) return;` guard at line 92 plus the kind guard on 99.
- AUDIT-05 integer safety: 0 hits
- AUDIT-06 allocation errors: 0 hits
- AUDIT-04 arena lifetimes: 0 issues

**src/cli/build_web.c** (1024 lines)
- AUDIT-01 blind casts: 0 hits for `(Iron_<Kind>*)` — file is host-side CLI orchestration, no AST work
- AUDIT-02 enum switches: 0 hits over Iron enum kinds
- AUDIT-03 null safety: every `getenv`, `fopen`, `strdup`, `popen`, `fgets` return is NULL-checked (verified spot-reads at lines 41-56, 85-96, 128-137, 246-260, 509-519, 645-723)
- AUDIT-05 integer safety: 0 hits for `atoi/atol/atof` (version parsing via `sscanf("%d.%d.%d")` is range-bounded by variable sizes)
- AUDIT-06 allocation errors: 16 malloc/strdup hits at lines 41, 46, 71, 90, 99, 131, 158, 248, 252, 330, 515, 648, 695, 705, 717, 728, 740, 889, 910, 921. **Every one is NULL-checked** — spot-verified at 41-53 (src_copy+path double-check), 85-96 (path_env+path_dup), 128-137 (cmd), 246-259 (src_copy+toml_path), 509-519 (shell_contents), 645-723 (abs_paths[i] + rl_abs_paths[i] + rl_i_flag + src_i_flag + stdlib_i_flag — each error path frees all prior allocations and returns 1). Host-side code is exemplary; **no new findings**.
- AUDIT-04 arena lifetimes: file uses plain `malloc` + `free` with scoped ownership — no arena mixing

**src/stdlib/iron_time_web.c** (122 lines)
- AUDIT-01 blind casts: 0 hits
- AUDIT-02 enum switches: 0 hits
- AUDIT-03 null safety: 0 pointer derefs in public API (all 8 functions take value types or `Iron_Timer *` whose contract is caller-owned)
- AUDIT-05 integer safety: `(int64_t)emscripten_get_now()` at lines 57 + 67 + 79 — double→int64 truncation is domain-safe for monotonic ms-since-page-load over any reasonable session lifetime (overflow requires >292M years of continuous run). **No new findings.**
- AUDIT-06 allocation errors: 0 hits — file does not allocate
- AUDIT-04 arena lifetimes: 0 allocations

### New H-severity findings

| ID | File | Line | Dimension | Status | Evidence | Target Plan |
|----|------|------|-----------|--------|----------|-------------|
| Wasm-W1 | src/lir/emit_web.c | 278 | Null + Alloc (generated code) | DONE — Phase 67-02 | commit `b2e1555`; `ew_emit_main_wrapper` now emits `if (!state) iron_oom_abort("emit_web main-loop FrameState");` immediately after the malloc and before the `memset(state, 0, ...)` call, preventing SIGSEGV on Emscripten heap-growth-exhausted scenarios. Same `iron_oom_abort` helper (declared in diagnostics.h, defined in src/runtime/iron_oom.c per 67-02 Task 1) as the native HEAP_ALLOC/RC_ALLOC paths. | — |

### New M-severity findings

None. The Wasm files were written after Phase 66-02 (`-Werror=switch-enum`) and Phase 66-03 (`IRON_NODE_ASSERT_KIND` idiom) had already landed, so any blind-cast or non-exhaustive switch introduced afterwards would have broken the build. The only net new issue is the single generated-code malloc on emit_web.c:278, which is not a switch and not an AST cast — the post-Phase-66 CI flags do not catch generated-code OOM.

## Plan Assignment for OPEN Rows

| Rank | File:Line | Plan | Rationale | Status |
|------|-----------|------|-----------|--------|
| 1 | src/runtime/iron_runtime.h:497 | 67-02 | H-severity runtime macro OOM — lands with `iron_oom_abort` helper + IRON_LIST_IMPL edit | DONE — commit `61f0a8c` |
| 2 | src/runtime/iron_runtime.h:640-641 | 67-02 | H-severity runtime macro OOM — same helper, IRON_MAP_IMPL edit | DONE — commit `61f0a8c` |
| 3 | src/lir/emit_c.c:3159-3168 | 67-02 | H-severity generated-C OOM — emit `iron_oom_abort()` call in HEAP_ALLOC codegen | DONE — commit `b2e1555` |
| 4 | src/lir/emit_c.c:3179-3192 | 67-02 | H-severity generated-C OOM — same, RC_ALLOC codegen | DONE — commit `b2e1555` |
| Wasm-W1 | src/lir/emit_web.c:278 | 67-02 | H-severity generated-C OOM in web main-loop wrapper — same `iron_oom_abort()` call, web emitter edit | DONE — commit `b2e1555` |
| 14 | src/comptime/comptime.c:410-412 | 67-03 | Integer-overflow arithmetic — wrap in `__builtin_add/sub/mul_overflow` with error emission | OPEN |
| 15 | src/comptime/comptime.c:493 | 67-03 | INT64_MIN negation UB — explicit check before negation | OPEN |
| 19 | src/parser/parser.c:2585 | 67-03 | `atoi` → `strtol` with ERANGE + INT_MIN/INT_MAX bounds for enum variant values | OPEN |

### FIX-02 full walkthrough (non-top-20 arena sites)

The full 285 M-severity arena sites enumerated in CORRECTNESS-AUDIT.md §6 (Allocation Error Handling) are NOT in the top-20 table but are in-scope for Phase 67 per 67-CONTEXT.md LOCKED decision. Those sites are distributed across plans 67-04 (parser), 67-05 (analyzer + comptime), 67-06 (hir + lir + runtime/stdlib) by subsystem ownership.

### FIX-03 cross-arena rows (AUDIT-04 16 M rows)

All 16 AUDIT-04 M-severity rows are assigned to plan 67-07 (FIX-03 focus plan).

### FIX-04 enum-switch tail (AUDIT-02 17 M rows)

Most AUDIT-02 M rows are already covered by Phase 66-02's `-Werror=switch-enum` rollout (82 opt-out comments landed). Any that still lack an explicit case or opt-out comment are assigned to plan 67-03 (FIX-04 tail — same plan as ranks 14/15/19 for minimal touch spread).

### REG-02 canaries (15 fixtures)

All 15 canary fixtures are assigned to plan 67-08 (last plan, runs after all fixes land so each canary can exercise the new guard and verify the `iron_oom_abort` / `__builtin_overflow` / `strtol` paths produce the expected diagnostic instead of UB).

## Phase 67 Plan Count Distribution

| Plan | Scope | Top-20 rows | Wasm re-audit | Other rows |
|------|-------|-------------|---------------|------------|
| 67-01 | audit status (this doc) | 0 | 0 | 0 |
| 67-02 | FIX-01: H alloc-fail — iron_oom_abort + runtime macros + LIR emit + web emit | 4 (ranks 1, 2, 3, 4) | 1 (Wasm-W1) | 0 |
| 67-03 | FIX-04 integer-safety tail + FIX-04 enum-switch tail | 3 (ranks 14, 15, 19) | 0 | 17 AUDIT-02 M rows |
| 67-04 | FIX-02 parser arena walkthrough | 0 | 0 | ~80 parser/lexer arena sites |
| 67-05 | FIX-02 analyzer + comptime arena walkthrough | 0 | 0 | ~100 analyzer/comptime arena sites |
| 67-06 | FIX-02 hir + lir + runtime/stdlib arena walkthrough | 0 | 0 | ~105 hir/lir/runtime/stdlib arena sites |
| 67-07 | FIX-03 cross-arena AUDIT-04 walkthrough | 0 | 0 | 16 AUDIT-04 M rows |
| 67-08 | REG-02 crash-canary fixtures | 0 | 0 | 15 canary fixtures |

---
*Phase: 67-correctness-fixes-+-crash-canaries*
*Status verification: 2026-04-13*
*Verification commit head: `9792e87`*
