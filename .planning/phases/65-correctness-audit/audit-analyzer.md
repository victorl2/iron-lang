# Correctness Audit: Analyzer

Generated: 2026-04-12
Scope: `src/analyzer/` (analyzer.c, typecheck.c, resolve.c, types.c, scope.c, escape.c, capture.c, concurrency.c, init_check.c, iface_collect.c)

---

## Blind Casts (AUDIT-01)

All AST node types embed `Iron_Span span` + `Iron_NodeKind kind` as their first two fields, so casting an `Iron_Node *` to a concrete node type is structurally safe when the `kind` field matches. The audit checks whether each cast is preceded by a `kind` check (explicit `if`, `case` label, or assertion).

### typecheck.c

| File:Line | Cast Expression | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|---------------|----------|---------------|-------------------|
| typecheck.c:127 | `(Iron_EnumVariant *)ed->variants[i]` | No -- relies on ed->variants only containing EnumVariant nodes | L | Add `assert(ed->variants[i]->kind == IRON_NODE_ENUM_VARIANT)` | blind_cast_enum_variant_tc.iron |
| typecheck.c:169 | `(Iron_Pattern *)pattern_node` | Yes -- `pattern_node->kind != IRON_NODE_PATTERN` check at line 167 | -- | OK | -- |
| typecheck.c:190 | `(Iron_EnumVariant *)ed->variants[vi]` | No -- same structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:294 | `(Iron_MethodDecl *)d` | Yes -- `d->kind != IRON_NODE_METHOD_DECL` at line 293 | -- | OK | -- |
| typecheck.c:372 | `(Iron_EnumConstruct *)init_node` | Yes -- `init_node->kind != IRON_NODE_ENUM_CONSTRUCT` at line 370 | -- | OK | -- |
| typecheck.c:399 | `(Iron_IntLit *)node` | Yes -- `node->kind == IRON_NODE_INT_LIT` at line 398 | -- | OK | -- |
| typecheck.c:409 | `(Iron_UnaryExpr *)node` | Yes -- `node->kind == IRON_NODE_UNARY` at line 407 | -- | OK | -- |
| typecheck.c:412 | `(Iron_IntLit *)ue->operand` | Yes -- `ue->operand->kind == IRON_NODE_INT_LIT` at line 410 | -- | OK | -- |
| typecheck.c:443 | `(Iron_InterfaceDecl *)csym->decl_node` | No -- relies on `csym->sym_kind == IRON_SYM_INTERFACE` implying decl_node is InterfaceDecl | M | Add `assert(csym->decl_node->kind == IRON_NODE_INTERFACE_DECL)` | blind_cast_iface_decl.iron |
| typecheck.c:464 | `(Iron_FuncDecl *)sig` | Yes -- `sig->kind == IRON_NODE_FUNC_DECL` at line 462 | -- | OK | -- |
| typecheck.c:471 | `(Iron_MethodDecl *)d` | Yes -- `d->kind != IRON_NODE_METHOD_DECL` at line 470 | -- | OK | -- |
| typecheck.c:503 | `(Iron_Ident *)generic_params[i]` | No -- relies on generic_params[] only containing Ident nodes | M | Add kind check | blind_cast_generic_param.iron |
| typecheck.c:556 | `(Iron_TypeAnnotation *)ann_node` | Yes -- `ann_node->kind != IRON_NODE_TYPE_ANNOTATION` at line 552 | -- | OK | -- |
| typecheck.c:600 | `(Iron_IntLit *)ann->array_size` | Yes -- `ann->array_size->kind == IRON_NODE_INT_LIT` at line 599 | -- | OK | -- |
| typecheck.c:708 | `(Iron_Ident *)ed->generic_params[i]` | No -- relies on generic_params containing Ident nodes | M | Add kind check | blind_cast_generic_param.iron |
| typecheck.c:728 | `(Iron_EnumVariant *)ed->variants[j]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:750 | `(Iron_EnumVariant *)ed->variants[j]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:786 | `(Iron_IntLit *)ann->array_size` | Yes -- `ann->array_size->kind == IRON_NODE_INT_LIT` at line 785 | -- | OK | -- |
| typecheck.c:804 | `(Iron_BinaryExpr *)expr` | Yes -- `expr->kind != IRON_NODE_BINARY` at line 803 | -- | OK | -- |
| typecheck.c:816 | `(Iron_Ident *)ident_side` | Yes -- `ident_side->kind != IRON_NODE_IDENT` at line 815 | -- | OK | -- |
| typecheck.c:824 | `(Iron_IsExpr *)expr` | Yes -- `expr->kind != IRON_NODE_IS` at line 823 | -- | OK | -- |
| typecheck.c:851 | `(Iron_MethodDecl *)d` | Yes -- `d->kind != IRON_NODE_METHOD_DECL` at line 849 | -- | OK | -- |
| typecheck.c:872 | `(Iron_TypeAnnotation *)ext->return_type` | Yes -- `ext->return_type->kind == IRON_NODE_TYPE_ANNOTATION` at line 871 | -- | OK | -- |
| typecheck.c:1025 | `(Iron_IntLit *)node` | Yes -- `case IRON_NODE_INT_LIT:` | -- | OK | -- |
| typecheck.c:1032 | `(Iron_FloatLit *)node` | Yes -- `case IRON_NODE_FLOAT_LIT:` | -- | OK | -- |
| typecheck.c:1039 | `(Iron_BoolLit *)node` | Yes -- `case IRON_NODE_BOOL_LIT:` | -- | OK | -- |
| typecheck.c:1046 | `(Iron_StringLit *)node` | Yes -- `case IRON_NODE_STRING_LIT:` | -- | OK | -- |
| typecheck.c:1053 | `(Iron_InterpString *)node` | Yes -- `case IRON_NODE_INTERP_STRING:` | -- | OK | -- |
| typecheck.c:1076 | `(Iron_NullLit *)node` | Yes -- `case IRON_NODE_NULL_LIT:` | -- | OK | -- |
| typecheck.c:1083 | `(Iron_Ident *)node` | Yes -- `case IRON_NODE_IDENT:` | -- | OK | -- |
| typecheck.c:1115 | `(Iron_BinaryExpr *)node` | Yes -- `case IRON_NODE_BINARY:` | -- | OK | -- |
| typecheck.c:1226 | `(Iron_UnaryExpr *)node` | Yes -- `case IRON_NODE_UNARY:` | -- | OK | -- |
| typecheck.c:1259 | `(Iron_CallExpr *)node` | Yes -- `case IRON_NODE_CALL:` | -- | OK | -- |
| typecheck.c:1264 | `(Iron_Ident *)ce->callee` | Yes -- `ce->callee->kind == IRON_NODE_IDENT` at line 1263 | -- | OK | -- |
| typecheck.c:1325 | `(Iron_IntLit *)ce->args[0]` | Yes -- `ce->args[0]->kind == IRON_NODE_INT_LIT` at line 1324 | -- | OK | -- |
| typecheck.c:1360 | `(Iron_ObjectDecl *)callee_sym->decl_node` | No -- relies on `callee_sym->sym_kind == IRON_SYM_TYPE` implying ObjectDecl | H | If decl_node is InterfaceDecl or EnumDecl this is UB; add kind check | blind_cast_type_sym_decl.iron |
| typecheck.c:1373 | `(Iron_Field *)od->fields[i]` | No -- relies on od->fields containing Field nodes | L | Add assert | blind_cast_field.iron |
| typecheck.c:1391 | `(Iron_IntLit *)ce->args[i]` | Yes -- guarded by is_int_literal_narrowing which checks IRON_NODE_INT_LIT | -- | OK | -- |
| typecheck.c:1401 | `(Iron_Ident *)od->generic_params[gi]` | No -- relies on generic_params containing Ident nodes | M | Add kind check | blind_cast_generic_param.iron |
| typecheck.c:1406 | `(Iron_TypeAnnotation *)fld->type_ann` | Yes -- `fld->type_ann->kind == IRON_NODE_TYPE_ANNOTATION` at line 1405 | -- | OK | -- |
| typecheck.c:1448 | `(Iron_Ident *)ce->callee` | Yes -- `ce->callee->kind == IRON_NODE_IDENT` at line 1446 | -- | OK | -- |
| typecheck.c:1464 | `(Iron_Ident *)ce->callee` | Yes -- `ce->callee->kind == IRON_NODE_IDENT` at line 1462 | -- | OK | -- |
| typecheck.c:1515 | `(Iron_IntLit *)ce->args[i]` | Yes -- guarded by is_int_literal_narrowing | -- | OK | -- |
| typecheck.c:1522 | `(Iron_Ident *)ce->callee` | Yes -- `ce->callee->kind == IRON_NODE_IDENT` at line 1521 | -- | OK | -- |
| typecheck.c:1525 | `(Iron_FuncDecl *)fn_sym->decl_node` | No -- relies on `fn_sym->sym_kind == IRON_SYM_FUNCTION` implying FuncDecl | M | Add kind check | blind_cast_func_sym_decl.iron |
| typecheck.c:1531 | `(Iron_Ident *)fd->generic_params[gi]` | No -- relies on generic_params containing Ident nodes | M | Add kind check | blind_cast_generic_param.iron |
| typecheck.c:1534 | `(Iron_Param *)fd->params[pi]` | No -- relies on params containing Param nodes | L | Add assert | blind_cast_param.iron |
| typecheck.c:1536 | `(Iron_TypeAnnotation *)fp->type_ann` | Yes -- `fp->type_ann->kind == IRON_NODE_TYPE_ANNOTATION` at line 1535 | -- | OK | -- |
| typecheck.c:1564 | `(Iron_MethodCallExpr *)node` | Yes -- `case IRON_NODE_METHOD_CALL:` | -- | OK | -- |
| typecheck.c:1572 | `(Iron_Ident *)mc->object` | Yes -- `mc->object->kind == IRON_NODE_IDENT` at line 1571 | -- | OK | -- |
| typecheck.c:1638 | `(Iron_FuncDecl *)msig` | Yes -- `msig->kind != IRON_NODE_FUNC_DECL` at line 1637 | -- | OK | -- |
| typecheck.c:1645 | `(Iron_TypeAnnotation *)fd->return_type` | Yes -- `fd->return_type->kind == IRON_NODE_TYPE_ANNOTATION` at line 1643 | -- | OK | -- |
| typecheck.c:1663 | `(Iron_MethodDecl *)d` | Yes -- `d->kind != IRON_NODE_METHOD_DECL` at line 1662 | -- | OK | -- |
| typecheck.c:1680 | `(Iron_MethodDecl *)d` | Yes -- `d->kind != IRON_NODE_METHOD_DECL` at line 1679 | -- | OK | -- |
| typecheck.c:1721 | `(Iron_FieldAccess *)node` | Yes -- `case IRON_NODE_FIELD_ACCESS:` | -- | OK | -- |
| typecheck.c:1751 | `(Iron_Field *)od->fields[i]` | No -- structural assumption | L | Add assert | blind_cast_field.iron |
| typecheck.c:1771 | `(Iron_ConstructExpr *)node` | Yes -- `case IRON_NODE_CONSTRUCT:` | -- | OK | -- |
| typecheck.c:1785 | `(Iron_ObjectDecl *)sym->decl_node` | No -- relies on `sym_kind == IRON_SYM_TYPE` implying ObjectDecl | H | Same as line 1360; if type sym points to InterfaceDecl/EnumDecl this is UB | blind_cast_type_sym_decl.iron |
| typecheck.c:1798 | `(Iron_Field *)od->fields[i]` | No -- structural assumption | L | Add assert | blind_cast_field.iron |
| typecheck.c:1815 | `(Iron_IntLit *)ce->args[i]` | Yes -- guarded by is_int_literal_narrowing | -- | OK | -- |
| typecheck.c:1863 | `(Iron_IsExpr *)node` | Yes -- `case IRON_NODE_IS:` | -- | OK | -- |
| typecheck.c:1871 | `(Iron_IndexExpr *)node` | Yes -- `case IRON_NODE_INDEX:` | -- | OK | -- |
| typecheck.c:1906 | `(Iron_SliceExpr *)node` | Yes -- `case IRON_NODE_SLICE:` | -- | OK | -- |
| typecheck.c:1963 | `(Iron_HeapExpr *)node` | Yes -- `case IRON_NODE_HEAP:` | -- | OK | -- |
| typecheck.c:1970 | `(Iron_RcExpr *)node` | Yes -- `case IRON_NODE_RC:` | -- | OK | -- |
| typecheck.c:1979 | `(Iron_ComptimeExpr *)node` | Yes -- `case IRON_NODE_COMPTIME:` | -- | OK | -- |
| typecheck.c:1986 | `(Iron_LambdaExpr *)node` | Yes -- `case IRON_NODE_LAMBDA:` | -- | OK | -- |
| typecheck.c:1996 | `(Iron_Param *)le->params[p]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:2010 | `(Iron_Param *)le->params[p]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:2029 | `(Iron_Ident *)ae->handle` | Yes -- `ae->handle->kind == IRON_NODE_IDENT` at line 2028 | -- | OK | -- |
| typecheck.c:2047 | `(Iron_TypeAnnotation *)al->type_ann` | Yes -- `al->type_ann->kind == IRON_NODE_TYPE_ANNOTATION` at line 2046 | -- | OK | -- |
| typecheck.c:2145 | `(Iron_EnumConstruct *)node` | Yes -- `case IRON_NODE_ENUM_CONSTRUCT:` | -- | OK | -- |
| typecheck.c:2166 | `(Iron_EnumVariant *)ed->variants[vi]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2189 | `(Iron_Ident *)ed->generic_params[gi]` | No -- relies on generic_params containing Ident | M | Add kind check | blind_cast_generic_param.iron |
| typecheck.c:2210 | `(Iron_Ident *)ed->generic_params[gi]` | No -- nested loop, same as above | M | Add kind check | blind_cast_generic_param.iron |
| typecheck.c:2301 | `(Iron_Ident *)ed->generic_params[gi]` | No -- same pattern | M | Add kind check | blind_cast_generic_param.iron |
| typecheck.c:2315 | `(Iron_EnumVariant *)ed->variants[vj]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2335 | `(Iron_EnumVariant *)ed->variants[vj2]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2364 | `(Iron_EnumVariant *)ed->variants[vi]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2539 | `(Iron_IntLit *)vd->init` | Yes -- guarded by is_int_literal_narrowing which checks IRON_NODE_INT_LIT | -- | OK | -- |
| typecheck.c:2577 | `(Iron_EnumConstruct *)vd->init` | Yes -- `vd->init->kind == IRON_NODE_ENUM_CONSTRUCT` at line 2528 | -- | OK | -- |
| typecheck.c:2587 | `(Iron_IntLit *)vd->init` | Yes -- guarded by is_int_literal_narrowing | -- | OK | -- |
| typecheck.c:2608 | `(Iron_Ident *)as->target` | Yes -- `as->target->kind == IRON_NODE_IDENT` at line 2607 | -- | OK | -- |
| typecheck.c:2641 | `(Iron_IntLit *)as->value` | Yes -- guarded by is_int_literal_narrowing | -- | OK | -- |
| typecheck.c:2650 | `(Iron_IntLit *)as->value` | Yes -- `as->value->kind == IRON_NODE_INT_LIT` at line 2649 | -- | OK | -- |
| typecheck.c:2701 | `(Iron_IntLit *)rs->value` | Yes -- guarded by is_int_literal_narrowing | -- | OK | -- |
| typecheck.c:2747 | `(Iron_Block *)is_s->body` | Yes -- `is_s->body->kind == IRON_NODE_BLOCK` at line 2746 | -- | OK | -- |
| typecheck.c:2771 | `(Iron_IsExpr *)is_s->condition` | No -- condition was classified as is-check by classify_is_check but cast has no kind re-check | M | Add `assert(is_s->condition->kind == IRON_NODE_IS)` | blind_cast_is_expr.iron |
| typecheck.c:2773 | `(Iron_Ident *)ie->expr` | Yes -- `ie->expr->kind == IRON_NODE_IDENT` at line 2772 | -- | OK | -- |
| typecheck.c:2845 | `(Iron_MatchCase *)ms->cases[i]` | No -- relies on cases containing MatchCase nodes | L | Add assert | blind_cast_match_case.iron |
| typecheck.c:2848 | `(Iron_Pattern *)mc->pattern` | Yes -- `mc->pattern->kind == IRON_NODE_PATTERN` at loop top (implicit by loop filter above) | -- | OK | -- |
| typecheck.c:2866 | `(Iron_EnumVariant *)ed->variants[vi]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2889 | `(Iron_EnumVariant *)ed->variants[i]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2910 | `(Iron_EnumVariant *)ed->variants[i]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2932 | `(Iron_MatchCase *)ms->cases[ci]` | No -- relies on cases containing MatchCase | L | Add assert | blind_cast_match_case.iron |
| typecheck.c:2937 | `(Iron_Ident *)mc->pattern` | Yes -- `mc->pattern->kind == IRON_NODE_IDENT` at line 2936 | -- | OK | -- |
| typecheck.c:2945 | `(Iron_EnumConstruct *)mc->pattern` | Yes -- `mc->pattern->kind == IRON_NODE_ENUM_CONSTRUCT` at line 2944 | -- | OK | -- |
| typecheck.c:2950 | `(Iron_Pattern *)mc->pattern` | Yes -- `mc->pattern->kind == IRON_NODE_PATTERN` at line 2949 | -- | OK | -- |
| typecheck.c:2958 | `(Iron_EnumVariant *)ed->variants[vi]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:2982 | `(Iron_EnumVariant *)ed->variants[vi]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:3056 | `(Iron_Block *)ss->body` | No -- assumes spawn body is always Block | M | Add `if (ss->body && ss->body->kind == IRON_NODE_BLOCK)` guard | blind_cast_spawn_body.iron |
| typecheck.c:3060 | `(Iron_ReturnStmt *)blk->stmts[i]` | Yes -- `blk->stmts[i]->kind == IRON_NODE_RETURN` at line 3059 | -- | OK | -- |
| typecheck.c:3064 | `(Iron_IntLit *)rs->value` | No -- casts rs->value to IntLit to access resolved_type field; relies on all expr nodes having resolved_type at same offset | H | Use a generic expr type or proper union access; reading resolved_type through wrong concrete type is aliasing-unsafe | blind_cast_expr_resolved_type.iron |
| typecheck.c:3118 | `(Iron_Param *)fd->params[i]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:3135 | `(Iron_Param *)fd->params[i]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:3141 | `(Iron_Block *)fd->body` | Yes -- `fd->body->kind == IRON_NODE_BLOCK` at line 3140 | -- | OK | -- |
| typecheck.c:3176 | `(Iron_Param *)md->params[i]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:3194 | `(Iron_Param *)md->params[i]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:3200 | `(Iron_Block *)md->body` | Yes -- `md->body->kind == IRON_NODE_BLOCK` at line 3199 | -- | OK | -- |
| typecheck.c:3217 | `(Iron_ObjectDecl *)decl` | Yes -- `decl->kind != IRON_NODE_OBJECT_DECL` at line 3215 | -- | OK | -- |
| typecheck.c:3225 | `(Iron_InterfaceDecl *)iface_sym->decl_node` | No -- relies on `sym_kind == IRON_SYM_INTERFACE` implying InterfaceDecl | M | Add kind check on decl_node | blind_cast_iface_decl.iron |
| typecheck.c:3234 | `(Iron_FuncDecl *)sig_node` | Yes -- `sig_node->kind == IRON_NODE_FUNC_DECL` at line 3233 | -- | OK | -- |
| typecheck.c:3242 | `(Iron_MethodDecl *)d` | Yes -- `d->kind != IRON_NODE_METHOD_DECL` at line 3240 | -- | OK | -- |
| typecheck.c:3292 | `(Iron_ValDecl *)decl` | Yes -- `decl->kind == IRON_NODE_VAL_DECL` at line 3291 | -- | OK | -- |
| typecheck.c:3302 | `(Iron_VarDecl *)decl` | Yes -- `decl->kind == IRON_NODE_VAR_DECL` at line 3301 | -- | OK | -- |
| typecheck.c:3320 | `(Iron_EnumDecl *)decl` | Yes -- `decl->kind != IRON_NODE_ENUM_DECL` at line 3319 | -- | OK | -- |
| typecheck.c:3338 | `(Iron_EnumVariant *)ed->variants[j]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_tc.iron |
| typecheck.c:3371 | `(Iron_FuncDecl *)decl` | Yes -- `decl->kind == IRON_NODE_FUNC_DECL` at line 3370 | -- | OK | -- |
| typecheck.c:3381 | `(Iron_Param *)fd->params[j]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:3390 | `(Iron_MethodDecl *)decl` | Yes -- `decl->kind == IRON_NODE_METHOD_DECL` at line 3389 | -- | OK | -- |
| typecheck.c:3410 | `(Iron_Param *)md->params[j]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| typecheck.c:3425 | `(Iron_FuncDecl *)decl` | Yes -- `decl->kind == IRON_NODE_FUNC_DECL` at line 3424 | -- | OK | -- |
| typecheck.c:3427 | `(Iron_MethodDecl *)decl` | Yes -- `decl->kind == IRON_NODE_METHOD_DECL` at line 3426 | -- | OK | -- |

