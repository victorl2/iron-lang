# Correctness Audit: Parser + Lexer

Generated: 2026-04-12
Scope: `src/parser/` (ast.h, ast.c, parser.c, parser.h, printer.c, printer.h) and `src/lexer/` (lexer.c, lexer.h)

---

## Blind Casts (AUDIT-01)

All AST node types embed `Iron_Span span` + `Iron_NodeKind kind` as their first two fields, so casting an `Iron_Node *` to a concrete node type is structurally safe when the `kind` field matches. The audit checks whether each cast is preceded by a `kind` check (explicit `if`, `case` label, or assertion).

### Parser: ast.c

| File:Line | Cast Expression | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|---------------|----------|---------------|-------------------|
| ast.c:89 | `(Iron_Program *)root` | Yes -- `case IRON_NODE_PROGRAM:` | -- | OK | -- |
| ast.c:97 | `(Iron_ObjectDecl *)root` | Yes -- `case IRON_NODE_OBJECT_DECL:` | -- | OK | -- |
| ast.c:102 | `(Iron_InterfaceDecl *)root` | Yes -- `case IRON_NODE_INTERFACE_DECL:` | -- | OK | -- |
| ast.c:108 | `(Iron_EnumDecl *)root` | Yes -- `case IRON_NODE_ENUM_DECL:` | -- | OK | -- |
| ast.c:112 | `(Iron_FuncDecl *)root` | Yes -- `case IRON_NODE_FUNC_DECL:` | -- | OK | -- |
| ast.c:120 | `(Iron_MethodDecl *)root` | Yes -- `case IRON_NODE_METHOD_DECL:` | -- | OK | -- |
| ast.c:128 | `(Iron_Param *)root` | Yes -- `case IRON_NODE_PARAM:` | -- | OK | -- |
| ast.c:133 | `(Iron_Field *)root` | Yes -- `case IRON_NODE_FIELD:` | -- | OK | -- |
| ast.c:138 | `(Iron_EnumVariant *)root` | Yes -- `case IRON_NODE_ENUM_VARIANT:` | -- | OK | -- |
| ast.c:143 | `(Iron_TypeAnnotation *)root` | Yes -- `case IRON_NODE_TYPE_ANNOTATION:` | -- | OK | -- |
| ast.c:149 | `(Iron_ValDecl *)root` | Yes -- `case IRON_NODE_VAL_DECL:` | -- | OK | -- |
| ast.c:155 | `(Iron_VarDecl *)root` | Yes -- `case IRON_NODE_VAR_DECL:` | -- | OK | -- |
| ast.c:161 | `(Iron_AssignStmt *)root` | Yes -- `case IRON_NODE_ASSIGN:` | -- | OK | -- |
| ast.c:167 | `(Iron_ReturnStmt *)root` | Yes -- `case IRON_NODE_RETURN:` | -- | OK | -- |
| ast.c:172 | `(Iron_IfStmt *)root` | Yes -- `case IRON_NODE_IF:` | -- | OK | -- |
| ast.c:181 | `(Iron_WhileStmt *)root` | Yes -- `case IRON_NODE_WHILE:` | -- | OK | -- |
| ast.c:187 | `(Iron_ForStmt *)root` | Yes -- `case IRON_NODE_FOR:` | -- | OK | -- |
| ast.c:194 | `(Iron_MatchStmt *)root` | Yes -- `case IRON_NODE_MATCH:` | -- | OK | -- |
| ast.c:201 | `(Iron_MatchCase *)root` | Yes -- `case IRON_NODE_MATCH_CASE:` | -- | OK | -- |
| ast.c:207 | `(Iron_DeferStmt *)root` | Yes -- `case IRON_NODE_DEFER:` | -- | OK | -- |
| ast.c:212 | `(Iron_FreeStmt *)root` | Yes -- `case IRON_NODE_FREE:` | -- | OK | -- |
| ast.c:217 | `(Iron_LeakStmt *)root` | Yes -- `case IRON_NODE_LEAK:` | -- | OK | -- |
| ast.c:222 | `(Iron_SpawnStmt *)root` | Yes -- `case IRON_NODE_SPAWN:` | -- | OK | -- |
| ast.c:228 | `(Iron_Block *)root` | Yes -- `case IRON_NODE_BLOCK:` | -- | OK | -- |
| ast.c:243 | `(Iron_InterpString *)root` | Yes -- `case IRON_NODE_INTERP_STRING:` | -- | OK | -- |
| ast.c:248 | `(Iron_BinaryExpr *)root` | Yes -- `case IRON_NODE_BINARY:` | -- | OK | -- |
| ast.c:253 | `(Iron_UnaryExpr *)root` | Yes -- `case IRON_NODE_UNARY:` | -- | OK | -- |
| ast.c:258 | `(Iron_CallExpr *)root` | Yes -- `case IRON_NODE_CALL:` | -- | OK | -- |
| ast.c:264 | `(Iron_MethodCallExpr *)root` | Yes -- `case IRON_NODE_METHOD_CALL:` | -- | OK | -- |
| ast.c:270 | `(Iron_FieldAccess *)root` | Yes -- `case IRON_NODE_FIELD_ACCESS:` | -- | OK | -- |
| ast.c:275 | `(Iron_IndexExpr *)root` | Yes -- `case IRON_NODE_INDEX:` | -- | OK | -- |
| ast.c:281 | `(Iron_SliceExpr *)root` | Yes -- `case IRON_NODE_SLICE:` | -- | OK | -- |
| ast.c:288 | `(Iron_LambdaExpr *)root` | Yes -- `case IRON_NODE_LAMBDA:` | -- | OK | -- |
| ast.c:295 | `(Iron_HeapExpr *)root` | Yes -- `case IRON_NODE_HEAP:` | -- | OK | -- |
| ast.c:300 | `(Iron_RcExpr *)root` | Yes -- `case IRON_NODE_RC:` | -- | OK | -- |
| ast.c:305 | `(Iron_ComptimeExpr *)root` | Yes -- `case IRON_NODE_COMPTIME:` | -- | OK | -- |
| ast.c:310 | `(Iron_IsExpr *)root` | Yes -- `case IRON_NODE_IS:` | -- | OK | -- |
| ast.c:315 | `(Iron_AwaitExpr *)root` | Yes -- `case IRON_NODE_AWAIT:` | -- | OK | -- |
| ast.c:320 | `(Iron_ConstructExpr *)root` | Yes -- `case IRON_NODE_CONSTRUCT:` | -- | OK | -- |
| ast.c:326 | `(Iron_ArrayLit *)root` | Yes -- `case IRON_NODE_ARRAY_LIT:` | -- | OK | -- |
| ast.c:333 | `(Iron_Pattern *)root` | Yes -- `case IRON_NODE_PATTERN:` | -- | OK | -- |
| ast.c:338 | `(Iron_EnumConstruct *)root` | Yes -- `case IRON_NODE_ENUM_CONSTRUCT:` | -- | OK | -- |

All casts in ast.c are guarded by switch-case labels. No unguarded casts.

### Parser: parser.c

