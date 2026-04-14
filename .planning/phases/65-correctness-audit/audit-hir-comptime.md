# Correctness Audit: HIR + Comptime

Audited directories: `src/hir/` and `src/comptime/`

Files audited (HIR):
- hir.c (642 lines) -- HIR constructors
- hir.h (608 lines) -- HIR type definitions
- hir_lower.c (2135 lines) -- AST-to-HIR lowering
- hir_lower.h (28 lines) -- lowering public API
- hir_to_lir.c (2682 lines) -- HIR-to-LIR lowering (**CRITICAL**: site of SIGSEGV incident)
- hir_to_lir.h (21 lines) -- HIR-to-LIR public API
- hir_print.c (577 lines) -- HIR pretty-printer
- hir_verify.c (569 lines) -- HIR verifier

---

## 1. Blind Casts (AUDIT-01)

### CRITICAL: hir_to_lir.c -- AST Node Blind Casts in collect_mono_enums_node()

The function `collect_mono_enums_node()` (line 460) is the exact site where the Phase 59 SIGSEGV lived.
It casts `Iron_Node *node` to specific AST struct types based on `node->kind`. The original version
used **local struct typedefs** that guessed AST layouts incorrectly (especially for `Iron_IfStmt`
which has `elif_conds`/`elif_bodies` between `body` and `else_body`). This was fixed in Phase 59
by using the real struct types from `parser/ast.h`. The comment at line 521-530 documents the fix.

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 1 | hir_to_lir.c | 465 | `(Iron_FuncDecl *)node` | `node->kind == IRON_NODE_FUNC_DECL` | M | Guarded by switch case -- safe if kind matches layout |
| 2 | hir_to_lir.c | 473 | `(Iron_MethodDecl *)node` | `node->kind == IRON_NODE_METHOD_DECL` | M | Guarded by switch case |
| 3 | hir_to_lir.c | 481 | `(Iron_Block *)node` | `node->kind == IRON_NODE_BLOCK` | M | Guarded by switch case |
| 4 | hir_to_lir.c | 487 | `(Iron_ValDecl *)node` | `node->kind == IRON_NODE_VAL_DECL` | M | Guarded by switch case |
| 5 | hir_to_lir.c | 493 | `(Iron_VarDecl *)node` | `node->kind == IRON_NODE_VAR_DECL` | M | Guarded by switch case |
| 6 | hir_to_lir.c | 499 | `(Iron_EnumConstruct *)node` | `node->kind == IRON_NODE_ENUM_CONSTRUCT` | M | Guarded by switch case |
| 7 | hir_to_lir.c | 506 | `(Iron_CallExpr *)node` | `node->kind == IRON_NODE_CALL` | M | Guarded by switch case |
| 8 | hir_to_lir.c | 514 | `(Iron_MethodCallExpr *)node` | `node->kind == IRON_NODE_METHOD_CALL` | M | Guarded by switch case |
| 9 | hir_to_lir.c | 532 | `(Iron_ReturnStmt *)node` | `node->kind == IRON_NODE_RETURN` | M | Guarded by switch case |
| 10 | hir_to_lir.c | 537 | `(Iron_IfStmt *)node` | `node->kind == IRON_NODE_IF` | M | Guarded -- was the SIGSEGV site, now using real type |
| 11 | hir_to_lir.c | 548 | `(Iron_WhileStmt *)node` | `node->kind == IRON_NODE_WHILE` | M | Guarded by switch case |
| 12 | hir_to_lir.c | 554 | `(Iron_MatchStmt *)node` | `node->kind == IRON_NODE_MATCH` | M | Guarded by switch case |
| 13 | hir_to_lir.c | 562 | `(Iron_AssignStmt *)node` | `node->kind == IRON_NODE_ASSIGN` | M | Guarded by switch case |
| 14 | hir_to_lir.c | 568 | `(Iron_BinaryExpr *)node` | `node->kind == IRON_NODE_BINARY` | M | Guarded by switch case |
| 15 | hir_to_lir.c | 575 | `(Iron_UnaryExpr *)node` | `node->kind == IRON_NODE_UNARY` | M | Guarded by switch case |
| 16 | hir_to_lir.c | 581 | `(Iron_FieldAccess *)node` | `node->kind == IRON_NODE_FIELD_ACCESS` | M | Guarded by switch case |
| 17 | hir_to_lir.c | 587 | `(Iron_Ident *)node` | `node->kind == IRON_NODE_IDENT` | M | Guarded by switch case |

### hir_to_lir.c -- Type Declaration Lowering Casts

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 18 | hir_to_lir.c | 604 | `(Iron_InterfaceDecl *)decl` | `decl->kind == IRON_NODE_INTERFACE_DECL` | M | Guarded by kind check |
| 19 | hir_to_lir.c | 613 | `(Iron_ObjectDecl *)decl` | `decl->kind == IRON_NODE_OBJECT_DECL` | M | Guarded by kind check |
| 20 | hir_to_lir.c | 625 | `(Iron_EnumDecl *)decl` | `decl->kind == IRON_NODE_ENUM_DECL` | M | Guarded by kind check |
| 21 | hir_to_lir.c | 656 | `(Iron_FuncDecl *)decl` | `decl->kind == IRON_NODE_FUNC_DECL` | M | Guarded by kind check |
| 22 | hir_to_lir.c | 675 | `(Iron_Param *)fd->params[p]` | Implicit: params are Iron_Param | M | No runtime guard; relies on parser correctness |
| 23 | hir_to_lir.c | 976 | `(Iron_Field *)od->fields[fi]` | Implicit: fields are Iron_Field | M | No runtime guard |
| 24 | hir_to_lir.c | 980 | `(Iron_TypeAnnotation *)fld->type_ann` | Checks `fld->type_ann->kind != IRON_NODE_TYPE_ANNOTATION` | L | Guarded by subsequent kind check |
| 25 | hir_to_lir.c | 1839 | `(Iron_EnumVariant *)match_ed->variants[j]` | Implicit: variants are Iron_EnumVariant | M | No runtime guard |
| 26 | hir_to_lir.c | 1855 | `(Iron_EnumVariant *)match_ed->variants[j]` | Implicit | M | Same as #25 |
| 27 | hir_to_lir.c | 1907 | `(Iron_EnumVariant *)match_ed->variants[j]` | Implicit | M | Same as #25 |

### hir_lower.c -- Iron_ExprNode Common Layout Cast

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 28 | hir_lower.c | 109 | `((Iron_ExprNode *)node)->resolved_type` | `node != NULL` only | **H** | `Iron_ExprNode` is a locally-defined struct assuming common prefix layout across ALL AST node types. If any node type violates `{span, kind, resolved_type}` prefix layout, this silently reads garbage. Add a static_assert or compile-time check that each AST type has compatible prefix |

