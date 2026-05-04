# Abort-Site Audit — Phase 1 M0 Compiler Hardening

**Audited:** 2026-04-17
**Scope:** every `iron_oom_abort` and `IRON_NODE_ASSERT_KIND` site transitively reachable from `iron_analyze_buffer`.
**Categories:** (a) REPLACE — hot path, convert to fallible channel; (b) KEEP — cold path OR genuine invariant after a guaranteeing check; (c) MOVE — precondition validated earlier so the site disappears.

This audit is a documentation deliverable only. No source modifications in this plan. Plans 02 / 03 / 04 / 05 consume this audit to decide per-site REPLACE / KEEP / MOVE actions.

## Summary

| File | `iron_oom_abort` sites | REPLACE (a) | KEEP (b) | MOVE (c) | `IRON_NODE_ASSERT_KIND` sites | REPLACE (a) | KEEP (b) | MOVE (c) |
|------|-----------------------|---------|------|------|-------------------------------|---------|------|------|
| src/parser/parser.c                            | 114 | 114 | 0 | 0 | 0  | 0 | 0 | 0 |
| src/analyzer/typecheck.c                       | 38  | 36  | 0 | 2 | 16 | 4 | 12 | 0 |
| src/analyzer/resolve.c                         | 8   | 8   | 0 | 0 | 6  | 2 | 4 | 0 |
| src/analyzer/capture.c                         | 6   | 6   | 0 | 0 | 0  | 0 | 0 | 0 |
| src/analyzer/init_check.c                      | 1   | 1   | 0 | 0 | 0  | 0 | 0 | 0 |
| src/analyzer/escape.c                          | 0   | 0   | 0 | 0 | 1  | 1 | 0 | 0 |
| src/analyzer/concurrency.c                     | 2   | 2   | 0 | 0 | 0  | 0 | 0 | 0 |
| src/analyzer/types.c                           | 18  | 0   | 0 | 18 | 0 | 0 | 0 | 0 |
| src/analyzer/web_await_check.c                 | 1   | 1   | 0 | 0 | 0  | 0 | 0 | 0 |
| src/analyzer/web_top_level_loader_check.c      | 1   | 1   | 0 | 0 | 0  | 0 | 0 | 0 |
| src/comptime/comptime.c                        | 18  | 16  | 0 | 2 | 0  | 0 | 0 | 0 |
| **Total**                                      | 207 | 185 | 0 | 22 | 23 | 7 | 16 | 0 |

## Methodology

Site enumeration commands (reproducible):

```
grep -n 'iron_oom_abort' src/parser/parser.c
grep -n 'iron_oom_abort' src/analyzer/typecheck.c
grep -n 'iron_oom_abort' src/analyzer/resolve.c
grep -n 'iron_oom_abort' src/analyzer/capture.c
grep -n 'iron_oom_abort' src/analyzer/init_check.c
grep -n 'iron_oom_abort' src/analyzer/concurrency.c
grep -n 'iron_oom_abort' src/analyzer/types.c
grep -n 'iron_oom_abort' src/analyzer/web_await_check.c
grep -n 'iron_oom_abort' src/analyzer/web_top_level_loader_check.c
grep -n 'iron_oom_abort' src/comptime/comptime.c
grep -n 'IRON_NODE_ASSERT_KIND' src/analyzer/typecheck.c
grep -n 'IRON_NODE_ASSERT_KIND' src/analyzer/resolve.c
grep -n 'IRON_NODE_ASSERT_KIND' src/analyzer/escape.c
```

Category assignment rules:

- (a) REPLACE — site is transitively reachable from `iron_analyze_buffer` on parse-or-analyze hot path AND a sensible fallible alternative exists (propagate `Iron_ErrorNode`, return `iron_type_make_primitive(IRON_TYPE_ERROR)` poison, or fall back to a static-string diagnostic message).
- (b) KEEP — cold path OR a genuine post-check invariant (site only reachable after a prior kind/symbol check that structurally guarantees the assert holds).
- (c) MOVE — the precondition can be validated earlier at pass entry; once validated there, the call-site itself is removed, not just converted.

**Transitive-reach note (RESEARCH.md Pitfall 6):** `ARENA_ALLOC` invokes `iron_oom_abort` internally on malloc failure; sites counted here are the ENCLOSING `iron_oom_abort` call after the `if (!ptr)` check, not the allocator interior. `src/analyzer/types.c` (18 sites) is on the hot path because every analyzer pass calls `iron_type_make_*`. `src/comptime/comptime.c` (18 sites) is on the hot path because `iron_comptime_apply` runs inside `iron_analyze` step 6.

## Per-file breakdown

### src/parser/parser.c (114 `iron_oom_abort` sites)