| File:Line | Cast Expression | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|---------------|----------|---------------|-------------------|
| parser.c:267 | `(Iron_TypeAnnotation *)elem` | Yes -- `elem->kind == IRON_NODE_TYPE_ANNOTATION` check at line 267 | -- | OK | -- |
| parser.c:1029 | `(Iron_Ident *)left` | Yes -- `left->kind == IRON_NODE_IDENT` check at line 1028 | -- | OK | -- |
| parser.c:1078 | `(Iron_Ident *)left` | Yes -- `left->kind == IRON_NODE_IDENT` check at line 1077 | -- | OK | -- |
| parser.c:1783 | `(Iron_SpawnStmt *)spawn_node` | Yes -- `spawn_node->kind == IRON_NODE_SPAWN` check at line 1783 | -- | OK | -- |
| parser.c:1833 | `(Iron_SpawnStmt *)spawn_node` | Yes -- `spawn_node->kind == IRON_NODE_SPAWN` check at line 1833 | -- | OK | -- |
| parser.c:2583 | `(Iron_EnumVariant *)variants[i]` | No -- relies on construction context (variants array only contains EnumVariant nodes) | L | Add `assert(variants[i]->kind == IRON_NODE_ENUM_VARIANT)` | blind_cast_enum_variant.iron |
| parser.c:2624 | `(Iron_FuncDecl *)n` | Yes -- `n->kind == IRON_NODE_FUNC_DECL` check at line 2624 | -- | OK | -- |
| parser.c:2626 | `(Iron_MethodDecl *)n` | Yes -- `n->kind == IRON_NODE_METHOD_DECL` check at line 2626 | -- | OK | -- |

### Parser: printer.c

| File:Line | Cast Expression | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|---------------|----------|---------------|-------------------|
| printer.c:58 | `(Iron_TypeAnnotation *)node` | Yes -- `node->kind == IRON_NODE_TYPE_ANNOTATION` check at line 57 | -- | OK | -- |
| printer.c:89 | `(Iron_Param *)node` | Yes -- `node->kind != IRON_NODE_PARAM` guard at line 88 | -- | OK | -- |
| printer.c:114 | `(Iron_Ident *)gps[i]` | Yes -- `gps[i]->kind == IRON_NODE_IDENT` check at line 114 | -- | OK | -- |
| printer.c:130 | `(Iron_Block *)node` | Yes -- `node->kind != IRON_NODE_BLOCK` guard at line 126 | -- | OK | -- |
| printer.c:162 | `(Iron_Program *)node` | Yes -- `case IRON_NODE_PROGRAM:` | -- | OK | -- |
| printer.c:172 | `(Iron_ImportDecl *)node` | Yes -- `case IRON_NODE_IMPORT_DECL:` | -- | OK | -- |
| printer.c:180 | `(Iron_ObjectDecl *)node` | Yes -- `case IRON_NODE_OBJECT_DECL:` | -- | OK | -- |
| printer.c:197 | `(Iron_Field *)n->fields[i]` | No -- relies on construction context (fields array only contains Field nodes) | L | Add `assert(n->fields[i]->kind == IRON_NODE_FIELD)` | blind_cast_object_field.iron |
| printer.c:212 | `(Iron_InterfaceDecl *)node` | Yes -- `case IRON_NODE_INTERFACE_DECL:` | -- | OK | -- |
| printer.c:217 | `(Iron_FuncDecl *)n->method_sigs[i]` | No -- relies on construction context (method_sigs contains FuncDecl nodes) | L | Add `assert(n->method_sigs[i]->kind == IRON_NODE_FUNC_DECL)` | blind_cast_iface_sig.iron |
| printer.c:233 | `(Iron_EnumDecl *)node` | Yes -- `case IRON_NODE_ENUM_DECL:` | -- | OK | -- |
| printer.c:239 | `(Iron_EnumVariant *)n->variants[i]` | No -- relies on construction context | L | Add kind assertion | blind_cast_enum_variant_printer.iron |
| printer.c:257 | `(Iron_FuncDecl *)node` | Yes -- `case IRON_NODE_FUNC_DECL:` | -- | OK | -- |
| printer.c:274 | `(Iron_MethodDecl *)node` | Yes -- `case IRON_NODE_METHOD_DECL:` | -- | OK | -- |
| printer.c:292 | `(Iron_ValDecl *)node` | Yes -- `case IRON_NODE_VAL_DECL:` | -- | OK | -- |
| printer.c:306 | `(Iron_VarDecl *)node` | Yes -- `case IRON_NODE_VAR_DECL:` | -- | OK | -- |
| printer.c:319 | `(Iron_AssignStmt *)node` | Yes -- `case IRON_NODE_ASSIGN:` | -- | OK | -- |
| printer.c:327 | `(Iron_ReturnStmt *)node` | Yes -- `case IRON_NODE_RETURN:` | -- | OK | -- |
| printer.c:339 | `(Iron_IfStmt *)node` | Yes -- `case IRON_NODE_IF:` | -- | OK | -- |
| printer.c:357 | `(Iron_WhileStmt *)node` | Yes -- `case IRON_NODE_WHILE:` | -- | OK | -- |
| printer.c:366 | `(Iron_ForStmt *)node` | Yes -- `case IRON_NODE_FOR:` | -- | OK | -- |
| printer.c:384 | `(Iron_MatchStmt *)node` | Yes -- `case IRON_NODE_MATCH:` | -- | OK | -- |
| printer.c:391 | `(Iron_MatchCase *)n->cases[i]` | No -- relies on construction context | L | Add `assert(n->cases[i]->kind == IRON_NODE_MATCH_CASE)` | blind_cast_match_case.iron |
| printer.c:417 | `(Iron_DeferStmt *)node` | Yes -- `case IRON_NODE_DEFER:` | -- | OK | -- |
| printer.c:424 | `(Iron_FreeStmt *)node` | Yes -- `case IRON_NODE_FREE:` | -- | OK | -- |
| printer.c:431 | `(Iron_LeakStmt *)node` | Yes -- `case IRON_NODE_LEAK:` | -- | OK | -- |
| printer.c:439 | `(Iron_SpawnStmt *)node` | Yes -- `case IRON_NODE_SPAWN:` | -- | OK | -- |
| printer.c:457 | `(Iron_IntLit *)node` | Yes -- `case IRON_NODE_INT_LIT:` | -- | OK | -- |
| printer.c:462 | `(Iron_FloatLit *)node` | Yes -- `case IRON_NODE_FLOAT_LIT:` | -- | OK | -- |
| printer.c:467 | `(Iron_StringLit *)node` | Yes -- `case IRON_NODE_STRING_LIT:` | -- | OK | -- |
| printer.c:472 | `(Iron_InterpString *)node` | Yes -- `case IRON_NODE_INTERP_STRING:` | -- | OK | -- |
| printer.c:477 | `(Iron_StringLit *)part` | Yes -- `part->kind == IRON_NODE_STRING_LIT` check at line 476 | -- | OK | -- |
| printer.c:492 | `(Iron_BoolLit *)node` | Yes -- `case IRON_NODE_BOOL_LIT:` | -- | OK | -- |
| printer.c:508 | `(Iron_BinaryExpr *)node` | Yes -- `case IRON_NODE_BINARY:` | -- | OK | -- |
| printer.c:519 | `(Iron_UnaryExpr *)node` cast and `(Iron_TokenKind)n->op` | Yes -- `case IRON_NODE_UNARY:` | -- | OK | -- |
| printer.c:529 | `(Iron_CallExpr *)node` | Yes -- `case IRON_NODE_CALL:` | -- | OK | -- |
| printer.c:536 | `(Iron_MethodCallExpr *)node` | Yes -- `case IRON_NODE_METHOD_CALL:` | -- | OK | -- |
| printer.c:543 | `(Iron_FieldAccess *)node` | Yes -- `case IRON_NODE_FIELD_ACCESS:` | -- | OK | -- |
| printer.c:550 | `(Iron_IndexExpr *)node` | Yes -- `case IRON_NODE_INDEX:` | -- | OK | -- |
| printer.c:559 | `(Iron_SliceExpr *)node` | Yes -- `case IRON_NODE_SLICE:` | -- | OK | -- |
| printer.c:571 | `(Iron_LambdaExpr *)node` | Yes -- `case IRON_NODE_LAMBDA:` | -- | OK | -- |
| printer.c:584 | `(Iron_HeapExpr *)node` | Yes -- `case IRON_NODE_HEAP:` | -- | OK | -- |
| printer.c:590 | `(Iron_RcExpr *)node` | Yes -- `case IRON_NODE_RC:` | -- | OK | -- |
| printer.c:596 | `(Iron_ComptimeExpr *)node` | Yes -- `case IRON_NODE_COMPTIME:` | -- | OK | -- |
| printer.c:604 | `(Iron_IsExpr *)node` | Yes -- `case IRON_NODE_IS:` | -- | OK | -- |
| printer.c:611 | `(Iron_AwaitExpr *)node` | Yes -- `case IRON_NODE_AWAIT:` | -- | OK | -- |
| printer.c:619 | `(Iron_ConstructExpr *)node` | Yes -- `case IRON_NODE_CONSTRUCT:` | -- | OK | -- |
| printer.c:634 | `(Iron_ArrayLit *)node` | Yes -- `case IRON_NODE_ARRAY_LIT:` | -- | OK | -- |
| printer.c:662 | `(Iron_Field *)node` | Yes -- `case IRON_NODE_FIELD:` | -- | OK | -- |
| printer.c:672 | `(Iron_MatchCase *)node` | Yes -- `case IRON_NODE_MATCH_CASE:` | -- | OK | -- |
| printer.c:684 | `(Iron_EnumVariant *)node` | Yes -- `case IRON_NODE_ENUM_VARIANT:` | -- | OK | -- |
| printer.c:703 | `(Iron_Pattern *)node` | Yes -- `case IRON_NODE_PATTERN:` | -- | OK | -- |
| printer.c:722 | `(Iron_EnumConstruct *)node` | Yes -- `case IRON_NODE_ENUM_CONSTRUCT:` | -- | OK | -- |