### hir_lower.c -- AST Node Casts in Statement/Expression Lowering

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 29 | hir_lower.c | 166 | `(Iron_TypeAnnotation *)ann_node` | `ann_node->kind == IRON_NODE_TYPE_ANNOTATION` | M | Guarded |
| 30 | hir_lower.c | 230 | `(Iron_ObjectDecl *)decl` | `decl->kind == IRON_NODE_OBJECT_DECL` | M | Guarded |
| 31 | hir_lower.c | 236 | `(Iron_EnumDecl *)decl` | `decl->kind == IRON_NODE_ENUM_DECL` | M | Guarded |
| 32 | hir_lower.c | 242 | `(Iron_InterfaceDecl *)decl` | `decl->kind == IRON_NODE_INTERFACE_DECL` | M | Guarded |
| 33 | hir_lower.c | 290 | `(Iron_Param *)params[p]` | Implicit | M | No runtime guard |
| 34 | hir_lower.c | 410 | `(Iron_EnumVariant *)ed->variants[i]` | Implicit | M | No runtime guard |
| 35 | hir_lower.c | 416 | `(Iron_EnumVariant *)ed->variants[vi]` | Bounds-checked by `vi < 0` earlier | M | Guarded |
| 36 | hir_lower.c | 447 | `(Iron_Pattern *)nested` | `nested->kind == IRON_NODE_PATTERN` | M | Guarded |
| 37 | hir_lower.c | 476 | `(Iron_ValDecl *)node` | `node->kind == IRON_NODE_VAL_DECL` | M | Guarded by switch |
| 38 | hir_lower.c | 557 | `(Iron_SpawnStmt *)vd->init` | `vd->init->kind == IRON_NODE_SPAWN` | M | Guarded by if-check |
| 39 | hir_lower.c | 618 | `(Iron_VarDecl *)node` | `node->kind == IRON_NODE_VAR_DECL` | M | Guarded by switch |
| 40 | hir_lower.c | 631 | `(Iron_AssignStmt *)node` | `node->kind == IRON_NODE_ASSIGN` | M | Guarded by switch |
| 41 | hir_lower.c | 655 | `(Iron_IfStmt *)node` | `node->kind == IRON_NODE_IF` | M | Guarded |
| 42 | hir_lower.c | 701 | `(Iron_WhileStmt *)node` | `node->kind == IRON_NODE_WHILE` | M | Guarded |
| 43 | hir_lower.c | 712 | `(Iron_ForStmt *)node` | `node->kind == IRON_NODE_FOR` | M | Guarded |
| 44 | hir_lower.c | 777 | `(Iron_BinaryExpr *)fs->iterable` | `fs->iterable->kind == IRON_NODE_BINARY` | M | Guarded by if-check |
| 45 | hir_lower.c | 802 | `(Iron_BinaryExpr *)fs->iterable` | Guarded by earlier check | M | Guarded |
| 46 | hir_lower.c | 876 | `(Iron_MatchStmt *)node` | `node->kind == IRON_NODE_MATCH` | M | Guarded |
| 47 | hir_lower.c | 882 | `(Iron_MatchCase *)ms->cases[i]` | Implicit | M | No runtime guard |
| 48 | hir_lower.c | 888 | `(Iron_Pattern *)mc->pattern` | `mc->pattern->kind == IRON_NODE_PATTERN` | M | Guarded |
| 49 | hir_lower.c | 926 | `(Iron_ReturnStmt *)node` | `node->kind == IRON_NODE_RETURN` | M | Guarded |
| 50 | hir_lower.c | 935 | `(Iron_DeferStmt *)node` | `node->kind == IRON_NODE_DEFER` | M | Guarded |
| 51 | hir_lower.c | 952 | `(Iron_FreeStmt *)node` | `node->kind == IRON_NODE_FREE` | M | Guarded |
| 52 | hir_lower.c | 961 | `(Iron_LeakStmt *)node` | `node->kind == IRON_NODE_LEAK` | M | Guarded |
| 53 | hir_lower.c | 970 | `(Iron_SpawnStmt *)node` | `node->kind == IRON_NODE_SPAWN` | M | Guarded |
| 54 | hir_lower.c | 1060 | `(Iron_IntLit *)node` | `node->kind == IRON_NODE_INT_LIT` | M | Guarded |
| 55 | hir_lower.c | 1067 | `(Iron_FloatLit *)node` | `node->kind == IRON_NODE_FLOAT_LIT` | M | Guarded |
| 56 | hir_lower.c | 1074 | `(Iron_StringLit *)node` | `node->kind == IRON_NODE_STRING_LIT` | M | Guarded |
| 57 | hir_lower.c | 1080 | `(Iron_InterpString *)node` | `node->kind == IRON_NODE_INTERP_STRING` | M | Guarded |
| 58 | hir_lower.c | 1094 | `(Iron_BoolLit *)node` | `node->kind == IRON_NODE_BOOL_LIT` | M | Guarded |
| 59 | hir_lower.c | 1100 | `(Iron_NullLit *)node` | `node->kind == IRON_NODE_NULL_LIT` | M | Guarded |
| 60 | hir_lower.c | 1106 | `(Iron_Ident *)node` | `node->kind == IRON_NODE_IDENT` | M | Guarded |
| 61 | hir_lower.c | 1163 | `(Iron_BinaryExpr *)node` | `node->kind == IRON_NODE_BINARY` | M | Guarded |
| 62 | hir_lower.c | 1173 | `(Iron_UnaryExpr *)node` | `node->kind == IRON_NODE_UNARY` | M | Guarded |
| 63 | hir_lower.c | 1187 | `(Iron_CallExpr *)node` | `node->kind == IRON_NODE_CALL` | M | Guarded |
| 64 | hir_lower.c | 1194 | `(Iron_Ident *)ce->callee` | `ce->callee->kind == IRON_NODE_IDENT` | M | Guarded by if-check |
| 65 | hir_lower.c | 1221 | `(Iron_MethodCallExpr *)node` | `node->kind == IRON_NODE_METHOD_CALL` | M | Guarded |
| 66 | hir_lower.c | 1237 | `(Iron_FieldAccess *)node` | `node->kind == IRON_NODE_FIELD_ACCESS` | M | Guarded |
| 67 | hir_lower.c | 1245 | `(Iron_IndexExpr *)node` | `node->kind == IRON_NODE_INDEX` | M | Guarded |
| 68 | hir_lower.c | 1253 | `(Iron_SliceExpr *)node` | `node->kind == IRON_NODE_SLICE` | M | Guarded |
| 69 | hir_lower.c | 1262 | `(Iron_LambdaExpr *)node` | `node->kind == IRON_NODE_LAMBDA` | M | Guarded |
| 70 | hir_lower.c | 1335 | `(Iron_HeapExpr *)node` | `node->kind == IRON_NODE_HEAP` | M | Guarded |
| 71 | hir_lower.c | 1343 | `(Iron_RcExpr *)node` | `node->kind == IRON_NODE_RC` | M | Guarded |
| 72 | hir_lower.c | 1350 | `(Iron_ConstructExpr *)node` | `node->kind == IRON_NODE_CONSTRUCT` | M | Guarded |
| 73 | hir_lower.c | 1375 | `(Iron_ArrayLit *)node` | `node->kind == IRON_NODE_ARRAY_LIT` | M | Guarded |
| 74 | hir_lower.c | 1414 | `(Iron_AwaitExpr *)node` | `node->kind == IRON_NODE_AWAIT` | M | Guarded |
| 75 | hir_lower.c | 1421 | `(Iron_ComptimeExpr *)node` | `node->kind == IRON_NODE_COMPTIME` | M | Guarded |
| 76 | hir_lower.c | 1427 | `(Iron_IsExpr *)node` | `node->kind == IRON_NODE_IS` | M | Guarded |
| 77 | hir_lower.c | 1439 | `(Iron_EnumConstruct *)node` | `node->kind == IRON_NODE_ENUM_CONSTRUCT` | M | Guarded |
| 78 | hir_lower.c | 1467 | `(Iron_Pattern *)node` | `node->kind == IRON_NODE_PATTERN` | M | Guarded |
| 79 | hir_lower.c | 1532 | `(Iron_FuncDecl *)decl` | `decl->kind == IRON_NODE_FUNC_DECL` | M | Guarded |
| 80 | hir_lower.c | 1550 | `(Iron_MethodDecl *)decl` | `decl->kind == IRON_NODE_METHOD_DECL` | M | Guarded |
| 81 | hir_lower.c | 1604 | `(Iron_ObjectDecl *)d` | `d->kind == IRON_NODE_OBJECT_DECL` | M | Guarded |
| 82 | hir_lower.c | 1610 | `(Iron_EnumDecl *)d` | `d->kind == IRON_NODE_ENUM_DECL` | M | Guarded |
| 83 | hir_lower.c | 1646 | `(Iron_ValDecl *)decl` | `decl->kind == IRON_NODE_VAL_DECL` | M | Guarded |
| 84 | hir_lower.c | 1654 | `(Iron_VarDecl *)decl` | `decl->kind == IRON_NODE_VAR_DECL` | M | Guarded |
| 85 | hir_lower.c | 1789 | `(Iron_LambdaExpr *)lp->ast_node` | `lp->kind == LIFT_LAMBDA` | M | Guarded by switch |
| 86 | hir_lower.c | 1887 | `(Iron_SpawnStmt *)lp->ast_node` | `lp->kind == LIFT_SPAWN` | M | Guarded by switch |
| 87 | hir_lower.c | 1978 | `(Iron_ForStmt *)lp->ast_node` | `lp->kind == LIFT_PARALLEL_FOR` | M | Guarded by switch |
| 88 | hir_lower.c | 1361 | `((Iron_Field *)od->fields[i])->name` | Implicit | M | No runtime guard |
| 89 | hir_lower.c | 1271 | `(Iron_Param *)le->params[p]` | Implicit | M | No runtime guard |
| 90 | hir_lower.c | 1584 | `(Iron_Param *)md->params[p]` | Implicit | M | No runtime guard |
| 91 | hir_lower.c | 1624 | `(Iron_Param *)md->params[p]` | Implicit | M | No runtime guard |
| 92 | hir_lower.c | 1696 | `(Iron_Param *)fd->params[p]` | Implicit | M | No runtime guard |
| 93 | hir_lower.c | 1894 | `(Iron_ReturnStmt *)sblk->stmts[ri]` | `sblk->stmts[ri]->kind == IRON_NODE_RETURN` | M | Guarded |
| 94 | hir_lower.c | 1897 | `(Iron_IntLit *)rs->value` | `rs->value->kind == IRON_NODE_INT_LIT` | M | Guarded by if-check |