Every site guards an `ARENA_ALLOC` / `iron_arena_strdup` result. All are on the LSP hot path — a single malformed input could starve the arena — and the parser already has a fallible error channel via `iron_make_error(p)` + `p->in_error_recovery`. Category: **all (a) REPLACE** — Plan 04 consumes this list.

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 187  | `iron_oom_abort("parser.c:iron_make_error")` | a — REPLACE | Core `iron_make_error`; OOM propagates via returning a single shared error-node sentinel (set `p->in_error_recovery`) |
| 268  | `iron_oom_abort("parser.c:iron_parse_type_annotation tuple elems")` | a — REPLACE | Tuple element array OOM; propagate via `iron_make_error(p)` |
| 273  | `iron_oom_abort("parser.c:iron_parse_type_annotation tuple")` | a — REPLACE | Tuple TypeAnnotation; propagate via ErrorNode |
| 287  | `iron_oom_abort("parser.c:iron_parse_type_annotation array")` | a — REPLACE | Array TypeAnnotation; propagate via ErrorNode |
| 334  | `iron_oom_abort("parser.c:iron_parse_type_annotation array elem name")` | a — REPLACE | strdup fallback to static "?" name, continue with ErrorNode |
| 389  | `iron_oom_abort("parser.c:iron_parse_type_annotation func")` | a — REPLACE | Func TypeAnnotation; propagate via ErrorNode |
| 446  | `iron_oom_abort("parser.c:iron_parse_type_annotation named")` | a — REPLACE | Named TypeAnnotation; propagate via ErrorNode |
| 453  | `iron_oom_abort("parser.c:iron_parse_type_annotation named name")` | a — REPLACE | strdup fallback to static "?" name |
| 509  | `iron_oom_abort("parser.c:iron_parse_generic_params")` | a — REPLACE | Generic-params Ident OOM; skip param and continue |
| 513  | `iron_oom_abort("parser.c:iron_parse_generic_params name")` | a — REPLACE | strdup fallback to static "?" |
| 519  | `iron_oom_abort("parser.c:iron_parse_generic_params constraint")` | a — REPLACE | Constraint-name OOM; drop constraint and continue |
| 567  | `iron_oom_abort("parser.c:iron_parse_param_list")` | a — REPLACE | Param-node OOM; skip param and continue |
| 573  | `iron_oom_abort("parser.c:iron_parse_param_list name")` | a — REPLACE | strdup fallback |
| 636  | `iron_oom_abort("parser.c:iron_parse_block")` | a — REPLACE | Block node; propagate via ErrorNode |
| 691  | `iron_oom_abort("parser.c:iron_parse_lambda")` | a — REPLACE | Lambda node; propagate via ErrorNode |
| 752  | `iron_oom_abort("parser.c:iron_parse_primary IntLit")` | a — REPLACE | IntLit node; propagate via ErrorNode |
| 756  | `iron_oom_abort("parser.c:iron_parse_primary IntLit value")` | a — REPLACE | Strdup fallback to "0" |
| 763  | `iron_oom_abort("parser.c:iron_parse_primary FloatLit")` | a — REPLACE | FloatLit node; ErrorNode |
| 767  | `iron_oom_abort("parser.c:iron_parse_primary FloatLit value")` | a — REPLACE | Strdup fallback to "0.0" |
| 774  | `iron_oom_abort("parser.c:iron_parse_primary StringLit")` | a — REPLACE | StringLit node; ErrorNode |
| 778  | `iron_oom_abort("parser.c:iron_parse_primary StringLit value")` | a — REPLACE | Strdup fallback to "" |
| 790  | `iron_oom_abort("parser.c:iron_parse_primary BoolLit true")` | a — REPLACE | BoolLit; ErrorNode |
| 800  | `iron_oom_abort("parser.c:iron_parse_primary BoolLit false")` | a — REPLACE | BoolLit; ErrorNode |
| 810  | `iron_oom_abort("parser.c:iron_parse_primary NullLit")` | a — REPLACE | NullLit; ErrorNode |
| 820  | `iron_oom_abort("parser.c:iron_parse_primary UnaryExpr minus")` | a — REPLACE | Unary; ErrorNode |
| 832  | `iron_oom_abort("parser.c:iron_parse_primary UnaryExpr not")` | a — REPLACE | Unary; ErrorNode |
| 844  | `iron_oom_abort("parser.c:iron_parse_primary UnaryExpr tilde")` | a — REPLACE | Unary; ErrorNode |
| 896  | `iron_oom_abort("parser.c:iron_parse_primary tuple ArrayLit")` | a — REPLACE | Tuple-as-array; ErrorNode |
| 905  | `iron_oom_abort("parser.c:iron_parse_primary tuple elems")` | a — REPLACE | Tuple elems arena; ErrorNode |
| 914  | `iron_oom_abort("parser.c:iron_parse_primary tuple tag")` | a — REPLACE | strdup fallback to static "?" |
| 935  | `iron_oom_abort("parser.c:iron_parse_primary HeapExpr")` | a — REPLACE | Heap node; ErrorNode |
| 949  | `iron_oom_abort("parser.c:iron_parse_primary RcExpr")` | a — REPLACE | Rc node; ErrorNode |
| 960  | `iron_oom_abort("parser.c:iron_parse_primary ComptimeExpr")` | a — REPLACE | Comptime node; ErrorNode |
| 971  | `iron_oom_abort("parser.c:iron_parse_primary AwaitExpr")` | a — REPLACE | Await node; ErrorNode |
| 988  | `iron_oom_abort("parser.c:iron_parse_primary ArrayLit bracket")` | a — REPLACE | ArrayLit; ErrorNode |
| 1041 | `iron_oom_abort("parser.c:iron_parse_primary Ident")` | a — REPLACE | Ident node; ErrorNode |
| 1045 | `iron_oom_abort("parser.c:iron_parse_primary Ident name")` | a — REPLACE | strdup fallback |
| 1055 | `iron_oom_abort("parser.c:iron_parse_primary Ident self")` | a — REPLACE | ErrorNode |
| 1067 | `iron_oom_abort("parser.c:iron_parse_primary Ident super")` | a — REPLACE | ErrorNode |
| 1121 | `iron_oom_abort("parser.c:iron_parse_expr_prec dot name")` | a — REPLACE | strdup fallback |
| 1139 | `iron_oom_abort("parser.c:iron_parse_expr_prec EnumConstruct call")` | a — REPLACE | EnumConstruct node; ErrorNode |
| 1153 | `iron_oom_abort("parser.c:iron_parse_expr_prec MethodCall ident")` | a — REPLACE | MethodCall node; ErrorNode |
| 1169 | `iron_oom_abort("parser.c:iron_parse_expr_prec MethodCall nonident")` | a — REPLACE | MethodCall node; ErrorNode |
| 1188 | `iron_oom_abort("parser.c:iron_parse_expr_prec EnumConstruct unit")` | a — REPLACE | EnumConstruct node; ErrorNode |
| 1203 | `iron_oom_abort("parser.c:iron_parse_expr_prec FieldAccess")` | a — REPLACE | FieldAccess node; ErrorNode |
| 1229 | `iron_oom_abort("parser.c:iron_parse_expr_prec SliceExpr")` | a — REPLACE | Slice node; ErrorNode |
| 1240 | `iron_oom_abort("parser.c:iron_parse_expr_prec IndexExpr")` | a — REPLACE | Index node; ErrorNode |
| 1259 | `iron_oom_abort("parser.c:iron_parse_expr_prec CallExpr")` | a — REPLACE | Call node; ErrorNode |
| 1284 | `iron_oom_abort("parser.c:iron_parse_expr_prec IsExpr type_name")` | a — REPLACE | strdup fallback |
| 1286 | `iron_oom_abort("parser.c:iron_parse_expr_prec IsExpr")` | a — REPLACE | Is node; ErrorNode |
| 1302 | `iron_oom_abort("parser.c:iron_parse_expr_prec BinaryExpr")` | a — REPLACE | Binary node; ErrorNode |
| 1349 | `iron_oom_abort("parser.c:iron_parse_if_stmt")` | a — REPLACE | If node; ErrorNode |
| 1370 | `iron_oom_abort("parser.c:iron_parse_while_stmt")` | a — REPLACE | While node; ErrorNode |
| 1393 | `iron_oom_abort("parser.c:iron_parse_for_stmt var_name")` | a — REPLACE | strdup fallback |
| 1418 | `iron_oom_abort("parser.c:iron_parse_for_stmt")` | a — REPLACE | For node; ErrorNode |
| 1485 | `iron_oom_abort("parser.c:iron_parse_pattern bname")` | a — REPLACE | strdup fallback |
| 1505 | `iron_oom_abort("parser.c:iron_parse_pattern")` | a — REPLACE | Pattern node; ErrorNode |
| 1511 | `iron_oom_abort("parser.c:iron_parse_pattern enum_name")` | a — REPLACE | strdup fallback |
| 1514 | `iron_oom_abort("parser.c:iron_parse_pattern variant_name")` | a — REPLACE | strdup fallback |
| 1560 | `iron_oom_abort("parser.c:iron_parse_match_stmt else Block")` | a — REPLACE | Block node; ErrorNode |
| 1595 | `iron_oom_abort("parser.c:iron_parse_match_stmt MatchCase error recovery")` | a — REPLACE | Already in recovery; ErrorNode |
| 1618 | `iron_oom_abort("parser.c:iron_parse_match_stmt case Block")` | a — REPLACE | Block; ErrorNode |
| 1628 | `iron_oom_abort("parser.c:iron_parse_match_stmt MatchCase")` | a — REPLACE | MatchCase; ErrorNode |
| 1642 | `iron_oom_abort("parser.c:iron_parse_match_stmt MatchStmt")` | a — REPLACE | Match; ErrorNode |
| 1665 | `iron_oom_abort("parser.c:iron_parse_spawn_stmt name")` | a — REPLACE | strdup fallback |
| 1683 | `iron_oom_abort("parser.c:iron_parse_spawn_stmt SpawnStmt")` | a — REPLACE | Spawn; ErrorNode |
| 1704 | `iron_oom_abort("parser.c:iron_parse_interp_string")` | a — REPLACE | Interp; ErrorNode |
| 1730 | `iron_oom_abort("parser.c:iron_parse_interp_string StringLit segment")` | a — REPLACE | StringLit; ErrorNode |
| 1734 | `iron_oom_abort("parser.c:iron_parse_interp_string StringLit segment value")` | a — REPLACE | strdup fallback |
| 1797 | `iron_oom_abort("parser.c:iron_parse_interp_string StringLit tail")` | a — REPLACE | StringLit; ErrorNode |
| 1801 | `iron_oom_abort("parser.c:iron_parse_interp_string StringLit tail value")` | a — REPLACE | strdup fallback |
| 1826 | `iron_oom_abort("parser.c:iron_parse_val_decl tuple binding name")` | a — REPLACE | strdup fallback |
| 1858 | `iron_oom_abort("parser.c:iron_parse_val_decl tuple arena_names")` | a — REPLACE | ErrorNode |
| 1881 | `iron_oom_abort("parser.c:iron_parse_val_decl tuple ValDecl")` | a — REPLACE | ValDecl; ErrorNode |
| 1924 | `iron_oom_abort("parser.c:iron_parse_val_decl spawn handle_name")` | a — REPLACE | strdup fallback |
| 1933 | `iron_oom_abort("parser.c:iron_parse_val_decl ValDecl")` | a — REPLACE | ValDecl; ErrorNode |
| 1939 | `iron_oom_abort("parser.c:iron_parse_val_decl ValDecl name")` | a — REPLACE | strdup fallback |
| 1977 | `iron_oom_abort("parser.c:iron_parse_var_decl spawn handle_name")` | a — REPLACE | strdup fallback |
| 1986 | `iron_oom_abort("parser.c:iron_parse_var_decl VarDecl")` | a — REPLACE | VarDecl; ErrorNode |
| 1992 | `iron_oom_abort("parser.c:iron_parse_var_decl VarDecl name")` | a — REPLACE | strdup fallback |
| 2017 | `iron_oom_abort("parser.c:iron_parse_stmt ReturnStmt")` | a — REPLACE | Return; ErrorNode |
| 2036 | `iron_oom_abort("parser.c:iron_parse_stmt DeferStmt")` | a — REPLACE | Defer; ErrorNode |
| 2046 | `iron_oom_abort("parser.c:iron_parse_stmt FreeStmt")` | a — REPLACE | Free; ErrorNode |
| 2056 | `iron_oom_abort("parser.c:iron_parse_stmt LeakStmt")` | a — REPLACE | Leak; ErrorNode |
| 2091 | `iron_oom_abort("parser.c:iron_parse_stmt AssignStmt")` | a — REPLACE | Assign; ErrorNode |
| 2148 | `iron_oom_abort("parser.c:iron_parse_import_decl path")` | a — REPLACE | strdup fallback |
| 2158 | `iron_oom_abort("parser.c:iron_parse_import_decl alias")` | a — REPLACE | strdup fallback |
| 2163 | `iron_oom_abort("parser.c:iron_parse_import_decl ImportDecl")` | a — REPLACE | Import; ErrorNode |
| 2185 | `iron_oom_abort("parser.c:iron_snake_to_camel")` | a — REPLACE | strdup fallback to input name |
| 2234 | `iron_oom_abort("parser.c:iron_parse_extern_func iron_name")` | a — REPLACE | strdup fallback |
| 2250 | `iron_oom_abort("parser.c:iron_parse_extern_func FuncDecl")` | a — REPLACE | FuncDecl; ErrorNode |
| 2334 | `iron_oom_abort("parser.c:iron_parse_func_or_method array MethodDecl")` | a — REPLACE | MethodDecl; ErrorNode |
| 2340 | `iron_oom_abort("parser.c:iron_parse_func_or_method array method_name")` | a — REPLACE | strdup fallback |
| 2353 | `iron_oom_abort("parser.c:iron_parse_func_or_method array elem_type_name")` | a — REPLACE | strdup fallback |
| 2404 | `iron_oom_abort("parser.c:iron_parse_func_or_method MethodDecl")` | a — REPLACE | MethodDecl; ErrorNode |
| 2409 | `iron_oom_abort("parser.c:iron_parse_func_or_method type_name")` | a — REPLACE | strdup fallback |
| 2412 | `iron_oom_abort("parser.c:iron_parse_func_or_method method_name")` | a — REPLACE | strdup fallback |
| 2439 | `iron_oom_abort("parser.c:iron_parse_func_or_method FuncDecl")` | a — REPLACE | FuncDecl; ErrorNode |
| 2444 | `iron_oom_abort("parser.c:iron_parse_func_or_method FuncDecl name")` | a — REPLACE | strdup fallback |
| 2487 | `iron_oom_abort("parser.c:iron_parse_object_decl extends_name")` | a — REPLACE | strdup fallback |
| 2500 | `iron_oom_abort("parser.c:iron_parse_object_decl impl iname")` | a — REPLACE | strdup fallback |
| 2550 | `iron_oom_abort("parser.c:iron_parse_object_decl Field")` | a — REPLACE | Field; ErrorNode |
| 2557 | `iron_oom_abort("parser.c:iron_parse_object_decl Field name")` | a — REPLACE | strdup fallback |
| 2569 | `iron_oom_abort("parser.c:iron_parse_object_decl ObjectDecl")` | a — REPLACE | Object; ErrorNode |
| 2575 | `iron_oom_abort("parser.c:iron_parse_object_decl ObjectDecl name")` | a — REPLACE | strdup fallback |
| 2641 | `iron_oom_abort("parser.c:iron_parse_interface_decl sig FuncDecl")` | a — REPLACE | FuncDecl; ErrorNode |
| 2647 | `iron_oom_abort("parser.c:iron_parse_interface_decl sig name")` | a — REPLACE | strdup fallback |
| 2667 | `iron_oom_abort("parser.c:iron_parse_interface_decl InterfaceDecl")` | a — REPLACE | Interface; ErrorNode |
| 2673 | `iron_oom_abort("parser.c:iron_parse_interface_decl InterfaceDecl name")` | a — REPLACE | strdup fallback |
| 2718 | `iron_oom_abort("parser.c:iron_parse_enum_decl EnumVariant")` | a — REPLACE | EnumVariant; ErrorNode |
| 2722 | `iron_oom_abort("parser.c:iron_parse_enum_decl EnumVariant name")` | a — REPLACE | strdup fallback |
| 2789 | `iron_oom_abort("parser.c:iron_parse_enum_decl EnumDecl")` | a — REPLACE | Enum; ErrorNode |
| 2795 | `iron_oom_abort("parser.c:iron_parse_enum_decl EnumDecl name")` | a — REPLACE | strdup fallback |
| 2941 | `iron_oom_abort("parser.c:iron_parse Program")` | a — REPLACE | Program root — the plan's dependency-root: at this single site, emit `IRON_ERR_OOM` diagnostic and return a minimal empty Program node so downstream analyzer passes see a valid but empty AST |