**Parser blind cast total: 5 unguarded (all L severity)**. All unguarded casts are in array-iteration contexts where the array was populated exclusively with the expected type at construction time.

### Lexer: lexer.c, lexer.h

| File:Line | Cast Expression | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|---------------|----------|---------------|-------------------|
| lexer.c:N/A | No node-type casts in lexer | -- | -- | -- | -- |

No blind casts found in `src/lexer/`. The lexer does not operate on AST node types; it produces tokens which are plain structs, not tagged unions requiring kind-checked casts.

---

## Enum Switch Exhaustiveness (AUDIT-02)

### Parser: ast.c

| File:Line | Switch Subject | Enum Type | Missing Values | Default Handling | Severity | Suggested Fix | Regression Fixture |
|-----------|---------------|-----------|----------------|------------------|----------|---------------|-------------------|
| ast.c:87 | `root->kind` | Iron_NodeKind | None -- all IRON_NODE_COUNT values handled (43 cases + COUNT sentinel) | No default | -- | OK -- exhaustive | -- |

### Parser: parser.c

| File:Line | Switch Subject | Enum Type | Missing Values | Default Handling | Severity | Suggested Fix | Regression Fixture |
|-----------|---------------|-----------|----------------|------------------|----------|---------------|-------------------|
| parser.c:642 | `iron_infix_prec(k)` switch on Iron_TokenKind | Iron_TokenKind | Many non-operator tokens missing | `default: return PREC_NONE` -- correct behavior for non-operator tokens | -- | OK -- intentional | -- |
| parser.c:674 | `iron_parse_primary(t->kind)` switch on Iron_TokenKind | Iron_TokenKind | Many non-expression-start tokens missing | `default: break` then falls through to error diagnostic | -- | OK -- intentional | -- |
| parser.c:1861 | `iron_parse_stmt(t->kind)` switch on Iron_TokenKind | Iron_TokenKind | Many non-statement-start tokens missing | `default:` falls through to expression-statement parsing | -- | OK -- intentional | -- |
| parser.c:2606 | `iron_parse_decl` switch on Iron_TokenKind | Iron_TokenKind | Many non-declaration tokens missing | `default:` emits error + syncs to toplevel | -- | OK -- intentional | -- |

### Parser: printer.c

| File:Line | Switch Subject | Enum Type | Missing Values | Default Handling | Severity | Suggested Fix | Regression Fixture |
|-----------|---------------|-----------|----------------|------------------|----------|---------------|-------------------|
| printer.c:28 | `(Iron_TokenKind)op` | Iron_TokenKind | All bitwise ops (AMP, PIPE, CARET, TILDE, SHL, SHR), PERCENT_ASSIGN, SHL_ASSIGN, SHR_ASSIGN, AMP_ASSIGN, PIPE_ASSIGN, CARET_ASSIGN missing from op_str | `default: return "?"` -- silently returns "?" for unhandled operators | M | Add cases for all bitwise operators (& | ^ ~ << >> and compound assignments) | switch_op_str_bitwise.iron |
| printer.c:158 | `node->kind` in print_node | Iron_NodeKind | None -- all IRON_NODE_COUNT values handled | No default | -- | OK -- exhaustive | -- |

**Parser enum switch total: 1 gap (M severity)** -- printer.c:28 `op_str` silently returns "?" for all Phase 59 bitwise operators.

### Lexer: lexer.c

| File:Line | Switch Subject | Enum Type | Missing Values | Default Handling | Severity | Suggested Fix | Regression Fixture |
|-----------|---------------|-----------|----------------|------------------|----------|---------------|-------------------|
| lexer.c:551 | `c` (char) in `iron_lex_punctuation` | char dispatch (not Iron enum) | All non-punctuation chars | `default:` emits IRON_ERR_INVALID_CHAR diagnostic + returns ERROR token | -- | OK -- intentional | -- |
| lexer.c:73-162 | `kw_kind_names[IRON_TOK_COUNT]` designated initializer | Iron_TokenKind | None -- all IRON_TOK_COUNT values have an entry | No switch (array lookup) | -- | OK -- exhaustive | -- |
| lexer.c:164-168 | `kind < 0 \|\| kind >= IRON_TOK_COUNT` in `iron_token_kind_str` | Iron_TokenKind | Boundary check present | Returns "IRON_TOK_UNKNOWN" for out-of-range | -- | OK | -- |

No issues found in lexer.c. The lexer does not switch over `Iron_TokenKind` for dispatch -- it uses character-level switching for lexing and a sorted table for keyword lookup.

---

## Null Safety (AUDIT-03)

### Parser: ast.h