### hir_lower.c -- Block Body Casts (body -> Iron_Block*)

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 95 | hir_lower.c | 570 | `(Iron_Block *)ss->body` | Implicit: bodies are Iron_Block | M | No kind guard; relies on parser |
| 96 | hir_lower.c | 659 | `(Iron_Block *)is->body` | Implicit | M | Same |
| 97 | hir_lower.c | 669 | `(Iron_Block *)is->else_body` | Implicit | M | Same |
| 98 | hir_lower.c | 676 | `(Iron_Block *)is->elif_bodies[ei]` | Implicit | M | Same |
| 99 | hir_lower.c | 690 | `(Iron_Block *)is->else_body` | Implicit | M | Same |
| 100 | hir_lower.c | 704 | `(Iron_Block *)ws->body` | Implicit | M | Same |
| 101 | hir_lower.c | 725 | `(Iron_Block *)fs->body` | Implicit | M | Same |
| 102 | hir_lower.c | 829 | `(Iron_Block *)fs->body` | Implicit | M | Same |
| 103 | hir_lower.c | 864 | `(Iron_Block *)fs->body` | Implicit | M | Same |
| 104 | hir_lower.c | 892 | `(Iron_Block *)mc->body` | Implicit | M | Same |
| 105 | hir_lower.c | 895 | `(Iron_Block *)mc->body` | Implicit | M | Same |
| 106 | hir_lower.c | 909 | `(Iron_Block *)ms->else_body` | Implicit | M | Same |
| 107 | hir_lower.c | 984 | `(Iron_Block *)ss->body` | Implicit | M | Same |
| 108 | hir_lower.c | 1021 | `(Iron_Block *)node` | `node->kind == IRON_NODE_BLOCK` (implicit) | M | Switch default; any unhandled kind reaches here |
| 109 | hir_lower.c | 1281 | `(Iron_Block *)le->body` | Implicit | M | Same |
| 110 | hir_lower.c | 1568 | `(Iron_Block *)md->body` | Implicit | M | Same |
| 111 | hir_lower.c | 1708 | `(Iron_Block *)fd->body` | Implicit | M | Same |
| 112 | hir_lower.c | 1753 | `(Iron_Block *)md->body` | Implicit | M | Same |
| 113 | hir_lower.c | 1876 | `(Iron_Block *)le->body` | Implicit | M | Same |
| 114 | hir_lower.c | 1891 | `(Iron_Block *)ss->body` | Implicit | M | Same |
| 115 | hir_lower.c | 1967 | `(Iron_Block *)ss->body` | Implicit | M | Same |
| 116 | hir_lower.c | 2048 | `(Iron_Block *)fs->body` | Implicit | M | Same |

### hir_verify.c -- const-stripping Casts

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 117 | hir_verify.c | 59 | `(IronHIR_Module *)mod` -- const-cast | `mod` non-null | L | Safe const-cast for API compat; function is read-only |
| 118 | hir_verify.c | 333 | `(IronHIR_Module *)mod` -- const-cast | `mod` non-null | L | Same |

### hir.c -- malloc Cast

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 119 | hir.c | 12 | `(Iron_Arena *)malloc(sizeof(Iron_Arena))` | No NULL check | M | Check for NULL return from malloc |

### Local Struct Typedef Shadows (CRITICAL Pattern)

The original `collect_mono_enums_node` in hir_to_lir.c used local struct typedefs like
`typedef struct { ... } LocalIfStmt;` that guessed the layout of real AST types. This caused the
SIGSEGV when `Iron_IfStmt` was accessed via a struct that omitted `elif_conds`/`elif_bodies`.

**Current status**: FIXED. All casts in `collect_mono_enums_node` now use the real types from
`parser/ast.h`. The comment at hir_to_lir.c:521-530 documents the fix. No local struct typedef
shadows remain in any HIR source file.