### src/analyzer/typecheck.c (38 `iron_oom_abort` + 16 `IRON_NODE_ASSERT_KIND`)

#### iron_oom_abort sites

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 218  | `iron_oom_abort("typecheck.c:emit_error msg")` | a — REPLACE | strdup fallback to a static "type error" message |
| 222  | `iron_oom_abort("typecheck.c:emit_error suggestion")` | a — REPLACE | Drop suggestion on OOM; keep the ERROR itself |
| 231  | `iron_oom_abort("typecheck.c:emit_warning msg")` | a — REPLACE | strdup fallback to a static "type warning" message |
| 235  | `iron_oom_abort("typecheck.c:emit_warning suggestion")` | a — REPLACE | Drop suggestion on OOM |
| 603  | `iron_oom_abort("typecheck.c:resolve_type_annotation tuple_elems")` | a — REPLACE | Return `iron_type_make_primitive(IRON_TYPE_ERROR)` poison |
| 622  | `iron_oom_abort("typecheck.c:resolve_type_annotation func_params")` | a — REPLACE | Return poison type |
| 693  | `iron_oom_abort("typecheck.c:resolve_type_annotation type_args")` | a — REPLACE | Return poison type |
| 707  | `iron_oom_abort("typecheck.c:resolve_type_annotation mangled")` | a — REPLACE | Return poison type |
| 725  | `iron_oom_abort("typecheck.c:resolve_type_annotation mono")` | a — REPLACE | Return poison type |
| 736  | `iron_oom_abort("typecheck.c:resolve_type_annotation mono_key")` | a — REPLACE | Return poison type |
| 772  | `iron_oom_abort("typecheck.c:resolve_type_annotation vpt")` | a — REPLACE | Return poison type |
| 780  | `iron_oom_abort("typecheck.c:resolve_type_annotation vpt row")` | a — REPLACE | Skip the row and continue |
| 796  | `iron_oom_abort("typecheck.c:resolve_type_annotation pib")` | a — REPLACE | Return poison type |
| 803  | `iron_oom_abort("typecheck.c:resolve_type_annotation pib row")` | a — REPLACE | Skip the row and continue |
| 2098 | `iron_oom_abort("typecheck.c:check_expr LAMBDA param_types")` | a — REPLACE | Return poison type |
| 2157 | `iron_oom_abort("typecheck.c:check_expr ARRAY_LIT tuple")` | a — REPLACE | Return poison type |
| 2286 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT inferred_args")` | a — REPLACE | Return poison type |
| 2379 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT mangled")` | a — REPLACE | Return poison type |
| 2397 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT mono")` | a — REPLACE | Return poison type |
| 2407 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT mono2_key")` | a — REPLACE | Return poison type |
| 2416 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT vpt")` | a — REPLACE | Return poison type |
| 2447 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT vpt row")` | a — REPLACE | Skip the row |
| 2461 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT pib2")` | a — REPLACE | Return poison type |
| 2468 | `iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT pib2 row")` | a — REPLACE | Skip the row |
| 2621 | `iron_oom_abort("typecheck.c:check_stmt VAL_DECL tuple-mismatch msg")` | a — REPLACE | Static message fallback |
| 2989 | `iron_oom_abort("typecheck.c:check_stmt MATCH covered payload")` | a — REPLACE | Treat match as non-exhaustive; emit existing non-exhaustive diag |
| 3066 | `iron_oom_abort("typecheck.c:check_stmt MATCH else-note")` | a — REPLACE | Static message fallback |
| 3087 | `iron_oom_abort("typecheck.c match-exhaustiveness covered[]")` | c — MOVE | Pre-allocate covered[] bitmap at pass entry once the variant count is known, rather than per-match-stmt |
| 3295 | `iron_oom_abort("typecheck.c:check_func_decl param_types")` | a — REPLACE | Skip this func decl; emit diag |
| 3356 | `iron_oom_abort("typecheck.c:check_method_decl param_types")` | a — REPLACE | Skip this method decl; emit diag |
| 3520 | `iron_oom_abort("typecheck.c:iron_typecheck enum vpt")` | a — REPLACE | Skip this enum; emit diag |
| 3526 | `iron_oom_abort("typecheck.c:iron_typecheck enum pib_ty")` | a — REPLACE | Skip this enum; emit diag |
| 3537 | `iron_oom_abort("typecheck.c:iron_typecheck enum vpt row")` | a — REPLACE | Skip this row |
| 3541 | `iron_oom_abort("typecheck.c:iron_typecheck enum ev payload_is_boxed")` | a — REPLACE | Skip this variant |
| 3545 | `iron_oom_abort("typecheck.c:iron_typecheck enum pib_row")` | a — REPLACE | Skip this row |
| 3575 | `iron_oom_abort("typecheck.c:iron_typecheck FUNC_DECL param_types")` | c — MOVE | Allocate at pass entry once per decl-count budget |
| 3605 | `iron_oom_abort("typecheck.c:iron_typecheck METHOD_DECL param_types")` | a — REPLACE | Skip this method; emit diag |
| 145 (near) | `iron_oom_abort` interior (covered[] 3080-3087 comment block) | b — documentation | Same site as 3087 above; no separate call |