| File:Line | Dereference Expression | Null Source | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------------|-------------|---------------|----------|---------------|-------------------|
| ast.h:N/A | No pointer dereferences in header | -- | -- | -- | -- | -- |

### Parser: ast.c

| File:Line | Dereference Expression | Null Source | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------------|-------------|---------------|----------|---------------|-------------------|
| ast.c:81-82 | `root`, `v`, `v->visit_node` | Function params | Yes -- `if (!root \|\| !v \|\| !v->visit_node) return` | -- | OK | -- |
| ast.c:84 | `v->visit_node(v, root)` | After null check | Yes | -- | OK | -- |
| ast.c:347 | `v->post_visit(v, root)` | Callback pointer | Yes -- `if (v->post_visit)` check | -- | OK | -- |

### Parser: parser.c

| File:Line | Dereference Expression | Null Source | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------------|-------------|---------------|----------|---------------|-------------------|
| parser.c:78 | `p->tokens[p->token_count - 1]` | Could underflow if token_count==0 | No -- relies on caller providing token_count >= 1 (EOF token) | M | Add `assert(p->token_count > 0)` in `iron_parser_create` | null_empty_token_stream.iron |
| parser.c:147 | `ARENA_ALLOC(p->arena, Iron_ErrorNode)` return value | Arena alloc can fail (OOM) | No -- result dereferenced at line 148 without null check | M | Check return value of ARENA_ALLOC; return NULL on OOM | null_arena_oom.iron |
| parser.c:225-228 | `iron_arena_alloc` return and `memcpy(arena_elems, ...)` | Arena alloc | No -- arena_elems is dereferenced at memcpy without null check | M | Add null check before memcpy | null_tuple_type_oom.iron |
| parser.c:231 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | Arena alloc | No -- no null check | M | Same pattern as above | -- |
| parser.c:244 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | Arena alloc | No -- no null check | M | Add null check | -- |
| parser.c:344 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | Arena alloc | No -- no null check | M | Add null check | -- |
| parser.c:397 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | Arena alloc | No -- no null check | M | Add null check | -- |
| parser.c:458 | `ARENA_ALLOC(p->arena, Iron_Ident)` | Arena alloc | No -- no null check | M | Add null check | -- |
| parser.c:513 | `ARENA_ALLOC(p->arena, Iron_Param)` | Arena alloc | No -- no null check | M | Add null check | -- |
| parser.c:580 | `ARENA_ALLOC(p->arena, Iron_Block)` | Arena alloc | No -- no null check | M | Add null check | -- |
| parser.c:628-630 | `ARENA_ALLOC(p->arena, Iron_LambdaExpr)` + `body->span` | Arena alloc; body could be error node | No null check on ARENA_ALLOC; body null check absent | M | Add null check for ARENA_ALLOC; check body before span access | null_lambda_body.iron |
| parser.c:737-739 | `operand->span` | iron_parse_expr_prec can return error node | Operand guaranteed non-null (iron_parse_primary always returns) | -- | OK | -- |
| parser.c:810-818 | `ARENA_ALLOC(p->arena, Iron_ArrayLit)` + arena_elems | Arena alloc | No null check | M | Add null check | -- |
| parser.c:826-827 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | Arena alloc | No null check | M | Add null check | -- |
| parser.c:1589-1591 | `malloc(len + 1)` return -> `lit_buf` | malloc | Yes -- checked at line 1590 `if (!lit_buf) return` | -- | OK | -- |
| parser.c:1623 | `malloc(expr_len + 1)` return -> `expr_buf` | malloc | Yes -- checked at line 1624 `if (expr_buf)` | -- | OK | -- |
| parser.c:1632 | `iron_lex_all(&el)` return -> `expr_toks` | stb_ds alloc | No -- result dereferenced without null check | M | Check if expr_toks is NULL | null_interp_lex.iron |
| parser.c:1641 | `iron_parse_expr_prec(&sub, PREC_NONE)` return | Parser | Yes -- checked at line 1645 | -- | OK | -- |
| parser.c:1970 | `path_buf` fixed-size buffer (256 bytes) | Import path > 256 chars | No bounds check, writes truncate silently | L | Add diagnostic for overly long import paths or use dynamic buffer | null_long_import.iron |

### Parser: printer.c

| File:Line | Dereference Expression | Null Source | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------------|-------------|---------------|----------|---------------|-------------------|
| printer.c:55-56 | `node` parameter | Could be NULL | Yes -- `if (!node) return` at line 56 | -- | OK | -- |
| printer.c:88-89 | `node` parameter in print_param | Could be NULL | Yes -- `if (!node \|\| node->kind != IRON_NODE_PARAM) return` | -- | OK | -- |
| printer.c:155-156 | `node` parameter in print_node | Could be NULL | Yes -- `if (!node) return` | -- | OK | -- |
| printer.c:394 | `mc->body->kind` | mc->body could be NULL (theoretical) | No -- body set by parser construction | L | Defensive null check | null_match_body.iron |
| printer.c:675 | `mc->body->kind` | mc->body could be NULL | No -- body set by parser construction | L | Defensive null check | -- |
| printer.c:739-747 | `iron_strbuf_create(512)` + `iron_arena_alloc(arena, len + 1, 1)` | Arena alloc | `out` checked at line 752 via ternary | -- | OK | -- |

**Parser null safety total: 15 unguarded (12 M, 3 L)**. The M-severity findings are predominantly missing null checks on ARENA_ALLOC return values -- the arena allocator returns NULL on OOM, and every call site dereferences the result immediately.

### Lexer: lexer.c

| File:Line | Dereference Expression | Null Source | Guard Present | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------------|-------------|---------------|----------|---------------|-------------------|
| lexer.c:176 | `l.src_len = src ? strlen(src) : 0` | `src` function parameter | Yes -- ternary null check | -- | OK | -- |
| lexer.c:189 | `l->src[l->pos]` in `iron_peek_char` | src buffer | Yes -- bounds check `l->pos >= l->src_len` at line 189 | -- | OK | -- |
| lexer.c:194 | `l->src[l->pos + 1]` in `iron_peek_next` | src buffer | Yes -- bounds check `l->pos + 1 >= l->src_len` at line 194 | -- | OK | -- |
| lexer.c:199 | `l->src[l->pos++]` in `iron_advance_char` | src buffer | Yes -- bounds check at line 199 | -- | OK | -- |
| lexer.c:252 | `iron_arena_alloc(l->arena, buf_cap, 1)` return -> `buf` | Arena alloc | Partially -- checked at line 253 (`if (!buf) buf_cap = 0`) but then re-allocated at line 265 | L | Redundant first allocation; only the second matters | -- |
| lexer.c:265-266 | `iron_arena_alloc(l->arena, 4096, 1)` return -> `buf` | Arena alloc | Yes -- checked at line 266 (`if (!buf)`) with error return | -- | OK | -- |
| lexer.c:363 | `iron_arena_strdup(l->arena, buf, buf_len)` | Arena strdup can return NULL | No -- result stored into `value` and returned in token without null check | M | Add null check; return error token on NULL | null_lexer_strdup.iron |
| lexer.c:424 | `iron_arena_strdup(l->arena, decbuf, strlen(decbuf))` | Arena strdup for hex value | No null check | M | Add null check | null_lexer_hex_strdup.iron |
| lexer.c:470 | `iron_arena_strdup(l->arena, decbuf, strlen(decbuf))` | Arena strdup for binary value | No null check | M | Same | -- |
| lexer.c:510 | `iron_arena_strdup(l->arena, l->src + start_pos, tok_len)` | Arena strdup for decimal value | No null check | M | Same | null_lexer_num_strdup.iron |
| lexer.c:532 | `iron_arena_strdup(l->arena, l->src + start_pos, tok_len)` | Arena strdup for identifier | No null check | M | Same | null_lexer_ident_strdup.iron |
| lexer.c:632 | `iron_arena_strdup(l->arena, msg, strlen(msg))` | Arena strdup for error message | No null check | L | Unlikely to matter (diagnostic message) | -- |
| lexer.c:762 | `iron_arena_strdup(l->arena, msg, strlen(msg))` | Arena strdup for error message | No null check | L | Same | -- |