No current local struct typedef shadows found: **0 H-severity** for this specific pattern.

---

## 2. Enum Switch Exhaustiveness (AUDIT-02)

### hir_lower.c Switches

| # | File | Line | Switch Over | Missing Cases | Severity | Suggested Fix |
|---|------|------|-------------|---------------|----------|---------------|
| 1 | hir_lower.c | 326 | `Iron_OpKind` (ast_op_to_hir_binop) | Has `default: return IRON_HIR_BINOP_ADD` -- silently maps unknown ops to ADD | M | Return error/poison value or assert |
| 2 | hir_lower.c | 351 | `Iron_OpKind` (compound_assign_base_op) | Has `default: return IRON_TOK_PLUS` -- silently maps unknown ops to PLUS | M | Return error value |
| 3 | hir_lower.c | 472 | `Iron_NodeKind` (lower_stmt_hir) | `default` catches all expression nodes -- by design for expr-as-statement | L | Intentional fallthrough |
| 4 | hir_lower.c | 1056 | `Iron_NodeKind` (lower_expr_hir) | `default` at 1492 returns NULL -- safe but silent | L | Add diagnostic for unknown nodes |
| 5 | hir_lower.c | 1175 | `Iron_OpKind` (unary op mapping) | `default` maps to IRON_HIR_UNOP_NEG | M | Should be assertion/error |
| 6 | hir_lower.c | 1529 | `Iron_NodeKind` (lower_module_decls_hir) | `default` skips non-func/method/val/var decl kinds | L | By design |
| 7 | hir_lower.c | 1786 | `LiftKind` (lift_pending) | All 3 cases covered (LAMBDA, SPAWN, PARALLEL_FOR) | L | Exhaustive |

### hir_to_lir.c Switches

| # | File | Line | Switch Over | Missing Cases | Severity | Suggested Fix |
|---|------|------|-------------|---------------|----------|---------------|
| 8 | hir_to_lir.c | 463 | `Iron_NodeKind` (collect_mono_enums_node) | Only handles 17 node kinds; `default: break` skips IRON_NODE_FOR, IRON_NODE_DEFER, IRON_NODE_LAMBDA, IRON_NODE_INDEX, IRON_NODE_SLICE, IRON_NODE_ARRAY_LIT, IRON_NODE_CONSTRUCT, etc. | M | Mono enums inside index/slice/array_lit/lambda bodies are silently missed. Add cases for IRON_NODE_FOR, IRON_NODE_LAMBDA, IRON_NODE_ARRAY_LIT at minimum |
| 9 | hir_to_lir.c | 697 | `IronHIR_BinOp` (hir_binop_to_lir) | All 18 cases covered + `default: POISON` | L | Exhaustive with safe default |
| 10 | hir_to_lir.c | 807 | `IronHIR_ExprKind` (lower_expr) | All 28 expr kinds handled; `default: poison` at 1460 | L | Exhaustive |
| 11 | hir_to_lir.c | 879 | `IronHIR_UnOp` (unary mapping) | All 3 cases covered + `default: POISON` | L | Exhaustive |
| 12 | hir_to_lir.c | 1012 | `Iron_TypeKind` (elem_suffix) | Missing: IRON_TYPE_FLOAT32, IRON_TYPE_FLOAT64, IRON_TYPE_INT8, IRON_TYPE_INT16, IRON_TYPE_INT64, IRON_TYPE_NULLABLE, IRON_TYPE_FUNC, IRON_TYPE_TUPLE | M | Narrow-int and tuple element arrays will get wrong suffix |
| 13 | hir_to_lir.c | 1530 | `IronHIR_StmtKind` (lower_stmt) | All 13 stmt kinds handled; `default: break` at 2093 | L | Exhaustive |

### hir_print.c Switches

| # | File | Line | Switch Over | Missing Cases | Severity | Suggested Fix |
|---|------|------|-------------|---------------|----------|---------------|
| 14 | hir_print.c | 39 | `IronHIR_BinOp` | All 18 cases + `default: "?"` | L | Exhaustive |
| 15 | hir_print.c | 64 | `IronHIR_UnOp` | All 3 cases + `default: "?"` | L | Exhaustive |
| 16 | hir_print.c | 116 | `IronHIR_StmtKind` | All 13 cases + `default: "UnknownStmt"` | L | Exhaustive |
| 17 | hir_print.c | 270 | `IronHIR_ExprKind` | All 28 cases + `default: "UnknownExpr"` | L | Exhaustive |

### hir_verify.c Switches

| # | File | Line | Switch Over | Missing Cases | Severity | Suggested Fix |
|---|------|------|-------------|---------------|----------|---------------|
| 18 | hir_verify.c | 145 | `IronHIR_StmtKind` | All 13 cases + `default: break` | L | Exhaustive |
| 19 | hir_verify.c | 307 | `IronHIR_ExprKind` | Missing: IRON_HIR_EXPR_ENUM_CONSTRUCT and IRON_HIR_EXPR_PATTERN fall through to `default: break` -- their sub-expressions (args, nested patterns) are not verified | M | Add cases for ENUM_CONSTRUCT and PATTERN to verify args/nested |

---

## 3. Null Safety (AUDIT-03)

| # | File | Line | Access Pattern | Guard | Severity | Suggested Fix |
|---|------|------|----------------|-------|----------|---------------|
| 1 | hir_to_lir.c | 466 | `fd->resolved_return_type` | NULL-safe: register_mono_enum checks `!type` | L | Safe |
| 2 | hir_to_lir.c | 482 | `blk->stmts[i]` | No NULL check on individual stmts | M | Add `if (!blk->stmts[i]) continue;` |
| 3 | hir_to_lir.c | 540 | `iff->elif_conds[i]`, `iff->elif_bodies[i]` | No NULL check; depends on parser | M | Add NULL guards |
| 4 | hir_to_lir.c | 557 | `mat->cases[i]` | No NULL check | M | Depends on parser correctness |
| 5 | hir_to_lir.c | 970 | `expr->method_call.object->type` | Guarded: `obj_type` used only after NULL check | L | Safe |
| 6 | hir_to_lir.c | 1122 | `ctx->hir->name_table` accessed with VarId as index | Bounds check via `arrlen` | L | Safe |
| 7 | hir_lower.c | 109 | `((Iron_ExprNode *)node)->resolved_type` | `node != NULL` checked | L | May read NULL resolved_type; callers handle NULL |
| 8 | hir_lower.c | 396 | `pat->enum_name` | Guarded by `if (!enum_type && pat->enum_name)` | L | Safe |
| 9 | hir_lower.c | 403 | `enum_type->enu.decl` | Returns early if NULL | L | Safe |
| 10 | hir_lower.c | 419 | `pat->binding_names`, `pat->nested_patterns` | NULL-checked before use | L | Safe |
| 11 | hir_lower.c | 1088 | `is->parts[i]` passed to lower_expr_hir | No NULL check; if NULL, lower_expr_hir returns NULL | L | lower_expr_hir handles NULL |
| 12 | hir_lower.c | 1389 | `od->fields` indexed without NULL check on `od` | `od` checked via `obj_type->object.decl` | L | Safe |
| 13 | hir_print.c | 109 | `block->stmts[i]` | print_stmt handles NULL | L | Safe |
| 14 | hir_verify.c | 384 | `expr->call.args[i]` | No NULL check before verify_expr; verify_expr handles NULL | L | Safe |
| 15 | hir_verify.c | 398 | `expr->method_call.args[i]` | Same | L | Safe |
| 16 | hir_verify.c | 500 | `expr->construct.field_values[i]` | Guarded by `if (expr->construct.field_values)` | L | Safe |
| 17 | hir_verify.c | 507 | `expr->array_lit.elements[i]` | No NULL check per element | M | Add NULL check before verify_expr |