#### IRON_NODE_ASSERT_KIND sites

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 474  | `IRON_NODE_ASSERT_KIND(csym->decl_node, IRON_NODE_INTERFACE_DECL)` | b — KEEP | `csym->kind` is filtered to IRON_SYMBOL_INTERFACE earlier; invariant after resolve |
| 539  | `IRON_NODE_ASSERT_KIND(generic_params[i], IRON_NODE_IDENT)` | b — KEEP | generic_params[] constructed by parser only with IDENT nodes; structural invariant |
| 753  | `IRON_NODE_ASSERT_KIND(ed->generic_params[i], IRON_NODE_IDENT)` | b — KEEP | Same as 539 (EnumDecl generic_params) |
| 1435 | `IRON_NODE_ASSERT_KIND(callee_sym->decl_node, IRON_NODE_OBJECT_DECL)` | a — REPLACE | Can fire on half-parsed AST where decl_node is IRON_NODE_ERROR; convert to graceful return-with-diag |
| 1480 | `IRON_NODE_ASSERT_KIND(od->generic_params[gi], IRON_NODE_IDENT)` | b — KEEP | generic_params[] structural invariant |
| 1607 | `IRON_NODE_ASSERT_KIND(fn_sym->decl_node, IRON_NODE_FUNC_DECL)` | a — REPLACE | Can fire on IRON_NODE_ERROR; graceful return-with-diag |
| 1617 | `IRON_NODE_ASSERT_KIND(fd->generic_params[gi], IRON_NODE_IDENT)` | b — KEEP | Structural invariant |
| 1887 | `IRON_NODE_ASSERT_KIND(sym->decl_node, IRON_NODE_OBJECT_DECL)` | a — REPLACE | Can fire on IRON_NODE_ERROR; graceful return-with-diag |
| 2298 | `IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT)` | b — KEEP | Structural invariant |
| 2323 | `IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT)` | b — KEEP | Structural invariant |
| 2350 | `IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT)` | b — KEEP | Structural invariant |
| 2427 | `IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT)` | b — KEEP | Structural invariant |
| 2841 | `IRON_NODE_ASSERT_KIND(rs->value, IRON_NODE_INT_LIT)` | b — KEEP | Return-value kind filtered upstream |
| 2917 | `IRON_NODE_ASSERT_KIND(is_s->condition, IRON_NODE_IS)` | b — KEEP | Narrowing if-stmt condition pre-checked |
| 3224 | `IRON_NODE_ASSERT_KIND(ss->body, IRON_NODE_BLOCK)` | b — KEEP | Spawn-body structural invariant (parser guarantees) |
| 3414 | `IRON_NODE_ASSERT_KIND(iface_sym->decl_node, IRON_NODE_INTERFACE_DECL)` | a — REPLACE | Can fire on IRON_NODE_ERROR; graceful return-with-diag |