**Lexer null safety total: 7 unguarded (5 M, 2 L)**. The M-severity findings are unchecked `iron_arena_strdup` return values in token-producing paths. If strdup returns NULL, tokens will have NULL `value` fields which downstream consumers dereference.

---

## Arena Lifetimes (AUDIT-04)

### Parser: ast.h, ast.c

| File:Line | Arena Source | Destination | Cross-Arena? | Severity | Suggested Fix | Regression Fixture |
|-----------|-------------|-------------|--------------|----------|---------------|-------------------|
| ast.h:N/A | No arena usage in header | -- | -- | -- | -- | -- |
| ast.c:N/A | No arena allocations in ast.c | -- | -- | -- | -- | -- |

### Parser: parser.c

| File:Line | Arena Source | Destination | Cross-Arena? | Severity | Suggested Fix | Regression Fixture |
|-----------|-------------|-------------|--------------|----------|---------------|-------------------|
| parser.c:147 | `p->arena` (parser arena) | `Iron_ErrorNode` -- stored into AST | No -- AST lifetime == arena lifetime | -- | OK | -- |
| parser.c:225 | `p->arena` | `arena_elems` array -- stored into TypeAnnotation node on same arena | No | -- | OK | -- |
| parser.c:231 | `p->arena` | `Iron_TypeAnnotation` -- stored into AST | No | -- | OK | -- |
| parser.c:244-337 | `p->arena` | Various TypeAnnotation and AST nodes | No -- all same arena | -- | OK | -- |
| parser.c:816-818 | `p->arena` | `arena_elems` for tuple literal -- stored into ArrayLit on same arena | No | -- | OK | -- |
| parser.c:1600-1603 | `p->arena` (via ARENA_ALLOC + iron_arena_strdup) | InterpString parts (stb_ds `arrput`) -- stb_ds array stored as `n->parts` | Yes -- stb_ds heap pointers stored into arena-allocated node | M | Transfer stb_ds array into arena allocation before return, or document that InterpString.parts array must be freed separately | arena_interp_parts.iron |
| parser.c:1604 | `n->parts` array pushed via `arrput` | stb_ds dynamic array | Yes -- stb_ds manages its own heap, but the node lives on arena | M | Same as above -- stb_ds array leak on arena reset | -- |

Note: Throughout parser.c, `arrput`/`arrfree` (stb_ds) is used extensively for temporary arrays during parsing. Most are transferred to arena before return (e.g., lines 225-229, 816-820, 1720-1724). However, several arrays are NOT transferred:

| File:Line | stb_ds Array | Transferred to Arena? | Severity | Suggested Fix | Regression Fixture |
|-----------|-------------|----------------------|----------|---------------|-------------------|
| parser.c:372 | `ann->func_params` | No -- stb_ds pointer stored directly into arena node | M | Copy to arena allocation before storing | arena_func_params.iron |
| parser.c:414-421 | `ann->generic_args` | No -- stb_ds pointer stored directly into arena node | M | Copy to arena allocation before storing | arena_generic_args.iron |
| parser.c:469 | generic_params from `iron_parse_generic_params` | No -- stb_ds pointer stored directly | M | Copy to arena allocation | arena_generic_params.iron |
| parser.c:527 | param_list from `iron_parse_param_list` | No -- stb_ds pointer stored directly | M | Copy to arena allocation | arena_param_list.iron |
| parser.c:558 | `stmts` from `iron_parse_block` | No -- stb_ds pointer stored directly into Iron_Block | M | Copy to arena allocation | arena_block_stmts.iron |
| parser.c:601-602 | `args` from `iron_parse_call_args` | No -- stb_ds pointer stored directly | M | Copy to arena allocation | arena_call_args.iron |
| parser.c:1214-1226 | `elif_conds` / `elif_bodies` | No -- stb_ds pointer stored directly into Iron_IfStmt | M | Copy to arena allocation | arena_elif.iron |
| parser.c:1409-1411 | `cases` in match statement | No -- stb_ds pointer stored directly into Iron_MatchStmt | M | Copy to arena allocation | arena_match_cases.iron |
| parser.c:1442-1443 | `blk->stmts` via arrput into arena-allocated block | No -- stb_ds pointer stored directly | M | Copy to arena allocation | -- |
| parser.c:2335-2336 | `impl_names` in object decl | No -- stb_ds pointer stored directly into Iron_ObjectDecl | M | Copy to arena allocation | arena_impl_names.iron |
| parser.c:2392 | `fields` in object decl | No -- stb_ds pointer stored directly | M | Copy to arena allocation | -- |
| parser.c:2484 | `method_sigs` in interface decl | No -- stb_ds pointer stored directly | M | Copy to arena allocation | -- |
| parser.c:2527-2573 | `variants` in enum decl, also `v->payload_type_anns` | No -- stb_ds pointers stored directly | M | Copy to arena allocation | arena_enum_variants.iron |
| parser.c:2697-2719 | `decls` in top-level program | No -- stb_ds pointer stored directly | M | Copy to arena allocation | arena_program_decls.iron |
| parser.c:925-938 | `elems` in array literal | No -- stb_ds pointer stored directly into Iron_ArrayLit | M | Copy to arena allocation | arena_array_elems.iron |