---

## 4. Arena Lifetimes (AUDIT-04)

| # | File | Line | Pattern | Severity | Description | Suggested Fix |
|---|------|------|---------|----------|-------------|---------------|
| 1 | hir.c | 12 | `malloc(sizeof(Iron_Arena))` arena on heap | L | Arena allocated on heap, owned by module -- freed in `iron_hir_module_destroy` | Correct ownership |
| 2 | hir.c | 29 | `arrput(mod->name_table, sentinel)` | M | name_table is a stb_ds array (heap-allocated) holding VarInfo structs. The `name` fields point into AST arena strings. If AST arena freed while HIR module alive, name pointers dangle | Document arena lifetime dependency: HIR module must not outlive AST |
| 3 | hir_lower.c | 173 | `iron_arena_alloc(ctx->module->arena, ...)` for tuple elem_types | L | Allocated in HIR arena -- correctly owned |
| 4 | hir_lower.c | 437 | `iron_arena_strdup(mod->arena, slot_field, ...)` | L | String allocated in HIR arena -- correctly owned |
| 5 | hir_to_lir.c | 80 | `iron_arena_alloc(ctx->lir_arena, ...)` for labels | L | LIR arena owns labels -- correctly owned |
| 6 | hir_to_lir.c | 398 | `calloc((size_t)n, sizeof(IronLIR_BlockId *))` for dominance frontiers | M | Heap-allocated DF arrays; `arrfree` called for each entry and then `free(df)` at end of `ssa_construct_func`. Correct but fragile if early return | Add cleanup-on-error path |
| 7 | hir_to_lir.c | 614 | `iron_type_make_object(ctx->lir_arena, obj)` | L | Type created in LIR arena -- correctly owned |
| 8 | hir_lower.c | 1088 | stb_ds `parts` array ownership: "do NOT arrfree" comment | M | Ownership transfer from stb_ds to HIR expr. If lower_expr_hir fails and returns NULL partway, parts array leaks | Add cleanup path for partial failure |
| 9 | hir_print.c | 558 | `Iron_Arena tmp = iron_arena_create(4096)` | L | Temporary arena correctly freed at line 574 | Safe |
| 10 | hir_print.c | 568 | `malloc(len + 1)` for result string | L | Caller must free; documented as malloc'd | Correct API contract |

---

## 5. Integer Safety (AUDIT-05)

| # | File | Line | Pattern | Severity | Suggested Fix |
|---|------|------|---------|----------|---------------|
| 1 | hir.c | 47 | `mod->next_var_id++` | L | uint32_t overflow at ~4 billion vars -- unreachable in practice |
| 2 | hir.c | 59 | `(ptrdiff_t)id >= arrlen(mod->name_table)` | L | VarId is uint32_t; comparison with ptrdiff_t is safe |
| 3 | hir.c | 80 | `mod->func_count = (int)arrlen(mod->funcs)` | L | int truncation if >2B functions -- unreachable |
| 4 | hir.c | 94 | `block->stmt_count = (int)arrlen(block->stmts)` | L | Same |
| 5 | hir_lower.c | 135 | `for (int d = ctx->scope_depth - 1; d >= 0; d--)` | L | scope_depth is int; loop is correct |
| 6 | hir_lower.c | 425 | `snprintf(slot_field, sizeof(slot_field), ...)` | M | Fixed 256-byte buffer; very long nested enum variant paths could truncate -- silent data loss | Use dynamic allocation or check for truncation |
| 7 | hir_to_lir.c | 77 | `snprintf(buf, sizeof(buf), ...)` for labels | L | Fixed 64-byte buffer; label_counter overflow at INT_MAX -- unreachable |
| 8 | hir_to_lir.c | 104-112 | `entry->instr_count` used as array index | L | instr_count maintained by LIR functions; consistent |
| 9 | hir_to_lir.c | 381-389 | `int limit = 10000` in dominator walk | L | Prevents infinite loop; 10K is generous bound |
| 10 | hir_to_lir.c | 412 | `int limit = 10000` in DF computation | L | Same pattern |
| 11 | hir_to_lir.c | 1897 | `(int)arm->pattern->int_lit.value` in match case value | M | int64_t truncated to int; match values > INT_MAX silently wrong | Use int64_t for case_values or check range |
| 12 | hir_verify.c | 61 | `snprintf(msg, sizeof(msg), ...)` | L | 256-byte fixed buffer; long var names truncated -- cosmetic only |

---

## 6. Allocation Error Handling (AUDIT-06)

| # | File | Line | Allocation | NULL check | Severity | Suggested Fix |
|---|------|------|------------|------------|----------|---------------|
| 1 | hir.c | 12 | `malloc(sizeof(Iron_Arena))` | **None** | M | Add NULL check; return NULL on OOM |
| 2 | hir.c | 15 | `ARENA_ALLOC(arena, IronHIR_Module)` | **None** | M | arena_alloc bumps pointer; NULL on OOM goes undetected |
| 3 | hir.c | 29 | `arrput(mod->name_table, sentinel)` | **None** | M | stb_ds realloc failure undefined |
| 4 | hir.c | 68 | `ARENA_ALLOC(mod->arena, IronHIR_Func)` | **None** | M | Same |
| 5 | hir.c | 79 | `arrput(mod->funcs, func)` | **None** | M | Same |
| 6 | hir.c | 86 | `ARENA_ALLOC(mod->arena, IronHIR_Block)` | **None** | M | Same |
| 7 | hir.c | 93 | `arrput(block->stmts, stmt)` | **None** | M | Same |
| 8-54 | hir.c | 103-641 | All 47 ARENA_ALLOC calls for stmts/exprs | **None** | M | Consistent pattern: no OOM check on arena alloc |
| 55 | hir_lower.c | 173 | `iron_arena_alloc(...)` for tuple types | **None** | M | No NULL check |
| 56 | hir_lower.c | 192 | `iron_arena_alloc(...)` for func param types | **None** | M | Same |
| 57 | hir_lower.c | 285 | `iron_arena_alloc(...)` for HIR params | **None** | M | Same |
| 58-72 | hir_lower.c | various | All iron_arena_alloc calls (15 sites) | **None** | M | Consistent pattern |
| 73 | hir_to_lir.c | 398 | `calloc(n, sizeof(...))` for DF arrays | NULL check present at line 399 | L | Correctly checked |
| 74 | hir_to_lir.c | 666-670 | `iron_arena_alloc(...)` for param types/LIR params | **None** | M | No NULL check |
| 75-82 | hir_to_lir.c | various | All iron_arena_alloc calls (10 sites) | **None** | M | Consistent pattern |
| 83 | hir_print.c | 568 | `malloc(len + 1)` | Checked at line 569 | L | Correctly checked |