### src/analyzer/resolve.c (8 `iron_oom_abort` + 6 `IRON_NODE_ASSERT_KIND`)

#### iron_oom_abort sites

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 66   | `iron_oom_abort("resolve.c:emit_undefined msg")` | a — REPLACE | Static-string fallback ("undefined identifier") |
| 637  | `iron_oom_abort("resolve.c:resolve_expr PATTERN unknown-enum msg")` | a — REPLACE | Static-string fallback |
| 666  | `iron_oom_abort("resolve.c:resolve_expr PATTERN no-variant msg")` | a — REPLACE | Static-string fallback |
| 690  | `iron_oom_abort("resolve.c:resolve_expr PATTERN shadow msg")` | a — REPLACE | Static-string fallback |
| 711  | `iron_oom_abort("resolve.c:resolve_expr ENUM_CONSTRUCT ident_node")` | a — REPLACE | Propagate NULL; caller handles |
| 740  | `iron_oom_abort("resolve.c:resolve_expr ENUM_CONSTRUCT method_call")` | a — REPLACE | Propagate NULL |
| 760  | `iron_oom_abort("resolve.c:resolve_expr ENUM_CONSTRUCT field_access")` | a — REPLACE | Propagate NULL |
| 790  | `iron_oom_abort("resolve.c:resolve_expr ENUM_CONSTRUCT no-variant msg")` | a — REPLACE | Static-string fallback |