**Parser arena lifetime total: 17 cross-arena findings (all M severity)**. The dominant pattern is stb_ds heap-managed arrays being stored into arena-allocated AST nodes. When the arena is freed, the stb_ds arrays become leaks (the stb_ds header is never `arrfree`'d). Conversely, if stb_ds arrays are freed while the AST is live, the AST has dangling pointers. This is a pervasive design choice rather than an isolated bug -- the parser relies on the arena and stb_ds arrays having the same practical lifetime.

### Lexer: lexer.c

| File:Line | Arena Source | Destination | Cross-Arena? | Severity | Suggested Fix | Regression Fixture |
|-----------|-------------|-------------|--------------|----------|---------------|-------------------|
| lexer.c:252-265 | `l->arena` | `buf` string buffer -- used locally, copied to arena at line 363 via `iron_arena_strdup` | No -- temporary allocation within same arena; slightly wasteful (4KB upfront) but lifetime-safe | L | Could use stack/malloc for temp buffer to avoid wasting arena space | arena_lexer_string_buf.iron |
| lexer.c:777 | stb_ds `tokens` array | Returned to caller | No -- caller responsibility to `arrfree(tokens)` | -- | OK -- documented in header | -- |

No cross-arena pointer storage issues found in lexer. The lexer allocates token values into the arena and returns a stb_ds token array to the caller. The caller (parser) reads from the stb_ds array but does not store stb_ds array pointers into arena-allocated structures -- the token array has independent lifetime.

### Parser: printer.c

| File:Line | Arena Source | Destination | Cross-Arena? | Severity | Suggested Fix | Regression Fixture |
|-----------|-------------|-------------|--------------|----------|---------------|-------------------|
| printer.c:740 | `iron_strbuf_create(512)` -- heap | StrBuf copied into arena at line 747 | No -- heap buffer freed at line 751 | -- | OK | -- |

---

## Integer Safety (AUDIT-05)

### Parser: ast.h

| File:Line | Expression | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|-------|----------|---------------|-------------------|
| ast.h:109 | `int decl_count` | All count fields use `int` but stb_ds `arrlen` returns `ptrdiff_t` (signed). Truncation possible if stb_ds array exceeds INT_MAX. | L | Use `int` is fine for practical parser limits; document assumption | -- |

### Parser: ast.c

| File:Line | Expression | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|-------|----------|---------------|-------------------|
| ast.c:60 | `kind < 0` | Enum comparison with 0 -- technically valid since Iron_NodeKind could be negative if compiler picks signed representation. Minor warning on some compilers. | L | Cast to `unsigned` or use `(unsigned)kind >= IRON_NODE_COUNT` | int_node_kind_sign.c |

### Parser: parser.c

| File:Line | Expression | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|-------|----------|---------------|-------------------|
| parser.c:213 | `int count = (int)arrlen(elems)` | `arrlen` returns `ptrdiff_t`; cast to int truncates on very large arrays | L | Acceptable for parser tuple elements (always small) | -- |
| parser.c:226 | `sizeof(Iron_Node *) * (size_t)count` | count known >= 2 here, safe | -- | OK | -- |
| parser.c:790 | `int count = (int)arrlen(elems)` | Same pattern as 213 | L | Acceptable for tuple literal elements | -- |
| parser.c:1710 | `int count = (int)arrlen(names)` | Same pattern | L | Acceptable for destructure bindings | -- |
| parser.c:1971-1989 | `path_len` and `sizeof(path_buf)` mixed with `size_t` | Buffer size calculation is sound but import path truncates silently at 256 chars | L | Add overflow check or use dynamic buffer | int_import_path_overflow.iron |
| parser.c:2570 | `(int)atoi(num->value)` | `atoi` has undefined behavior on overflow; no range validation for explicit enum variant values | M | Use `strtol` with range check | int_enum_value_overflow.iron |

### Parser: printer.c

| File:Line | Expression | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|-------|----------|---------------|-------------------|
| printer.c:N/A | No integer safety issues found in printer.c | -- | -- | -- | -- |

**Parser integer safety total: 6 findings (1 M, 5 L)**. The M-severity finding is `atoi` usage for enum variant explicit values without overflow detection.

### Lexer: lexer.c

| File:Line | Expression | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|-----------|-------|----------|---------------|-------------------|
| lexer.c:164 | `kind < 0` in `iron_token_kind_str` | Same pattern as ast.c:60 -- enum comparison with 0, technically valid but may warn | L | Use `(unsigned)kind >= IRON_TOK_COUNT` | int_token_kind_sign.c |
| lexer.c:237 | `size_t start_pos = l->pos` mixed with `uint32_t start_line/start_col` | `size_t` for position, `uint32_t` for line/col -- file >4GB would overflow `uint32_t` position arithmetic in `tok_len = (uint32_t)(l->pos - start_pos)` | L | Acceptable for practical source file sizes | -- |
| lexer.c:403-404 | `hexbuf[64]` with `copy_len = digits_len < sizeof(hexbuf)-1` | Hex literal truncated at 63 hex digits (252 bits) -- but `strtoll` range-checks afterwards | L | Acceptable -- strtoll catches overflow | -- |
| lexer.c:449-450 | `binbuf[128]` with same truncation pattern | Binary literal truncated at 127 binary digits | L | Acceptable | -- |
| lexer.c:410-421 | `strtoll(hexbuf, NULL, 16)` with ERANGE check | Properly range-checked | -- | OK | -- |
| lexer.c:455-467 | `strtoll(binbuf, NULL, 2)` with ERANGE check | Properly range-checked | -- | OK | -- |
| lexer.c:278 | `PUSH_CHAR macro: if (buf_len < buf_max - 1)` | String content silently truncated at 4095 bytes | M | Emit diagnostic when string exceeds buffer, or use dynamic resizing | int_lexer_string_truncation.iron |

**Lexer integer safety total: 5 findings (1 M, 4 L)**. The M-severity finding is silent truncation of string literal content at 4095 bytes (the PUSH_CHAR macro drops characters beyond `buf_max - 1` without any diagnostic).

---

## Allocation Error Handling (AUDIT-06)

### Parser: ast.h, ast.c

| File:Line | Allocation Call | Return Checked? | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|-----------------|----------|---------------|-------------------|
| ast.h:N/A | No allocations in header | -- | -- | -- | -- |
| ast.c:N/A | No allocations in ast.c | -- | -- | -- | -- |

### Parser: parser.c

All ARENA_ALLOC calls in parser.c (approximately 80+) use the `ARENA_ALLOC` macro which wraps `iron_arena_alloc`. None of these check the return value for NULL.

| File:Line | Allocation Call | Return Checked? | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|-----------------|----------|---------------|-------------------|
| parser.c:147 | `ARENA_ALLOC(p->arena, Iron_ErrorNode)` | No | M | Add null check; return NULL on OOM | alloc_error_node.iron |
| parser.c:225 | `iron_arena_alloc(p->arena, ...)` for arena_elems | No | M | Add null check | alloc_tuple_elems.iron |
| parser.c:231 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | No | M | Add null check | -- |
| parser.c:244 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | No | M | Add null check | -- |
| parser.c:344 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | No | M | Add null check | -- |
| parser.c:397 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` | No | M | Add null check | -- |
| parser.c:458 | `ARENA_ALLOC(p->arena, Iron_Ident)` | No | M | Add null check | -- |
| parser.c:513 | `ARENA_ALLOC(p->arena, Iron_Param)` | No | M | Add null check | -- |
| parser.c:580 | `ARENA_ALLOC(p->arena, Iron_Block)` | No | M | Add null check | -- |
| parser.c:628 | `ARENA_ALLOC(p->arena, Iron_LambdaExpr)` | No | M | Add null check | -- |
| parser.c:678 | `ARENA_ALLOC(p->arena, Iron_IntLit)` | No | M | Add null check | -- |
| parser.c:685 | `ARENA_ALLOC(p->arena, Iron_FloatLit)` | No | M | Add null check | -- |
| parser.c:695 | `ARENA_ALLOC(p->arena, Iron_StringLit)` | No | M | Add null check | -- |
| parser.c:710 | `ARENA_ALLOC(p->arena, Iron_BoolLit)` (x2 for true/false) | No | M | Add null check | -- |
| parser.c:727 | `ARENA_ALLOC(p->arena, Iron_NullLit)` | No | M | Add null check | -- |
| parser.c:737 | `ARENA_ALLOC(p->arena, Iron_UnaryExpr)` (x3 for -, not, ~) | No | M | Add null check | -- |
| parser.c:810 | `ARENA_ALLOC(p->arena, Iron_ArrayLit)` | No | M | Add null check | -- |
| parser.c:816 | `iron_arena_alloc(...)` for tuple arena_elems | No | M | Add null check | -- |
| parser.c:826 | `ARENA_ALLOC(p->arena, Iron_TypeAnnotation)` for tuple sentinel | No | M | Add null check | -- |
| parser.c:846 | `ARENA_ALLOC(p->arena, Iron_HeapExpr)` | No | M | Add null check | -- |
| parser.c:858 | `ARENA_ALLOC(p->arena, Iron_RcExpr)` | No | M | Add null check | -- |
| parser.c:868 | `ARENA_ALLOC(p->arena, Iron_ComptimeExpr)` | No | M | Add null check | -- |
| parser.c:878 | `ARENA_ALLOC(p->arena, Iron_AwaitExpr)` | No | M | Add null check | -- |
| parser.c:895 | `ARENA_ALLOC(p->arena, Iron_ArrayLit)` for array literals | No | M | Add null check | -- |
| parser.c:947 | `ARENA_ALLOC(p->arena, Iron_Ident)` | No | M | Add null check | -- |
| parser.c:1036 | `ARENA_ALLOC(p->arena, Iron_EnumConstruct)` | No | M | Add null check | -- |
| parser.c:1049 | `ARENA_ALLOC(p->arena, Iron_MethodCallExpr)` | No | M | Add null check | -- |
| parser.c:1064 | `ARENA_ALLOC(p->arena, Iron_MethodCallExpr)` | No | M | Add null check | -- |
| parser.c:1082 | `ARENA_ALLOC(p->arena, Iron_EnumConstruct)` | No | M | Add null check | -- |
| parser.c:1096 | `ARENA_ALLOC(p->arena, Iron_FieldAccess)` | No | M | Add null check | -- |
| parser.c:1121 | `ARENA_ALLOC(p->arena, Iron_SliceExpr)` | No | M | Add null check | -- |
| parser.c:1131 | `ARENA_ALLOC(p->arena, Iron_IndexExpr)` | No | M | Add null check | -- |
| parser.c:1149 | `ARENA_ALLOC(p->arena, Iron_CallExpr)` | No | M | Add null check | -- |
| parser.c:1174 | `ARENA_ALLOC(p->arena, Iron_IsExpr)` | No | M | Add null check | -- |
| parser.c:1189 | `ARENA_ALLOC(p->arena, Iron_BinaryExpr)` | No | M | Add null check | -- |
| parser.c:1235 | `ARENA_ALLOC(p->arena, Iron_IfStmt)` | No | M | Add null check | -- |
| parser.c:1255 | `ARENA_ALLOC(p->arena, Iron_WhileStmt)` | No | M | Add null check | -- |
| parser.c:1301 | `ARENA_ALLOC(p->arena, Iron_ForStmt)` | No | M | Add null check | -- |
| parser.c:1386 | `ARENA_ALLOC(p->arena, Iron_Pattern)` | No | M | Add null check | -- |
| parser.c:1438-1443 | `ARENA_ALLOC(p->arena, Iron_Block)` for synthetic match body blocks | No | M | Add null check | -- |
| parser.c:1472-1476 | `ARENA_ALLOC(p->arena, Iron_MatchCase)` | No | M | Add null check | -- |
| parser.c:1503-1507 | `ARENA_ALLOC(p->arena, Iron_MatchCase)` | No | M | Add null check | -- |
| parser.c:1516 | `ARENA_ALLOC(p->arena, Iron_MatchStmt)` | No | M | Add null check | -- |
| parser.c:1555 | `ARENA_ALLOC(p->arena, Iron_SpawnStmt)` | No | M | Add null check | -- |
| parser.c:1575 | `ARENA_ALLOC(p->arena, Iron_InterpString)` | No | M | Add null check | -- |
| parser.c:1589 | `malloc(len + 1)` for lit_buf in interp string | Yes -- checked at line 1590 | -- | OK | -- |
| parser.c:1623 | `malloc(expr_len + 1)` for expr_buf in interp string | Yes -- checked at line 1624 | -- | OK | -- |
| parser.c:1744 | `ARENA_ALLOC(p->arena, Iron_ValDecl)` | No | M | Add null check | -- |
| parser.c:1794 | `ARENA_ALLOC(p->arena, Iron_ValDecl)` | No | M | Add null check | -- |
| parser.c:1844 | `ARENA_ALLOC(p->arena, Iron_VarDecl)` | No | M | Add null check | -- |
| parser.c:1873 | `ARENA_ALLOC(p->arena, Iron_ReturnStmt)` | No | M | Add null check | -- |
| parser.c:1891 | `ARENA_ALLOC(p->arena, Iron_DeferStmt)` | No | M | Add null check | -- |
| parser.c:1900 | `ARENA_ALLOC(p->arena, Iron_FreeStmt)` | No | M | Add null check | -- |
| parser.c:1909 | `ARENA_ALLOC(p->arena, Iron_LeakStmt)` | No | M | Add null check | -- |
| parser.c:1940 | `ARENA_ALLOC(p->arena, Iron_AssignStmt)` | No | M | Add null check | -- |
| parser.c:2009 | `ARENA_ALLOC(p->arena, Iron_ImportDecl)` | No | M | Add null check | -- |
| parser.c:2030 | `iron_arena_alloc(arena, len + 1, 1)` for snake_to_camel | No | M | Returns input name on NULL (safe fallback at line 2031) | -- |
| parser.c:2094 | `ARENA_ALLOC(p->arena, Iron_FuncDecl)` | No | M | Add null check | -- |
| parser.c:2177 | `ARENA_ALLOC(p->arena, Iron_MethodDecl)` | No | M | Add null check | -- |
| parser.c:2244 | `ARENA_ALLOC(p->arena, Iron_MethodDecl)` | No | M | Add null check | -- |
| parser.c:2276 | `ARENA_ALLOC(p->arena, Iron_FuncDecl)` | No | M | Add null check | -- |
| parser.c:2383 | `ARENA_ALLOC(p->arena, Iron_Field)` | No | M | Add null check | -- |
| parser.c:2400 | `ARENA_ALLOC(p->arena, Iron_ObjectDecl)` | No | M | Add null check | -- |
| parser.c:2468 | `ARENA_ALLOC(p->arena, Iron_FuncDecl)` for interface sig | No | M | Add null check | -- |
| parser.c:2492 | `ARENA_ALLOC(p->arena, Iron_InterfaceDecl)` | No | M | Add null check | -- |
| parser.c:2541 | `ARENA_ALLOC(p->arena, Iron_EnumVariant)` | No | M | Add null check | -- |
| parser.c:2590 | `ARENA_ALLOC(p->arena, Iron_EnumDecl)` | No | M | Add null check | -- |
| parser.c:2735 | `ARENA_ALLOC(p->arena, Iron_Program)` | No | M | Add null check | -- |

`iron_arena_strdup` calls (~40 instances) also have unchecked return values throughout parser.c.

### Parser: printer.c

| File:Line | Allocation Call | Return Checked? | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|-----------------|----------|---------------|-------------------|
| printer.c:740 | `iron_strbuf_create(512)` -- internal malloc | No -- strbuf may start empty on OOM | L | Check strbuf validity | alloc_printer_strbuf.iron |
| printer.c:747 | `iron_arena_alloc(arena, len + 1, 1)` | Yes -- checked via `out ? out : ""` at line 752 | -- | OK | -- |

**Parser allocation error handling total: ~70 unchecked ARENA_ALLOC calls (all M), ~40 unchecked iron_arena_strdup calls (all M), 2 checked malloc calls, 1 checked arena_alloc. Systemic: the parser assumes arena allocation never fails.**

### Lexer: lexer.c

| File:Line | Allocation Call | Return Checked? | Severity | Suggested Fix | Regression Fixture |
|-----------|----------------|-----------------|----------|---------------|-------------------|
| lexer.c:252 | `iron_arena_alloc(l->arena, buf_cap, 1)` -- initial string buffer | Partially -- checked but then overridden at line 265 | L | Remove redundant first allocation | alloc_lexer_redundant.iron |
| lexer.c:265 | `iron_arena_alloc(l->arena, 4096, 1)` -- string buffer | Yes -- checked at line 266; returns error token on NULL | -- | OK | -- |
| lexer.c:363 | `iron_arena_strdup(l->arena, buf, buf_len)` -- final string value | No | M | Add null check; return error token | alloc_lexer_strdup.iron |
| lexer.c:424 | `iron_arena_strdup(l->arena, decbuf, strlen(decbuf))` -- hex value | No | M | Add null check | -- |
| lexer.c:470 | `iron_arena_strdup(l->arena, decbuf, strlen(decbuf))` -- binary value | No | M | Add null check | -- |
| lexer.c:510 | `iron_arena_strdup(l->arena, l->src + start_pos, tok_len)` -- decimal/float value | No | M | Add null check | -- |
| lexer.c:532 | `iron_arena_strdup(l->arena, l->src + start_pos, tok_len)` -- identifier text | No | M | Add null check | alloc_lexer_ident.iron |
| lexer.c:632 | `iron_arena_strdup(l->arena, msg, strlen(msg))` -- error msg | No | L | Non-critical (diagnostic string) | -- |
| lexer.c:762 | `iron_arena_strdup(l->arena, msg, strlen(msg))` -- error msg | No | L | Non-critical (diagnostic string) | -- |

**Lexer allocation error handling total: 5 unchecked iron_arena_strdup in token-producing paths (all M), 2 unchecked iron_arena_strdup in diagnostic paths (L), 1 checked arena_alloc. Same systemic pattern as parser: arena strdup assumed to never fail.**

---

## Cross-Platform (AUDIT-08)

### Parser: ast.h

| File:Line | Pattern | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|---------|-------|----------|---------------|-------------------|
| ast.h:8 | `#include <stdint.h>` | Standard C99 header -- portable | -- | OK | -- |
| ast.h:7 | `#include <stdbool.h>` | Standard C99 header -- portable | -- | OK | -- |

### Parser: ast.c

| File:Line | Pattern | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|---------|-------|----------|---------------|-------------------|
| ast.c:N/A | No platform-specific code found | -- | -- | -- | -- |

### Parser: parser.c

| File:Line | Pattern | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|---------|-------|----------|---------------|-------------------|
| parser.c:226 | `_Alignof(Iron_Node *)` | C11 `_Alignof` -- requires C11 or later; not available in MSVC prior to C11 mode | L | Use `sizeof(void *)` as alignment, or add `#ifdef _MSC_VER` fallback using `__alignof` | xplat_alignof.c |
| parser.c:816 | `_Alignof(Iron_Node *)` | Same as above | L | Same fix | -- |
| parser.c:1720 | `_Alignof(const char *)` | Same as above | L | Same fix | -- |

### Parser: parser.h

| File:Line | Pattern | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|---------|-------|----------|---------------|-------------------|
| parser.h:N/A | No platform-specific code found | -- | -- | -- | -- |

### Parser: printer.c

| File:Line | Pattern | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|---------|-------|----------|---------------|-------------------|
| printer.c:N/A | No platform-specific code found | -- | -- | -- | -- |

### Parser: printer.h

| File:Line | Pattern | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|---------|-------|----------|---------------|-------------------|
| printer.h:7 | `#include <stdio.h>` for FILE* | Standard C89 header -- portable | -- | OK | -- |

**Parser cross-platform total: 3 findings (all L severity)**. The `_Alignof` usage is C11-only; MSVC in C mode requires `__alignof` instead. The project likely already compiles in C11 mode given other C11 usage, but the finding is documented for completeness.

### Lexer: lexer.c, lexer.h

| File:Line | Pattern | Issue | Severity | Suggested Fix | Regression Fixture |
|-----------|---------|-------|----------|---------------|-------------------|
| lexer.c:7 | `#include <ctype.h>` | Standard C89 header -- portable | -- | OK | -- |
| lexer.c:8 | `#include <errno.h>` | Standard C89 header -- portable | -- | OK | -- |
| lexer.c:387 | `isxdigit((unsigned char)...)` | Correct cast to unsigned char for ctype macros | -- | OK | -- |
| lexer.c:477 | `isdigit((unsigned char)...)` | Correct cast to unsigned char | -- | OK | -- |
| lexer.c:494 | `isalpha((unsigned char)...)` | Correct cast to unsigned char | -- | OK | -- |

No cross-platform issues found in lexer. All ctype macro calls correctly cast to `unsigned char`. No POSIX-only headers, no GCC builtins, no platform-specific code paths.

---

## Summary -- Parser + Lexer

| Dimension | High | Medium | Low | Total |
|-----------|------|--------|-----|-------|
| Blind Casts (AUDIT-01) | 0 | 0 | 5 | 5 |
| Enum Switch Exhaustiveness (AUDIT-02) | 0 | 1 | 0 | 1 |
| Null Safety (AUDIT-03) | 0 | 17 | 5 | 22 |
| Arena Lifetimes (AUDIT-04) | 0 | 17 | 1 | 18 |
| Integer Safety (AUDIT-05) | 0 | 2 | 9 | 11 |
| Allocation Error Handling (AUDIT-06) | 0 | 117 | 5 | 122 |
| Cross-Platform (AUDIT-08) | 0 | 0 | 3 | 3 |
| **Total** | **0** | **154** | **28** | **182** |

### Key Systemic Patterns

1. **Unchecked ARENA_ALLOC / iron_arena_strdup**: 117 M-severity findings across parser.c and lexer.c. The entire parser+lexer assumes arena allocation never fails. A single `iron_arena_alloc_or_abort()` wrapper would eliminate this class.

2. **stb_ds arrays stored into arena nodes**: 17 M-severity findings in parser.c. Arrays built with `arrput` (heap-managed by stb_ds) are stored directly into arena-allocated AST nodes without transfer. This is a deliberate design tradeoff -- transferring all arrays to arena would fix the lifetime mismatch but requires touching ~15 call sites.

3. **No High-severity findings**: All issues are Medium (correctness risk under abnormal conditions like OOM) or Low (theoretical / unlikely). The parser and lexer are well-structured with consistent guard patterns for normal operation.