**Summary for AUDIT-06**: 82 M-severity allocation sites with no OOM handling across HIR files. The arena allocator itself does not return NULL on failure (it aborts or has undefined behavior depending on the underlying implementation). This is a systemic design choice, not per-file.

---

## 7. Cross-Platform (AUDIT-08)

| # | File | Line | Pattern | Severity | Suggested Fix |
|---|------|------|---------|----------|---------------|
| 1 | hir_to_lir.c | 378 | `__attribute__((unused))` on `hirlir_dominates` | L | GCC/Clang-specific. MSVC ignores unknown attributes. Add `#ifdef __GNUC__` guard or use `(void)hirlir_dominates;` |
| 2 | hir_lower.c | 174 | `_Alignof(Iron_Type *)` | L | C11 standard; supported by MSVC 2019+. No issue for current targets |

**Summary for AUDIT-08**: Only 1 GCC/Clang-specific attribute found. No POSIX-specific code, no platform-specific headers, no #ifdef guards needed. The HIR layer is highly portable.

---

## Comptime Audit (src/comptime/)

Files audited:
- comptime.c (1253 lines) -- tree-walking AST interpreter + cache + AST replacement walker
- comptime.h (103 lines) -- comptime value types, context, public API

### 1. Blind Casts (AUDIT-01) -- Comptime

#### comptime.c -- Expression Evaluator Casts

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 120 | comptime.c | 315 | `(Iron_IntLit *)node` | `node->kind == IRON_NODE_INT_LIT` | M | Guarded by switch |
| 121 | comptime.c | 324 | `(Iron_FloatLit *)node` | `node->kind == IRON_NODE_FLOAT_LIT` | M | Guarded |
| 122 | comptime.c | 333 | `(Iron_BoolLit *)node` | `node->kind == IRON_NODE_BOOL_LIT` | M | Guarded |
| 123 | comptime.c | 338 | `(Iron_StringLit *)node` | `node->kind == IRON_NODE_STRING_LIT` | M | Guarded |
| 124 | comptime.c | 350 | `(Iron_Ident *)node` | `node->kind == IRON_NODE_IDENT` | M | Guarded |
| 125 | comptime.c | 362 | `(Iron_BinaryExpr *)node` | `node->kind == IRON_NODE_BINARY` | M | Guarded |
| 126 | comptime.c | 468 | `(Iron_UnaryExpr *)node` | `node->kind == IRON_NODE_UNARY` | M | Guarded |
| 127 | comptime.c | 491 | `(Iron_CallExpr *)node` | `node->kind == IRON_NODE_CALL` | M | Guarded |
| 128 | comptime.c | 499 | `(Iron_Ident *)call->callee` | `call->callee->kind == IRON_NODE_IDENT` | M | Guarded by if-check |
| 129 | comptime.c | 564 | `(Iron_FuncDecl *)sym->decl_node` | `sym->sym_kind == IRON_SYM_FUNCTION` | M | Guarded; extra `fn->kind` check at line 565 |
| 130 | comptime.c | 600 | `(Iron_Param *)fn->params[i]` | Implicit | M | No runtime guard |
| 131 | comptime.c | 635 | `(Iron_ArrayLit *)node` | `node->kind == IRON_NODE_ARRAY_LIT` | M | Guarded |
| 132 | comptime.c | 652 | `(Iron_ConstructExpr *)node` | `node->kind == IRON_NODE_CONSTRUCT` | M | Guarded |
| 133 | comptime.c | 668 | `(Iron_ObjectDecl *)sym->decl_node` | `sym->decl_node->kind == IRON_NODE_OBJECT_DECL` | M | Guarded |
| 134 | comptime.c | 670 | `(Iron_Field *)od->fields[i]` | Implicit; bounded by `i < od->field_count` | M | No kind guard |
| 135 | comptime.c | 702 | `(Iron_ComptimeExpr *)node` | `node->kind == IRON_NODE_COMPTIME` | M | Guarded |

#### comptime.c -- Statement Evaluator Casts

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 136 | comptime.c | 727 | `(Iron_Block *)node` | `node->kind == IRON_NODE_BLOCK` | M | Guarded |
| 137 | comptime.c | 735 | `(Iron_ValDecl *)node` | `node->kind == IRON_NODE_VAL_DECL` | M | Guarded |
| 138 | comptime.c | 746 | `(Iron_VarDecl *)node` | `node->kind == IRON_NODE_VAR_DECL` | M | Guarded |
| 139 | comptime.c | 757 | `(Iron_AssignStmt *)node` | `node->kind == IRON_NODE_ASSIGN` | M | Guarded |
| 140 | comptime.c | 761 | `(Iron_Ident *)as->target` | `as->target->kind == IRON_NODE_IDENT` | M | Guarded |
| 141 | comptime.c | 777 | `(Iron_ReturnStmt *)node` | `node->kind == IRON_NODE_RETURN` | M | Guarded |
| 142 | comptime.c | 788 | `(Iron_IfStmt *)node` | `node->kind == IRON_NODE_IF` | M | Guarded |
| 143 | comptime.c | 820 | `(Iron_WhileStmt *)node` | `node->kind == IRON_NODE_WHILE` | M | Guarded |
| 144 | comptime.c | 842 | `(Iron_ForStmt *)node` | `node->kind == IRON_NODE_FOR` | M | Guarded |

#### comptime.c -- AST Replacement Walker Casts