#### IRON_NODE_ASSERT_KIND sites

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 256  | `IRON_NODE_ASSERT_KIND(ctx->current_method->owner_sym->decl_node, ...)` | b — KEEP | `current_method` only set after structural check; invariant |
| 278  | `IRON_NODE_ASSERT_KIND(ctx->current_method->owner_sym->decl_node, ...)` | b — KEEP | Same as 256; explicitly documented as "structurally safe; still call assert" |
| 651  | `IRON_NODE_ASSERT_KIND(esym->decl_node, IRON_NODE_ENUM_DECL)` | a — REPLACE | Can fire on IRON_NODE_ERROR; graceful early-return with diag |
| 732  | `IRON_NODE_ASSERT_KIND(ec, IRON_NODE_ENUM_CONSTRUCT)` | b — KEEP | Argument comes from explicit cast of the ENUM_CONSTRUCT branch |
| 775  | `IRON_NODE_ASSERT_KIND(esym->decl_node, IRON_NODE_ENUM_DECL)` | a — REPLACE | Same as 651 |
| 819  | (the existing IRON_NODE_ERROR case — not an assert) | n/a | Already the graceful example analog; kept untouched |

### src/analyzer/capture.c (6 `iron_oom_abort` sites)

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 389  | `iron_oom_abort("capture.c:find_captures arr")` | a — REPLACE | Return empty captures array; emit note diagnostic |
| 393  | `iron_oom_abort("capture.c:find_captures name")` | a — REPLACE | strdup fallback to "?" |
| 429  | `iron_oom_abort("capture.c:find_spawn_captures arr")` | a — REPLACE | Return empty; emit note |
| 433  | `iron_oom_abort("capture.c:find_spawn_captures name")` | a — REPLACE | strdup fallback |
| 468  | `iron_oom_abort("capture.c:find_pfor_captures arr")` | a — REPLACE | Return empty; emit note |
| 472  | `iron_oom_abort("capture.c:find_pfor_captures name")` | a — REPLACE | strdup fallback |