### resolve.c

| File:Line | Cast Expression | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|---------------|----------|---------------|-------------------|
| resolve.c:75 | `(Iron_ObjectDecl *)node` | Yes -- `case IRON_NODE_OBJECT_DECL:` | -- | OK | -- |
| resolve.c:90 | `(Iron_InterfaceDecl *)node` | Yes -- `case IRON_NODE_INTERFACE_DECL:` | -- | OK | -- |
| resolve.c:104 | `(Iron_EnumDecl *)node` | Yes -- `case IRON_NODE_ENUM_DECL:` | -- | OK | -- |
| resolve.c:117 | `(Iron_EnumVariant *)ed->variants[i]` | No -- relies on variants containing EnumVariant | L | Add assert | blind_cast_enum_variant_resolve.iron |
| resolve.c:129 | `(Iron_FuncDecl *)node` | Yes -- `case IRON_NODE_FUNC_DECL:` | -- | OK | -- |
| resolve.c:144 | `(Iron_ImportDecl *)node` | Yes -- `case IRON_NODE_IMPORT_DECL:` | -- | OK | -- |
| resolve.c:168 | `(Iron_MethodDecl *)node` | Yes -- `node->kind != IRON_NODE_METHOD_DECL` at line 165 | -- | OK | -- |
| resolve.c:208 | `(Iron_Ident *)node` | Yes -- `case IRON_NODE_IDENT:` | -- | OK | -- |
| resolve.c:236 | `(Iron_ObjectDecl *)ctx->current_method->owner_sym->decl_node` | No -- relies on owner_sym being an ObjectDecl sym kind; fails if owner is enum type | H | Add kind check on decl_node before cast | blind_cast_owner_decl.iron |
| resolve.c:251 | `(Iron_ObjectDecl *)ctx->current_method->owner_sym->decl_node` | No -- same pattern as line 236 | H | Add kind check | blind_cast_owner_decl.iron |
| resolve.c:271 | `(Iron_FuncDecl *)node` | Yes -- `case IRON_NODE_FUNC_DECL:` | -- | OK | -- |
| resolve.c:281 | `(Iron_Param *)fd->params[i]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| resolve.c:297 | `(Iron_MethodDecl *)node` | Yes -- `case IRON_NODE_METHOD_DECL:` | -- | OK | -- |
| resolve.c:321 | `(Iron_Param *)md->params[i]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| resolve.c:340 | `(Iron_ValDecl *)node` | Yes -- `case IRON_NODE_VAL_DECL:` | -- | OK | -- |
| resolve.c:360 | `(Iron_VarDecl *)node` | Yes -- `case IRON_NODE_VAR_DECL:` | -- | OK | -- |
| resolve.c:368 | `(Iron_Block *)node` | Yes -- `case IRON_NODE_BLOCK:` | -- | OK | -- |
| resolve.c:380 | `(Iron_AssignStmt *)node` | Yes -- `case IRON_NODE_ASSIGN:` | -- | OK | -- |
| resolve.c:387 | `(Iron_ReturnStmt *)node` | Yes -- `case IRON_NODE_RETURN:` | -- | OK | -- |
| resolve.c:393 | `(Iron_IfStmt *)node` | Yes -- `case IRON_NODE_IF:` | -- | OK | -- |
| resolve.c:406 | `(Iron_WhileStmt *)node` | Yes -- `case IRON_NODE_WHILE:` | -- | OK | -- |
| resolve.c:413 | `(Iron_ForStmt *)node` | Yes -- `case IRON_NODE_FOR:` | -- | OK | -- |
| resolve.c:427 | `(Iron_MatchStmt *)node` | Yes -- `case IRON_NODE_MATCH:` | -- | OK | -- |
| resolve.c:437 | `(Iron_MatchCase *)node` | Yes -- `case IRON_NODE_MATCH_CASE:` | -- | OK | -- |
| resolve.c:446 | `(Iron_DeferStmt *)node` | Yes -- `case IRON_NODE_DEFER:` | -- | OK | -- |
| resolve.c:452 | `(Iron_FreeStmt *)node` | Yes -- `case IRON_NODE_FREE:` | -- | OK | -- |
| resolve.c:458 | `(Iron_LeakStmt *)node` | Yes -- `case IRON_NODE_LEAK:` | -- | OK | -- |
| resolve.c:464 | `(Iron_SpawnStmt *)node` | Yes -- `case IRON_NODE_SPAWN:` | -- | OK | -- |
| resolve.c:481 | `(Iron_InterpString *)node` | Yes -- `case IRON_NODE_INTERP_STRING:` | -- | OK | -- |
| resolve.c:487 | `(Iron_BinaryExpr *)node` | Yes -- `case IRON_NODE_BINARY:` | -- | OK | -- |
| resolve.c:494 | `(Iron_UnaryExpr *)node` | Yes -- `case IRON_NODE_UNARY:` | -- | OK | -- |
| resolve.c:500 | `(Iron_CallExpr *)node` | Yes -- `case IRON_NODE_CALL:` | -- | OK | -- |
| resolve.c:507 | `(Iron_MethodCallExpr *)node` | Yes -- `case IRON_NODE_METHOD_CALL:` | -- | OK | -- |
| resolve.c:514 | `(Iron_FieldAccess *)node` | Yes -- `case IRON_NODE_FIELD_ACCESS:` | -- | OK | -- |
| resolve.c:520 | `(Iron_IndexExpr *)node` | Yes -- `case IRON_NODE_INDEX:` | -- | OK | -- |
| resolve.c:527 | `(Iron_SliceExpr *)node` | Yes -- `case IRON_NODE_SLICE:` | -- | OK | -- |
| resolve.c:535 | `(Iron_LambdaExpr *)node` | Yes -- `case IRON_NODE_LAMBDA:` | -- | OK | -- |
| resolve.c:538 | `(Iron_Param *)le->params[i]` | No -- structural assumption | L | Add assert | blind_cast_param.iron |
| resolve.c:551 | `(Iron_HeapExpr *)node` | Yes -- `case IRON_NODE_HEAP:` | -- | OK | -- |
| resolve.c:557 | `(Iron_RcExpr *)node` | Yes -- `case IRON_NODE_RC:` | -- | OK | -- |
| resolve.c:563 | `(Iron_ComptimeExpr *)node` | Yes -- `case IRON_NODE_COMPTIME:` | -- | OK | -- |
| resolve.c:569 | `(Iron_IsExpr *)node` | Yes -- `case IRON_NODE_IS:` | -- | OK | -- |
| resolve.c:575 | `(Iron_AwaitExpr *)node` | Yes -- `case IRON_NODE_AWAIT:` | -- | OK | -- |
| resolve.c:581 | `(Iron_ConstructExpr *)node` | Yes -- `case IRON_NODE_CONSTRUCT:` | -- | OK | -- |
| resolve.c:592 | `(Iron_ArrayLit *)node` | Yes -- `case IRON_NODE_ARRAY_LIT:` | -- | OK | -- |
| resolve.c:599 | `(Iron_Pattern *)node` | Yes -- `case IRON_NODE_PATTERN:` | -- | OK | -- |
| resolve.c:612 | `(Iron_EnumDecl *)esym->decl_node` | No -- relies on `esym->sym_kind == IRON_SYM_ENUM` implying EnumDecl | M | Add kind check on decl_node | blind_cast_enum_decl.iron |
| resolve.c:615 | `(Iron_EnumVariant *)ed->variants[i]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_resolve.iron |
| resolve.c:659 | `(Iron_EnumConstruct *)node` | Yes -- `case IRON_NODE_ENUM_CONSTRUCT:` | -- | OK | -- |
| resolve.c:665 | `(Iron_Ident *)iron_arena_alloc(...)` | N/A -- constructing new node, not casting existing | -- | OK | -- |
| resolve.c:674 | `(Iron_MethodCallExpr *)ec` | No -- reinterprets EnumConstruct as MethodCallExpr in-place | H | Dangerous reinterpretation relies on identical memory layout; add static_assert or use fresh allocation | enum_construct_reinterpret.iron |
| resolve.c:680 | `(Iron_FieldAccess *)ec` | No -- reinterprets EnumConstruct as FieldAccess in-place | H | Same as line 674 | enum_construct_reinterpret.iron |
| resolve.c:689 | `(Iron_EnumDecl *)esym->decl_node` | No -- relies on sym_kind implying EnumDecl | M | Add kind check | blind_cast_enum_decl.iron |
| resolve.c:692 | `(Iron_EnumVariant *)ed->variants[i]` | No -- structural assumption | L | Add assert | blind_cast_enum_variant_resolve.iron |

### types.c

All casts are type-system internal. No AST node casts present. No unguarded casts.

### scope.c

No AST node casts. stb_ds API only. No unguarded casts.

### analyzer.c

No casts (orchestrator only). Clean.

---

## Enum Switch Exhaustiveness (AUDIT-02)

### typecheck.c

| File:Line | Switch Subject | Missing Cases | Default Handling | Severity | Suggested Fix | Regression Fixture |
|-----------|---------------|---------------|-----------------|----------|---------------|-------------------|
| typecheck.c:90 (type_mangle_component) | `Iron_TypeKind` | Missing: IRON_TYPE_NULLABLE, IRON_TYPE_FUNC, IRON_TYPE_ARRAY, IRON_TYPE_RC, IRON_TYPE_GENERIC_PARAM, IRON_TYPE_NULL, IRON_TYPE_ERROR, IRON_TYPE_OBJECT, IRON_TYPE_INTERFACE, IRON_TYPE_TUPLE | `default:` falls through to iron_type_to_string | L | Intentional fallback; document in comment | -- |
| typecheck.c:230 (type_bit_width) | `Iron_TypeKind` | Missing: all non-numeric/bool types | `default: return 0` | -- | OK -- intentional | -- |
| typecheck.c:245 (value_fits_type) | `Iron_TypeKind` | Missing: all non-integer types | `default: return true` | L | Permissive default on non-integer is intentional but fragile | switch_value_fits.iron |
| typecheck.c:262 (is_narrow_integer) | `Iron_TypeKind` | Missing: non-narrow types | `default: return false` | -- | OK | -- |
| typecheck.c:1023 (check_expr) | `Iron_NodeKind` | Missing: IRON_NODE_PROGRAM, IRON_NODE_ERROR, IRON_NODE_OBJECT_DECL, IRON_NODE_INTERFACE_DECL, IRON_NODE_ENUM_DECL, IRON_NODE_FUNC_DECL, IRON_NODE_METHOD_DECL, IRON_NODE_PARAM, IRON_NODE_FIELD, IRON_NODE_TYPE_ANNOTATION, IRON_NODE_ENUM_VARIANT, IRON_NODE_BLOCK, IRON_NODE_VAL_DECL, IRON_NODE_VAR_DECL, IRON_NODE_ASSIGN, IRON_NODE_RETURN, IRON_NODE_IF, IRON_NODE_WHILE, IRON_NODE_FOR, IRON_NODE_MATCH, IRON_NODE_MATCH_CASE, IRON_NODE_DEFER, IRON_NODE_FREE, IRON_NODE_LEAK, IRON_NODE_SPAWN, IRON_NODE_IMPORT_DECL, IRON_NODE_PATTERN | `default: return ERROR` | L | Statements fall through to ERROR which is acceptable; the switch is expression-only | -- |
| typecheck.c:1273 (is_numeric_or_bool) | `Iron_TypeKind` | Missing: non-numeric/bool types | `default: break` | -- | OK | -- |
| typecheck.c:2437 (check_stmt) | `Iron_NodeKind` | Missing: all expr-only kinds and decl kinds (IRON_NODE_PROGRAM, IRON_NODE_ERROR, IRON_NODE_OBJECT_DECL, IRON_NODE_INTERFACE_DECL, IRON_NODE_ENUM_DECL, literals, etc.) | `default: check_expr(ctx, node)` | L | Default is intentional (expr-as-stmt); could add explicit cases for documentation | -- |

### resolve.c

| File:Line | Switch Subject | Missing Cases | Default Handling | Severity | Suggested Fix | Regression Fixture |
|-----------|---------------|---------------|-----------------|----------|---------------|-------------------|
| resolve.c:72 (collect_decl) | `Iron_NodeKind` | Missing: all non-decl kinds (IRON_NODE_IDENT, IRON_NODE_BLOCK, etc.) | `default: break` | -- | OK -- only processes decls | -- |
| resolve.c:204 (resolve_node) | `Iron_NodeKind` | Missing: IRON_NODE_IMPORT_DECL (listed but break only), IRON_NODE_PROGRAM (break only), IRON_NODE_ERROR (break only) | `default: break` | L | Silently ignores unknown node kinds; add diagnostic in default for safety | switch_resolve_unknown_node.iron |

### types.c

| File:Line | Switch Subject | Missing Cases | Default Handling | Severity | Suggested Fix | Regression Fixture |
|-----------|---------------|---------------|-----------------|----------|---------------|-------------------|
| types.c:20 (is_primitive_kind) | `Iron_TypeKind` | Missing: IRON_TYPE_OBJECT, IRON_TYPE_INTERFACE, IRON_TYPE_ENUM, IRON_TYPE_NULLABLE, IRON_TYPE_FUNC, IRON_TYPE_ARRAY, IRON_TYPE_RC, IRON_TYPE_GENERIC_PARAM, IRON_TYPE_TUPLE | `default: return false` | -- | OK -- intentionally excludes non-primitives | -- |
| types.c:209 (iron_type_equals) | `Iron_TypeKind` | Covers all 27 kinds; no missing cases | No default | -- | OK -- exhaustive | -- |
| types.c:283 (iron_type_to_string) | `Iron_TypeKind` | Covers all 27 kinds; no missing cases | Falls through to `return "<unknown>"` | -- | OK | -- |
| types.c:423 (iron_type_is_integer) | `Iron_TypeKind` | Missing: non-integer types | `default: return false` | -- | OK | -- |
| types.c:436 (iron_type_is_float) | `Iron_TypeKind` | Missing: non-float types | `default: return false` | -- | OK | -- |

### scope.c / analyzer.c

No switch statements over Iron enums.

---

## Null Safety (AUDIT-03)

| File:Line | Expression | Risk | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|------|----------|---------------|-------------------|
| typecheck.c:149 | `iron_symbol_create` return used without NULL check | sym could be NULL if arena OOM | M | Check `if (!sym) return` | null_safety_tc_define.iron |
| typecheck.c:152 | `iron_scope_define` called with potentially NULL sym | Same chain as line 149 | M | Guard against NULL sym | null_safety_tc_define.iron |
| typecheck.c:523 | `shgeti(ctx->narrowed, name)` returns -1 if missing; idx used as int not ptrdiff_t | Truncation risk on 64-bit | L | Use `ptrdiff_t idx` consistently | null_safety_narrowing.iron |
| typecheck.c:1360 | `(Iron_ObjectDecl *)callee_sym->decl_node` -- decl_node can be NULL for builtin symbols (e.g., primitive type syms created in resolve.c have `decl_node=NULL`) | Null deref of od if decl_node is NULL for a type sym that isn't an object | H | Add `if (!callee_sym->decl_node) break;` before cast | null_deref_builtin_type.iron |
| typecheck.c:1581 | `obj_id->resolved_type->object.decl->name` -- triple deref without NULL checks | If decl is NULL, null deref | M | Add `decl` null check | null_deref_obj_method.iron |
| typecheck.c:2075 | `arrput(elem_types, et)` with et possibly NULL | NULL element pushed into stb_ds array | L | Already checked `if (et)` at line 2075 | -- |
| typecheck.c:2080 | `arrlen(elem_types)` -- elem_types may be NULL if no elements pushed | stb_ds arrlen(NULL) returns 0; safe | -- | OK | -- |
| typecheck.c:2528-2530 | Ternary chain accessing vd->init->kind without NULL check on vd->init | `vd->init` is checked at line 2527 (decl_type && init_type path); however the ternary re-reads vd->init which could theoretically be NULL in a pathological case | L | Add explicit `vd->init != NULL` guard | null_deref_val_init.iron |
| typecheck.c:3059 | `blk->stmts[i]->kind` without NULL check on stmts[i] | If stmts[i] is NULL, null deref | M | Add `if (!blk->stmts[i]) continue;` | null_deref_spawn_stmts.iron |
| typecheck.c:3113 | `param_types` alloc result unchecked before use at line 3119 | arena OOM leads to NULL deref | M | Add NULL check after alloc | null_alloc_param_types.iron |
| typecheck.c:3170 | `param_types` alloc result unchecked before use at line 3177 | Same pattern | M | Add NULL check | null_alloc_param_types.iron |
| resolve.c:42 | `ctx->current_scope->parent` deref without checking if current_scope is NULL | If iron_scope_create failed, current_scope is NULL | M | Add NULL check | null_safety_pop_scope.iron |
| resolve.c:236 | `ctx->current_method->owner_sym->decl_node` triple chain | owner_sym checked at line 231; decl_node check at line 232; safe but fragile | L | Already guarded | -- |
| scope.c:25 | `shgeti(s->symbols, name)` -- symbols might not be initialized | sh_new_strdup at line 17 ensures initialized; safe | -- | OK | -- |
| types.c:89-92 | `iron_arena_alloc` return in iron_type_make_func not checked after param copy | First ARENA_ALLOC is checked; second `iron_arena_alloc` at line 89 is checked (returns NULL) | -- | OK | -- |
| types.c:192-193 | `iron_arena_alloc` for tuple copy checked (returns NULL at line 194) | -- | OK | -- |

---

## Arena Lifetimes (AUDIT-04)

| File:Line | Pattern | Risk | Severity | Suggested Fix | Regression Fixture |
|-----------|--------|------|----------|---------------|-------------------|
| typecheck.c:529 | `shput(ctx->narrowed, name, ty)` -- stb_ds strdup keys stored alongside arena-allocated type pointers | stb_ds heap-allocates key copies (sh_new_strdup); type pointers come from the compilation arena. Keys survive arena reset but point to stale strings. However, narrowed map is freed at end of typecheck (line 3434), and arena outlives the entire typecheck call -- safe as long as no arena reset occurs mid-typecheck | L | Document lifetime coupling | -- |
| typecheck.c:693 | `shput(ctx->mono_registry, arena_strdup(mangled), mono)` -- stb_ds copies key via strdup but value is arena pointer | Same pattern: stb_ds heap-allocated map with arena-allocated values. Map is freed at function end. If the arena were reset before mono_registry is read, values would dangle | M | mono_registry is never freed (line 3436 frees narrowed and spawn_result_types but NOT mono_registry) -- this is a memory leak of the stb_ds hash map | arena_mono_registry_leak.iron |
| typecheck.c:2075 | `arrput(elem_types, et)` -- stb_ds dynamic array holding arena-allocated Iron_Type pointers | Array is freed at line 2123 (arrfree). Pointers remain valid as long as arena lives. Safe. | -- | OK | -- |
| scope.c:17 | `sh_new_strdup(s->symbols)` -- stb_ds hash map allocated on heap, scope allocated on arena | If arena is reset, the scope pointer becomes invalid but the stb_ds map leaks since no shfree() is called on scope destruction | M | Add shfree in a scope destructor or accept as known design tradeoff (arenas are bulk-freed at process end) | arena_scope_leak.iron |
| resolve.c:749-874 | Built-in symbols allocated via arena but stored in stb_ds hash maps | Same arena + stb_ds coupling pattern | M | Design tradeoff -- document | -- |

---

## Integer Safety (AUDIT-05)

| File:Line | Expression | Risk | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|------|----------|---------------|-------------------|
| typecheck.c:536 | `int n = (int)shlenu(ctx->narrowed)` -- shlenu returns size_t, truncated to int | On a program with >2^31 narrowing entries (practically impossible) this would overflow | L | Use `ptrdiff_t n` | int_safety_narrowing_count.iron |
| typecheck.c:2080 | `arrlen(elem_types) > 1` -- arrlen returns ptrdiff_t, compared with int literal | Comparison is safe (arrlen never exceeds INT_MAX for array literal element count) | -- | OK | -- |
| typecheck.c:2082 | `for (int i = 1; i < arrlen(elem_types); i++)` -- signed/unsigned comparison | arrlen returns ptrdiff_t which is signed; loop index int is narrower. Practically safe but technically lossy if arrlen > INT_MAX | L | Use `ptrdiff_t i` | -- |
| typecheck.c:2927 | `bool covered[256]` with `if (vc > 256) vc = 256` -- silently truncates variant coverage check for enums with >256 variants | Variants beyond 256 are never checked for exhaustiveness | M | Use dynamic allocation or arena alloc sized to vc | int_safety_large_enum.iron |
| types.c:50 | `for (int k = IRON_TYPE_INT; k <= IRON_TYPE_ERROR; k++)` -- iterates enum range | If new type kinds are added beyond IRON_TYPE_ERROR, loop misses them | L | Add static_assert on IRON_TYPE_ERROR being last | int_safety_type_enum.iron |
| types.c:169 | `char buf[1024]` fixed-size buffer for tuple mangled name | A deeply nested tuple type could exceed 1024 chars, silently truncating the name | M | Check pos against sizeof(buf) and fail gracefully | int_safety_tuple_name.iron |
| types.c:339 | `char buf[512]` for func type string | Deeply nested function types could exceed 512 chars | L | Same as above; less likely in practice | -- |
| types.c:375 | `char buf[512]` for generic enum string | Same truncation risk | L | Same | -- |
| types.c:397 | `char buf[1024]` for tuple string | Same pattern; consistent with mangled name buffer | L | Same | -- |
| resolve.c:604-607 | `char msg[256]` with snprintf | snprintf truncates; messages may be cut off for long names but no UB | L | Consider dynamic allocation for long type names | -- |

---

## Allocation Error Handling (AUDIT-06)

| File:Line | Allocation | Error Handled | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|---------------|----------|---------------|-------------------|
| typecheck.c:149 | `iron_symbol_create(ctx->arena, ...)` in tc_define | No -- return value used without NULL check | M | Add NULL check | alloc_error_tc_define.iron |
| typecheck.c:534 | `sh_new_strdup(copy)` narrowing deep-copy | No -- stb_ds may fail silently | M | Check copy != NULL | alloc_error_narrowing.iron |
| typecheck.c:563-565 | `iron_arena_alloc` for tuple elem_types in resolve_type_annotation | No -- result used without NULL check | M | Add NULL check | alloc_error_tuple_resolve.iron |
| typecheck.c:583 | `iron_arena_alloc` for func param_types in resolve_type_annotation | No -- result used without NULL check | M | Add NULL check | alloc_error_func_resolve.iron |
| typecheck.c:651-653 | `iron_arena_alloc` for type_args in generic enum resolution | No -- result used without NULL check | M | Add NULL check | alloc_error_generic_enum.iron |
| typecheck.c:682-684 | `iron_arena_alloc` for mono type | No -- result used without NULL check (memset would crash) | M | Add NULL check | alloc_error_mono_type.iron |
| typecheck.c:723-726 | `iron_arena_alloc` for variant_payload_types | No -- result used without NULL check | M | Add NULL check | alloc_error_vpt.iron |
| typecheck.c:730-732 | `iron_arena_alloc` for payload row | No -- result used without NULL check | M | Add NULL check | alloc_error_payload_row.iron |
| typecheck.c:746-748 | `iron_arena_alloc` for payload_is_boxed | No -- result used without NULL check | M | Add NULL check | alloc_error_pib.iron |
| typecheck.c:1991-1994 | `iron_arena_alloc` for lambda param_types | No -- result used without NULL check | M | Add NULL check | alloc_error_lambda_params.iron |
| typecheck.c:2050-2052 | `iron_arena_alloc` for tuple elements in check_expr | No -- result used without NULL check | M | Add NULL check | alloc_error_tuple_check.iron |
| typecheck.c:2178-2180 | `iron_arena_alloc` for inferred_args | No -- result used without NULL check | M | Add NULL check | alloc_error_inferred_args.iron |
| typecheck.c:2276-2278 | `iron_arena_alloc` for mono type (path 2) | No -- result used without NULL check | M | Add NULL check | alloc_error_mono_type2.iron |
| typecheck.c:2293-2296 | `iron_arena_alloc` for vpt (path 2) | No -- result used without NULL check | M | Add NULL check | alloc_error_vpt2.iron |
| typecheck.c:2317-2319 | `iron_arena_alloc` for payload row (path 2) | No -- result used without NULL check | M | Add NULL check | alloc_error_payload2.iron |
| typecheck.c:2331-2332 | `iron_arena_alloc` for pib2 | No -- result used without NULL check | M | Add NULL check | alloc_error_pib2.iron |
| typecheck.c:2337-2339 | `iron_arena_alloc` for pib2_row | No -- result used without NULL check | M | Add NULL check | alloc_error_pib2_row.iron |
| typecheck.c:2840-2842 | `iron_arena_alloc` for covered array | No -- result used without NULL check | M | Add NULL check | alloc_error_covered.iron |
| typecheck.c:3113-3115 | `iron_arena_alloc` for func param_types in check_func_decl | No -- result used without NULL check | M | Add NULL check | alloc_error_func_params.iron |
| typecheck.c:3170-3172 | `iron_arena_alloc` for method param_types | No -- result used without NULL check | M | Add NULL check | alloc_error_method_params.iron |
| typecheck.c:3328-3330 | `iron_arena_alloc` for vpt in pre-pass | No -- result used without NULL check | M | Add NULL check | alloc_error_prepass_vpt.iron |
| typecheck.c:3343-3344 | `iron_arena_alloc` for payload row in pre-pass | No -- result used without NULL check | M | Add NULL check | alloc_error_prepass_row.iron |
| typecheck.c:3346-3348 | `iron_arena_alloc` for payload_is_boxed in pre-pass | No -- result used without NULL check | M | Add NULL check | alloc_error_prepass_pib.iron |
| typecheck.c:3349-3351 | `iron_arena_alloc` for pib_row in pre-pass | No -- result used without NULL check | M | Add NULL check | alloc_error_prepass_pib_row.iron |
| typecheck.c:3377-3379 | `iron_arena_alloc` for func param_types in pre-pass | No -- result used without NULL check | M | Add NULL check | alloc_error_prepass_func.iron |
| typecheck.c:3406-3408 | `iron_arena_alloc` for method param_types in pre-pass | No -- result used without NULL check | M | Add NULL check | alloc_error_prepass_method.iron |
| resolve.c:50-51 | `iron_symbol_create` in define_sym | No -- return value used without NULL check | M | Add NULL check | alloc_error_define_sym.iron |
| resolve.c:77 | `iron_symbol_create` in collect_decl | No -- return value used without NULL check | M | Add NULL check | alloc_error_collect_decl.iron |
| resolve.c:665-666 | `iron_arena_alloc` for synthetic Ident in ENUM_CONSTRUCT reclassification | No -- result used without NULL check | M | Add NULL check | alloc_error_synth_ident.iron |
| types.c:64 | `ARENA_ALLOC` in iron_type_make_nullable | Yes -- `if (!t) return NULL` at line 65 | -- | OK | -- |
| types.c:73 | `ARENA_ALLOC` in iron_type_make_rc | Yes -- `if (!t) return NULL` at line 74 | -- | OK | -- |
| types.c:82 | `ARENA_ALLOC` in iron_type_make_func | Yes -- `if (!t) return NULL` at line 83 | -- | OK | -- |
| types.c:89-91 | `iron_arena_alloc` for param copy | Yes -- `if (!copy) return NULL` at line 91 | -- | OK | -- |
| types.c:101 | `ARENA_ALLOC` in iron_type_make_array | Yes -- `if (!t) return NULL` at line 102 | -- | OK | -- |
| types.c:111 | `ARENA_ALLOC` in iron_type_make_object | Yes -- `if (!t) return NULL` at line 112 | -- | OK | -- |
| types.c:120 | `ARENA_ALLOC` in iron_type_make_interface | Yes -- `if (!t) return NULL` at line 121 | -- | OK | -- |
| types.c:129 | `ARENA_ALLOC` in iron_type_make_enum | Yes -- `if (!t) return NULL` at line 130 | -- | OK | -- |
| types.c:138 | `ARENA_ALLOC` in iron_type_make_generic_param | Yes -- `if (!t) return NULL` at line 139 | -- | OK | -- |
| types.c:186 | `ARENA_ALLOC` in iron_type_make_tuple | Yes -- `if (!t) return NULL` at line 187 | -- | OK | -- |
| types.c:192 | `iron_arena_alloc` for tuple elem copy | Yes -- `if (!copy) return NULL` at line 194 | -- | OK | -- |
| scope.c:9 | `ARENA_ALLOC` in iron_scope_create | Yes -- `if (!s) return NULL` at line 10 | -- | OK | -- |
| scope.c:58 | `ARENA_ALLOC` in iron_symbol_create | Yes -- `if (!sym) return NULL` at line 59 | -- | OK | -- |

---

## Cross-Platform (AUDIT-08)

| File:Line | Pattern | Risk | Severity | Suggested Fix | Regression Fixture |
|-----------|--------|------|----------|---------------|-------------------|
| (none) | No `__attribute__`, `__builtin_*`, POSIX-only headers, `_GNU_SOURCE`, or platform-specific `#ifdef` in any of the 5 core analyzer files | -- | -- | -- | -- |

All 5 core analyzer files (analyzer.c, typecheck.c, resolve.c, types.c, scope.c) are fully portable C99/C11 with no platform-specific code.