| # | File | Line | Cast Expression | Guard | Severity | Suggested Fix |
|---|------|------|-----------------|-------|----------|---------------|
| 145 | comptime.c | 990 | `(Iron_ComptimeExpr *)node` | `node->kind == IRON_NODE_COMPTIME` | M | Guarded |
| 146 | comptime.c | 1034 | `(Iron_Program *)node` | `node->kind == IRON_NODE_PROGRAM` | M | Guarded |
| 147 | comptime.c | 1039 | `(Iron_FuncDecl *)node` | `node->kind == IRON_NODE_FUNC_DECL` | M | Guarded |
| 148 | comptime.c | 1044 | `(Iron_MethodDecl *)node` | `node->kind == IRON_NODE_METHOD_DECL` | M | Guarded |
| 149 | comptime.c | 1049 | `(Iron_Block *)node` | `node->kind == IRON_NODE_BLOCK` | M | Guarded |
| 150 | comptime.c | 1054 | `(Iron_ValDecl *)node` | `node->kind == IRON_NODE_VAL_DECL` | M | Guarded |
| 151 | comptime.c | 1059 | `(Iron_VarDecl *)node` | `node->kind == IRON_NODE_VAR_DECL` | M | Guarded |
| 152 | comptime.c | 1064 | `(Iron_AssignStmt *)node` | `node->kind == IRON_NODE_ASSIGN` | M | Guarded |
| 153 | comptime.c | 1069 | `(Iron_ReturnStmt *)node` | `node->kind == IRON_NODE_RETURN` | M | Guarded |
| 154 | comptime.c | 1074 | `(Iron_IfStmt *)node` | `node->kind == IRON_NODE_IF` | M | Guarded |
| 155 | comptime.c | 1083 | `(Iron_WhileStmt *)node` | `node->kind == IRON_NODE_WHILE` | M | Guarded |
| 156 | comptime.c | 1089 | `(Iron_ForStmt *)node` | `node->kind == IRON_NODE_FOR` | M | Guarded |
| 157 | comptime.c | 1095 | `(Iron_BinaryExpr *)node` | `node->kind == IRON_NODE_BINARY` | M | Guarded |
| 158 | comptime.c | 1101 | `(Iron_UnaryExpr *)node` | `node->kind == IRON_NODE_UNARY` | M | Guarded |
| 159 | comptime.c | 1106 | `(Iron_CallExpr *)node` | `node->kind == IRON_NODE_CALL` | M | Guarded |
| 160 | comptime.c | 1112 | `(Iron_MethodCallExpr *)node` | `node->kind == IRON_NODE_METHOD_CALL` | M | Guarded |
| 161 | comptime.c | 1118 | `(Iron_FieldAccess *)node` | `node->kind == IRON_NODE_FIELD_ACCESS` | M | Guarded |
| 162 | comptime.c | 1123 | `(Iron_IndexExpr *)node` | `node->kind == IRON_NODE_INDEX` | M | Guarded |
| 163 | comptime.c | 1129 | `(Iron_ArrayLit *)node` | `node->kind == IRON_NODE_ARRAY_LIT` | M | Guarded |
| 164 | comptime.c | 1134 | `(Iron_HeapExpr *)node` | `node->kind == IRON_NODE_HEAP` | M | Guarded |
| 165 | comptime.c | 1139 | `(Iron_RcExpr *)node` | `node->kind == IRON_NODE_RC` | M | Guarded |
| 166 | comptime.c | 1144 | `(Iron_ConstructExpr *)node` | `node->kind == IRON_NODE_CONSTRUCT` | M | Guarded |
| 167 | comptime.c | 1149 | `(Iron_DeferStmt *)node` | `node->kind == IRON_NODE_DEFER` | M | Guarded |
| 168 | comptime.c | 1154 | `(Iron_FreeStmt *)node` | `node->kind == IRON_NODE_FREE` | M | Guarded |
| 169 | comptime.c | 1159 | `(Iron_SpawnStmt *)node` | `node->kind == IRON_NODE_SPAWN` | M | Guarded |
| 170 | comptime.c | 1164 | `(Iron_MatchStmt *)node` | `node->kind == IRON_NODE_MATCH` | M | Guarded |
| 171 | comptime.c | 1171 | `(Iron_MatchCase *)node` | `node->kind == IRON_NODE_MATCH_CASE` | M | Guarded |
| 172 | comptime.c | 1176 | `(Iron_AwaitExpr *)node` | `node->kind == IRON_NODE_AWAIT` | M | Guarded |
| 173 | comptime.c | 1181 | `(Iron_LambdaExpr *)node` | `node->kind == IRON_NODE_LAMBDA` | M | Guarded |

### 2. Enum Switch Exhaustiveness (AUDIT-02) -- Comptime

| # | File | Line | Switch Over | Missing Cases | Severity | Suggested Fix |
|---|------|------|-------------|---------------|----------|---------------|
| 20 | comptime.c | 108 | `Iron_ComptimeValKind` (cache_read) | Missing: IRON_CVAL_ARRAY, IRON_CVAL_STRUCT (no serialization) | L | By design; documented at line 207 |
| 21 | comptime.c | 192 | `Iron_ComptimeValKind` (cache_write) | Missing: IRON_CVAL_ARRAY, IRON_CVAL_STRUCT, IRON_CVAL_NULL | L | By design; complex types not cached |
| 22 | comptime.c | 310 | `Iron_NodeKind` (eval_expr) | Handles 13 node kinds; `default` emits error for unsupported kinds. Missing: IRON_NODE_METHOD_CALL, IRON_NODE_FIELD_ACCESS, IRON_NODE_INDEX, IRON_NODE_SLICE, IRON_NODE_INTERP_STRING, IRON_NODE_LAMBDA, IRON_NODE_IS, IRON_NODE_MATCH, IRON_NODE_CAST | M | Emits error on hit -- safe but incomplete interpreter |
| 23 | comptime.c | 397 | `Iron_OpKind` (int arithmetic) | Missing: bitwise ops (SHL, SHR, AMP, PIPE, CARET) | L | Falls through to unsupported-op error |
| 24 | comptime.c | 430 | `Iron_OpKind` (float arithmetic) | Missing: modulo, bitwise | L | Falls through to error |
| 25 | comptime.c | 724 | `Iron_NodeKind` (eval_stmt) | Handles 8 node kinds; `default` calls eval_expr | L | Intentional fallback |
| 26 | comptime.c | 898 | `Iron_ComptimeValKind` (val_to_ast) | IRON_CVAL_STRUCT and IRON_CVAL_NULL return NULL -- cannot round-trip struct/null comptime values | M | Add STRUCT val-to-ast conversion |
| 27 | comptime.c | 1031 | `Iron_NodeKind` (replace_in_node) | Handles 24 node kinds; `default` is leaf node -- exhaustive for all known node types with children | L | Exhaustive |

### 3. Null Safety (AUDIT-03) -- Comptime

| # | File | Line | Access Pattern | Guard | Severity | Suggested Fix |
|---|------|------|----------------|-------|----------|---------------|
| 18 | comptime.c | 28-32 | `cval_alloc` dereferences return of `iron_arena_alloc` without NULL check | **None** | M | arena_alloc may return NULL on OOM |
| 19 | comptime.c | 494 | `call->callee->kind` -- no NULL check on `call->callee` | **None** | M | Add `if (!call->callee)` guard |
| 20 | comptime.c | 557 | `iron_scope_lookup` result used without full NULL check | Checked `!sym` at line 558 | L | Safe |
| 21 | comptime.c | 641-644 | `al->elements[i]` passed to eval_expr | No per-element NULL check | M | eval_expr handles NULL |
| 22 | comptime.c | 678-680 | `ce->args[i]` passed to eval_expr | No per-element NULL check | L | eval_expr handles NULL |
| 23 | comptime.c | 802 | `is->elif_conds[i]` | No NULL check | M | Depends on parser correctness |
| 24 | comptime.c | 808 | `is->elif_bodies[i]` | No NULL check | M | Same |
| 25 | comptime.c | 870 | `iterable->as_array.elems[i]` | Bounded by `count` but not NULL-checked | M | Add NULL guard |

### 4. Arena Lifetimes (AUDIT-04) -- Comptime