### src/analyzer/init_check.c (1 `iron_oom_abort` site)

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 69   | `iron_oom_abort("init_check.c:emit_uninit_error msg")` | a — REPLACE | Static-string fallback ("variable may be uninitialized") |

### src/analyzer/escape.c (1 `IRON_NODE_ASSERT_KIND` site)

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 293  | `IRON_NODE_ASSERT_KIND(ls->expr, IRON_NODE_IDENT)` | a — REPLACE | Leak statement expr can be IRON_NODE_ERROR after parse recovery; graceful skip with diag |

### src/analyzer/concurrency.c (2 `iron_oom_abort` sites)

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 70   | `iron_oom_abort("concurrency.c:emit_err msg")` | a — REPLACE | Static-string fallback ("concurrency error") |
| 78   | `iron_oom_abort("concurrency.c:emit_warn msg")` | a — REPLACE | Static-string fallback ("concurrency warning") |

### src/analyzer/types.c (18 `iron_oom_abort` sites — ALL category (c) MOVE)

Every site here is an `ARENA_ALLOC` / `iron_arena_strdup` result check following a type-constructor call. These are called transitively by every analyzer pass. The best fix is to add a single arena-exhaustion check at pass entry (move the precondition), then treat `iron_type_make_*` as infallible within the pass. Interned primitive singletons already bypass this (they never allocate). Action: MOVE the OOM check to `iron_types_init` / `iron_analyze_buffer` pass-entry budget verification.

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 68   | `iron_oom_abort("types.c:iron_type_make_nullable")` | c — MOVE | Move arena-budget check to pass entry |
| 77   | `iron_oom_abort("types.c:iron_type_make_rc")` | c — MOVE | Move arena-budget check to pass entry |
| 86   | `iron_oom_abort("types.c:iron_type_make_func")` | c — MOVE | Move arena-budget check to pass entry |
| 94   | `iron_oom_abort("types.c:iron_type_make_func param copy")` | c — MOVE | Move arena-budget check to pass entry |
| 105  | `iron_oom_abort("types.c:iron_type_make_array")` | c — MOVE | Move arena-budget check to pass entry |
| 115  | `iron_oom_abort("types.c:iron_type_make_object")` | c — MOVE | Move arena-budget check to pass entry |
| 124  | `iron_oom_abort("types.c:iron_type_make_interface")` | c — MOVE | Move arena-budget check to pass entry |
| 133  | `iron_oom_abort("types.c:iron_type_make_enum")` | c — MOVE | Move arena-budget check to pass entry |
| 142  | `iron_oom_abort("types.c:iron_type_make_generic_param")` | c — MOVE | Move arena-budget check to pass entry |
| 185  | `iron_oom_abort("types.c:tuple_build_mangled_name")` | c — MOVE | Move arena-budget check to pass entry |
| 192  | `iron_oom_abort("types.c:iron_type_make_tuple")` | c — MOVE | Move arena-budget check to pass entry |
| 199  | `iron_oom_abort("types.c:iron_type_make_tuple elem copy")` | c — MOVE | Move arena-budget check to pass entry |
| 312  | `iron_oom_abort("types.c:iron_type_to_string NULLABLE")` | c — MOVE | to_string is debug/diagnostic path; move to caller-level NULL handling |
| 321  | `iron_oom_abort("types.c:iron_type_to_string RC")` | c — MOVE | Same |
| 332  | `iron_oom_abort("types.c:iron_type_to_string ARRAY")` | c — MOVE | Same |
| 356  | `iron_oom_abort("types.c:iron_type_to_string FUNC")` | c — MOVE | Same |
| 392  | `iron_oom_abort("types.c:iron_type_to_string ENUM generic")` | c — MOVE | Same |
| 416  | `iron_oom_abort("types.c:iron_type_to_string TUPLE")` | c — MOVE | Same |

### src/analyzer/web_await_check.c (1 `iron_oom_abort` site)

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 72   | `iron_oom_abort("web_await_check.c:emit_await_error msg")` | a — REPLACE | Static-string fallback ("await error"); only reached when target == IRON_TARGET_WEB |

### src/analyzer/web_top_level_loader_check.c (1 `iron_oom_abort` site)

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 82   | `iron_oom_abort("web_top_level_loader_check.c:emit_loader_error msg")` | a — REPLACE | Static-string fallback ("top-level loader error") |

### src/comptime/comptime.c (18 `iron_oom_abort` sites)