| # | File | Line | Pattern | Severity | Description | Suggested Fix |
|---|------|------|---------|----------|-------------|---------------|
| 11 | comptime.c | 264 | `iron_arena_strdup(arena, sb.data, sb.len)` then `iron_strbuf_free(&sb)` | L | Correctly copies data to arena before freeing strbuf |
| 12 | comptime.c | 546 | `iron_arena_alloc(ctx->arena, ...)` for read_file buffer | L | Arena-allocated; owned by eval context |
| 13 | comptime.c | 589 | `arrput(arg_vals, av)` -- stb_ds array of arena-allocated vals | L | arg_vals freed at line 603; values stay in arena |
| 14 | comptime.c | 1242 | `replace_in_node((Iron_Node **)&program, &rctx)` | M | Modifies AST in place; replaced nodes become unreachable in arena but not freed (arena bump allocator). Benign memory waste but could grow on large codebases |

### 5. Integer Safety (AUDIT-05) -- Comptime

| # | File | Line | Pattern | Severity | Suggested Fix |
|---|------|------|---------|----------|---------------|
| 13 | comptime.c | 92 | `snprintf(cache_path, sizeof(cache_path), ...)` | L | Fixed 256 buffer; hash path always fits |
| 14 | comptime.c | 106 | `(Iron_ComptimeValKind)kind_int` -- unchecked int-to-enum cast | M | `kind_int` read from untrusted cache file; could be out-of-range. Add bounds check |
| 15 | comptime.c | 148 | `long str_len = ftell(f) - start` | M | ftell can return -1 on error; subtraction may produce large positive value. Check `ftell != -1` before using |
| 16 | comptime.c | 203 | `(int)val->as_string.len` -- size_t to int truncation in fprintf | L | String length > INT_MAX would truncate printf output -- theoretical |
| 17 | comptime.c | 318 | `strtoll(lit->value, NULL, 10)` | L | No overflow detection; INT64_MAX+1 wraps silently |
| 18 | comptime.c | 398 | `lv->as_int + rv->as_int` | M | int64_t addition overflow is undefined in C (signed). Use overflow-safe arithmetic or wrap in checked_add |
| 19 | comptime.c | 399 | `lv->as_int - rv->as_int` | M | Same |
| 20 | comptime.c | 401 | `lv->as_int * rv->as_int` | M | Same -- multiplication overflow |
| 21 | comptime.c | 474 | `-operand->as_int` | M | Negation of INT64_MIN is UB |
| 22 | comptime.c | 520 | `snprintf(full_path, sizeof(full_path), "%s/%s", ...)` | L | Fixed 4096 buffer; very long paths truncated |
| 23 | comptime.c | 849 | `int64_t i = 0; i < n` in for-range loop | L | Safe: loop count bounded by step_limit |

### 6. Allocation Error Handling (AUDIT-06) -- Comptime

| # | File | Line | Allocation | NULL check | Severity | Suggested Fix |
|---|------|------|------------|------------|----------|---------------|
| 84 | comptime.c | 28 | `iron_arena_alloc(...)` for cval_alloc | **None** | M | No OOM check |
| 85 | comptime.c | 104 | `iron_arena_alloc(...)` for cache read val | **None** | M | Same |
| 86 | comptime.c | 153 | `iron_arena_alloc(...)` for cache string | **None** | M | Same |
| 87 | comptime.c | 546 | `iron_arena_alloc(...)` for read_file buf | **None** | M | Same |
| 88 | comptime.c | 638 | `iron_arena_alloc(...)` for array elems | **None** | M | Same |
| 89 | comptime.c | 656 | `iron_arena_alloc(...)` for struct field_names | **None** | M | Same |
| 90 | comptime.c | 659 | `iron_arena_alloc(...)` for struct field_vals | **None** | M | Same |
| 91 | comptime.c | 901 | `iron_arena_alloc(...)` for IntLit in val_to_ast | **None** | M | Same |
| 92 | comptime.c | 914 | `iron_arena_alloc(...)` for FloatLit | **None** | M | Same |
| 93 | comptime.c | 926 | `iron_arena_alloc(...)` for BoolLit | **None** | M | Same |
| 94 | comptime.c | 936 | `iron_arena_alloc(...)` for StringLit | **None** | M | Same |
| 95 | comptime.c | 947 | `iron_arena_alloc(...)` for ArrayLit | **None** | M | Same |
| 96 | comptime.c | 955 | `iron_arena_alloc(...)` for array elements | **None** | M | Same |
| 97-99 | comptime.c | various | stb_ds arrput/shput calls | **None** | M | Same systemic issue |

### 7. Cross-Platform (AUDIT-08) -- Comptime

| # | File | Line | Pattern | Severity | Suggested Fix |
|---|------|------|---------|----------|---------------|
| 3 | comptime.c | 22 | `#include <sys/stat.h>` | **H** | POSIX-only header; used for `mkdir()` at lines 180-181. Windows requires `<direct.h>` and `_mkdir()` |
| 4 | comptime.c | 180 | `mkdir(".iron-build", 0755)` | **H** | `mkdir()` with mode is POSIX-only. Windows `_mkdir()` takes no mode argument. Add `#ifdef _WIN32` guard with `_mkdir` fallback |
| 5 | comptime.c | 181 | `mkdir(".iron-build/comptime", 0755)` | **H** | Same issue |
| 6 | comptime.c | 95 | `fopen(cache_path, "r")` / line 189 `fopen(..., "w")` | L | POSIX-style paths with `/` separator. On Windows, fopen accepts `/` so this is safe |

---

## Summary -- HIR + Comptime

| Dimension | High | Medium | Low | Total |
|-----------|------|--------|-----|-------|
| Blind Casts | 1 | 171 | 2 | 174 |
| Enum Switch Exhaustiveness | 0 | 7 | 20 | 27 |
| Null Safety | 0 | 13 | 12 | 25 |
| Arena Lifetimes | 0 | 5 | 9 | 14 |
| Integer Safety | 0 | 9 | 14 | 23 |
| Allocation Error Handling | 0 | 99 | 1 | 100 |
| Cross-Platform | 3 | 0 | 3 | 6 |
| **Total** | **4** | **304** | **61** | **369** |

### Key Takeaways

1. **hir_to_lir.c is the highest-risk file** -- site of the SIGSEGV incident. The local struct typedef bug pattern is now fixed but the file contains 27 blind casts in the AST walker alone. The `collect_mono_enums_node` switch is incomplete (8 missing node kinds).

2. **Iron_ExprNode common layout assumption** (hir_lower.c:109) is the only H-severity blind cast in HIR -- a locally-defined struct guessing the prefix layout of all AST nodes.

3. **comptime.c has 3 H-severity cross-platform findings** -- `<sys/stat.h>` and `mkdir()` with POSIX mode bits will fail to compile on Windows MSVC.

4. **Allocation error handling is the largest category** (100 findings, all M) -- systemic across all files. The arena allocator does not return NULL on OOM by design.

5. **Integer safety in comptime** -- signed integer overflow in arithmetic operations (comptime.c:398-401) is undefined behavior in C. The comptime evaluator should use overflow-safe wrappers.

6. **No local struct typedef shadows remain** -- the exact pattern that caused the SIGSEGV is fully eliminated.