| Line | Call | Category | Justification |
|------|------|----------|---------------|
| 32   | `iron_oom_abort("comptime.c:cval_alloc")` | c — MOVE | Comptime-value allocator; move to per-eval budget cap at entry of iron_comptime_apply |
| 108  | `iron_oom_abort("comptime.c:comptime_cache_read val")` | a — REPLACE | Return NULL to skip cache on OOM; caller already handles missing cache |
| 158  | `iron_oom_abort("comptime.c:comptime_cache_read string buf")` | a — REPLACE | Return NULL to skip cache on OOM |
| 280  | `iron_oom_abort("comptime.c:build_call_trace")` | a — REPLACE | Fallback to static "(call trace unavailable)" message |
| 609  | `iron_oom_abort("comptime.c:iron_comptime_eval_expr read_file error msg")` | a — REPLACE | Static-string fallback ("comptime read_file failed") |
| 625  | `iron_oom_abort("comptime.c:iron_comptime_eval_expr read_file fbuf")` | a — REPLACE | Return comptime error value; emit diag |
| 719  | `iron_oom_abort("comptime.c:iron_comptime_eval_expr ARRAY_LIT elems")` | a — REPLACE | Return comptime error value |
| 738  | `iron_oom_abort("comptime.c:iron_comptime_eval_expr CONSTRUCT field_names")` | a — REPLACE | Return comptime error value |
| 742  | `iron_oom_abort("comptime.c:iron_comptime_eval_expr CONSTRUCT field_vals")` | a — REPLACE | Return comptime error value |
| 1005 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast IntLit")` | c — MOVE | Pre-check arena budget at val_to_ast entry; AST rewrite is bulk op |
| 1013 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast IntLit value")` | a — REPLACE | strdup fallback to "0" |
| 1020 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast FloatLit")` | a — REPLACE | Return ErrorNode; emit diag |
| 1027 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast FloatLit value")` | a — REPLACE | strdup fallback to "0.0" |
| 1034 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast BoolLit")` | a — REPLACE | Return ErrorNode; emit diag |
| 1045 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast StringLit")` | a — REPLACE | Return ErrorNode; emit diag |
| 1051 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast StringLit value")` | a — REPLACE | strdup fallback to "" |
| 1058 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast ArrayLit")` | a — REPLACE | Return ErrorNode; emit diag |
| 1068 | `iron_oom_abort("comptime.c:iron_comptime_val_to_ast ArrayLit elements")` | a — REPLACE | Return ErrorNode; emit diag |

## Plan-consumer index

Future plans consume this audit to decide per-site actions. Cross-references below map HARD-NN requirements to the categories listed above.

- **Plan 02 (HARD-03 no short-circuits + HARD-04 ErrorNode tolerance + HARD-10 IRON_NODE_ASSERT audit)** consumes:
  - Every row in `typecheck.c` / `resolve.c` / `escape.c` IRON_NODE_ASSERT_KIND tables where category is **a — REPLACE** (7 sites total).
  - Every row in `capture.c`, `init_check.c`, `concurrency.c`, `web_await_check.c`, `web_top_level_loader_check.c` where category is **a — REPLACE** (12 sites).
  - Category **b — KEEP** rows are documented but NOT touched by Plan 02.

- **Plan 03 (HARD-05 cancellation polls)** does NOT create new abort sites. It only cross-references this audit when adding cancel-poll sites, to avoid inserting polls between an `ARENA_ALLOC` and its NULL-check.

- **Plan 04 (HARD-08 recursion-depth guard + HARD-09 parser abort audit + HARD-07 pthread_once)** consumes:
  - ALL 114 rows in `src/parser/parser.c` (all category **a — REPLACE**).
  - Category **c — MOVE** rows in `typecheck.c` (2 sites: 3087, 3575).

- **Plan 05 (HARD-02 LSP-mode FS gating + HARD-11 parity fixture)** consumes:
  - Category **a — REPLACE** and **c — MOVE** rows in `comptime.c` (18 sites). The FS-gating sites at 95, 97, 134, 143, 189-191, 197, 199, 603 (per RESEARCH.md Pitfall 4) are orthogonal to this audit but are grep-findable alongside it.
  - All rows in `types.c` (18 sites, all **c — MOVE**) land as a unified arena-budget check in this plan as well, because the per-arena scoping lock (HARD-06) makes a single entry-point audit feasible.

## Notes

- The stub implementation of `iron_analyze_buffer` in Plan 01 delegates to `iron_analyze` unchanged, so none of the abort sites above are removed in Plan 01. The audit is a READ-ONLY deliverable of Plan 01 intended to drive Plans 02–05.
- `src/diagnostics/diagnostics.c`'s `iron_ice` + `iron_node_assert_kind_impl` host the abort mechanism itself; they are not enumerated here because they are the SINK (called-by-others) rather than leaf call sites. Their replacement is out of scope for Phase 1: both remain as backstops for Plan 02–05 category **b** sites.
- `src/runtime/iron_oom.c` defines `iron_oom_abort` itself; also the SINK and not enumerated here.
- The 207 + 23 = 230 total sites match the 190 + 26 + (types.c's 18) ≈ 234 back-of-the-envelope in `.planning/codebase/CONCERNS.md` and PROJECT.md. The minor delta (230 vs ~240) is because CONCERNS.md counted at the codebase snapshot when comptime.c's `iron_comptime_val_to_ast` had fewer literal-constructor branches.
