# Phase 33: Value & Mutable Captures + Optimizer Guards - Research

**Researched:** 2026-04-02
**Domain:** Iron compiler — parser func-type annotations, optimizer DCE purity, Iron_List_Iron_Closure runtime support
**Confidence:** HIGH

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

- Root-cause and fix the infinite loop when compiling `[func() -> T]` type expressions. Fix must handle all collection types (arrays, lists, maps) containing `Iron_Closure` values.
- DCE purity fix: `MAKE_CLOSURE` is pure when `capture_count == 0`, side-effecting when `capture_count > 0`. Update `iron_lir_instr_is_pure()` in `lir_optimize.c` line 2783.
- OPT-02 already done: function inliner skips `__lambda_*` functions (Phase 32).
- OPT-03 partially done: copy-prop excludes allocas captured by MAKE_CLOSURE (Phase 32). Verify both work correctly with the new test examples.
- TDD approach: create all 8 test files (examples 1-4, 7, 12, 13, 14) upfront with .iron + .expected pairs. Then iterate on compiler fixes until all pass.
- Examples 1-3 already pass. New examples to add: 4, 7, 12, 13, 14.
- Use exact Iron source from `docs/lambda-capture.md` wrapped in `func main() { ... }`.

### Claude's Discretion

- Root-cause investigation approach for the infinite loop (may be in type resolution, HIR lowering, LIR emission, or optimizer)
- Whether to add additional optimizer guards beyond what's specified
- Internal implementation of the DCE conditional check
- Ordering of fixes (hang fix first vs DCE fix first)

### Deferred Ideas (OUT OF SCOPE)

None — discussion stayed within phase scope

</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| CAPT-01 | User can capture an immutable `val` by value in a lambda | Already implemented in Phase 32; examples 1, 3 pass |
| CAPT-02 | User can capture a mutable `var` by reference, mutations visible outside | Already implemented in Phase 32; examples 2, 3 pass |
| CAPT-03 | User can capture multiple variables of different types in one lambda | Already implemented; example 3 passes |
| CAPT-04 | User can capture a loop variable by snapshot | Blocked by parser infinite loop; fix tracked in this phase |
| OPT-01 | DCE does not eliminate MAKE_CLOSURE instructions | `iron_lir_instr_is_pure()` at line 2840 marks MAKE_CLOSURE as pure — must fix |
| OPT-02 | Function inliner skips lifted lambda functions | Already done in Phase 32: guard at lir_optimize.c:3304 |
| OPT-03 | Copy propagation and store-load elimination respect heap-box indirection | Partially done in Phase 32: copy-prop Step 1b guard; store-load-elim invalidates escaped allocas on CALL |

</phase_requirements>

---

## Summary

Phase 33 requires fixing five distinct issues across the compiler pipeline. The most critical is an infinite loop in the parser triggered whenever a function type (`func() -> T`) appears as an element type in an array type annotation (`[func() -> T]`). This blocks examples 4, 7 (partially), 8, and 14. The root cause is a structural limitation: `Iron_TypeAnnotation` (ast.h:204) has no fields to represent function parameter types or return types — it only stores a name string. When the parser encounters `[func() -> Int]` as a type annotation, it allocates `[`, looks for an identifier for the element type name, finds `func` (which is `IRON_TOK_FUNC`, not `IRON_TOK_IDENTIFIER`), emits a parse error, then calls `iron_expect(RBRACKET)` which also fails without advancing. The parser returns to the outer block loop with current token still `]`. The block loop then spins forever generating error nodes, consuming arena memory at >5GB/hour.

A secondary parser issue blocks example 12: `if` cannot be used as an expression in the current parser. `val action = if flag { ... } else { ... }` requires `IRON_TOK_IF` to be handled in `iron_parse_primary`. This is not present — `if` is statement-only.

The optimizer DCE issue (`OPT-01`) is confirmed at `lir_optimize.c:2840` where `IRON_LIR_MAKE_CLOSURE` is in the pure-instruction list. When a closure is stored into an array (e.g., `callbacks.push(...)`) but the MAKE_CLOSURE result itself has no direct downstream use, DCE marks it dead and removes it.

There is also a missing runtime type: `Iron_List_Iron_Closure` is not defined in `iron_runtime.h`. The codegen emits `Iron_List_Iron_Closure_create()` and `Iron_List_Iron_Closure_push()` for `[func() -> Int]` arrays, but no struct or function definitions exist for this type.

**Primary recommendation:** Fix the parser first (func-type annotations + if-as-expression), then add `Iron_List_Iron_Closure` to the runtime, then fix DCE purity for MAKE_CLOSURE.

---

## Root Cause: Parser Infinite Loop

### Confirmed Root Cause

**File:** `src/parser/parser.c`
**Location:** `iron_parse_type_annotation()` lines 190-259 + `iron_parse_block()` lines 352-385

**The token trace for `var callbacks: [func() -> Int] = []`:**

1. `iron_parse_var_decl` sees `:`, calls `iron_parse_type_annotation`
2. `iron_parse_type_annotation` matches `[` (advances past it)
3. `iron_check(p, IRON_TOK_IDENTIFIER)` — FAILS because current token is `func` (IRON_TOK_FUNC, not IRON_TOK_IDENTIFIER)
4. Sets `ann->name = "<error>"`, does NOT advance
5. Calls `iron_expect(p, IRON_TOK_RBRACKET)` — current is `func`, not `]` — emits error but does NOT advance
6. Returns annotation with `name = "<error>"`, position still at `func`

Back in `iron_parse_var_decl`:
- `iron_match(p, IRON_TOK_ASSIGN)` — current is `func`, not `=` — no match, init stays NULL
- Returns var decl with bad type annotation and no initializer

Back in outer block loop (position at `func`):
- `iron_parse_stmt` → default → `iron_parse_expr` → `iron_parse_primary` sees `IRON_TOK_FUNC` → calls `iron_parse_lambda`
- `iron_parse_lambda` advances past `func`, parses `()`, matches `->`, calls `iron_parse_type_annotation` which consumes `Int`
- Then calls `iron_parse_block` expecting `{` but finds `]`
- `iron_expect(LBRACE)` fails, sets `in_error_recovery = true`, returns error node WITHOUT advancing
- `iron_parse_lambda` returns with body = error node
- `iron_parse_stmt` returns ExprStmt(lambda)

Back in block loop (position at `]`):
- `iron_check(p, IRON_TOK_RBRACE)` — `]` is not `}` → false → LOOP CONTINUES
- `iron_parse_stmt` → `iron_parse_expr` → `iron_parse_primary` → `IRON_TOK_RBRACKET` hits `default:` case
- **Line 701:** `/* Do not advance — let the caller handle recovery */` → returns error WITHOUT advancing
- Loop spins forever on `]`, allocating arena memory each iteration

### The Critical Non-Advance Rule

`iron_parse_primary` at line 697-702:
```c
/* Unexpected token in expression position */
iron_emit_diag(p, IRON_ERR_EXPECTED_EXPR,
               iron_token_span(p, t),
               "expected expression");
/* Do not advance — let the caller handle recovery */
return iron_make_error(p);
```

This is intentional design for non-consuming error returns, but the block loop at line 364 has no protection against infinite spin on non-advancing error tokens.

### Fix Location

**Root fix (stops the hang):** `iron_parse_type_annotation` must handle `IRON_TOK_FUNC` to parse `func([ParamType, ...]) [-> ReturnType]` as a function type. This requires:

1. Adding function-type fields to `Iron_TypeAnnotation` in `src/parser/ast.h`:
   ```c
   bool          is_func;
   Iron_Node   **func_params;     /* type annotations for params */
   int           func_param_count;
   Iron_Node    *func_return;     /* return type annotation, NULL = void */
   ```

2. In `iron_parse_type_annotation`, adding a branch before the named-type case to handle `IRON_TOK_FUNC`:
   ```c
   if (iron_match(p, IRON_TOK_FUNC)) {
       /* parse func([T, ...]) [-> R] */
       ann->is_func = true;
       /* parse parameter types from (...) */
       /* parse optional -> ReturnType */
   }
   ```

3. In `resolve_type_annotation` (`src/analyzer/typecheck.c:171`), adding a branch to build `IRON_TYPE_FUNC` when `ann->is_func` is true.

4. In `iron_parse_type_annotation`'s array branch (line 194-224), when element type check fails, advance past the remaining tokens of the func type rather than leaving position at `func`. Simplest safe fix: if `iron_check(p, IRON_TOK_FUNC)`, delegate to the new func-type parsing branch.

**Emergency fix (stops the hang without full type support):** In the array branch of `iron_parse_type_annotation`, after a failed identifier check, scan forward to `]`:
```c
if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
    /* Skip to matching ] */
    while (!iron_check(p, IRON_TOK_RBRACKET) && !iron_check(p, IRON_TOK_EOF)) {
        p->pos++;
    }
    iron_match(p, IRON_TOK_RBRACKET);
    ann->name = "<error>";
    return (Iron_Node *)ann;
}
```
This prevents the infinite loop but does not provide correct type information. The full fix (above) is required for correct semantics.

---

## Issue 2: If-as-Expression (Example 12)

**Example 12** uses `val action = if flag { ... } else { ... }`. The current parser only supports `if` as a statement (handled in `iron_parse_stmt` case `IRON_TOK_IF`). `iron_parse_primary` has no `IRON_TOK_IF` case.

**Fix:** Add `case IRON_TOK_IF:` to `iron_parse_primary` to parse an if-expression. The result is an `Iron_IfStmt` node used in expression context. The HIR lowerer and typecheck already handle `IRON_NODE_IF` nodes — however, `if` as an expression producing a value (the last expression of each branch) requires HIR to emit a PHI node. This is a non-trivial change.

**Scope assessment:** Example 12 may need to be simplified for Phase 33 if if-as-expression proves too large. The simplest alternative rewrite that captures the same semantics:
```iron
var message = "none"
val flag = true
var action_closure: func() = func() {}
if flag {
    action_closure = func() { message = "was true" }
} else {
    action_closure = func() { message = "was false" }
}
action_closure()
println(message)
```
This avoids `if` as an expression. Alternatively, the phase plan should assess if if-as-expression is in scope or if example 12 should use the rewritten form.

---

## Issue 3: DCE Eliminating MAKE_CLOSURE (OPT-01)

**File:** `src/lir/lir_optimize.c`
**Location:** `iron_lir_instr_is_pure()` at line 2823, specifically line 2840

Current code (incorrect):
```c
case IRON_LIR_MAKE_CLOSURE: case IRON_LIR_FUNC_REF:
    return true;  // WRONG for capturing closures
```

When a closure is created inside a loop and pushed to an array:
```iron
val captured = i
callbacks.push(func() -> Int { return captured })
```
The `MAKE_CLOSURE` instruction creates the closure value. If DCE considers it pure and its result value ID has no downstream direct-use instruction (only used transitively via the push call's argument), DCE may remove it.

**Fix (per locked decision):** Make `iron_lir_instr_is_pure()` accept the instruction pointer rather than just the kind, OR add a separate helper for DCE that checks capture_count:

```c
// Option A: Change signature (requires updating all 8 call sites)
bool iron_lir_instr_is_pure(const IronLIR_Instr *instr) {
    ...
    case IRON_LIR_MAKE_CLOSURE:
        return instr->make_closure.capture_count == 0;
    ...
}

// Option B: Keep existing signature unchanged, add inline check in run_dce only
// In run_dce Step 1 seeding:
if (!iron_lir_instr_is_pure(in->kind) ||
    in->id == IRON_LIR_VALUE_INVALID ||
    (in->kind == IRON_LIR_MAKE_CLOSURE && in->make_closure.capture_count > 0)) {
    // mark live
}
```

**Option B is simpler** — it doesn't require changing the function signature or updating 8 call sites. Only DCE needs the fix; copy-prop and inliner already have separate MAKE_CLOSURE guards.

The check belongs in `run_dce` step 1 (line 1229) and step 3 (line 1289):
```c
// Step 1 (seeding):
if (!iron_lir_instr_is_pure(in->kind) ||
    (in->kind == IRON_LIR_MAKE_CLOSURE && in->make_closure.capture_count > 0) ||
    in->id == IRON_LIR_VALUE_INVALID) {
```

```c
// Step 3 (keep-or-remove):
if (!iron_lir_instr_is_pure(in->kind) ||
    (in->kind == IRON_LIR_MAKE_CLOSURE && in->make_closure.capture_count > 0) ||
    in->id == IRON_LIR_VALUE_INVALID) {
    is_live = true;
```

---

## Issue 4: Missing Iron_List_Iron_Closure Runtime Type

**File:** `src/runtime/iron_runtime.h`

The codegen (`emit_c.c:235-253`) converts `IRON_TYPE_ARRAY` with `IRON_TYPE_FUNC` element to `Iron_List_Iron_Closure` C type. It emits:
- `Iron_List_Iron_Closure _vN = Iron_List_Iron_Closure_create();`
- `Iron_List_Iron_Closure_push(&_vN, ...);`
- `Iron_List_Iron_Closure_get(&_vN, idx);`

The runtime only has:
```c
typedef struct Iron_List_int64_t { ... } Iron_List_int64_t;
typedef struct Iron_List_double  { ... } Iron_List_double;
typedef struct Iron_List_bool    { ... } Iron_List_bool;
typedef struct Iron_List_Iron_String { ... } Iron_List_Iron_String;
IRON_LIST_DECL(int64_t, int64_t)
IRON_LIST_DECL(Iron_String, Iron_String)
// ... etc.
```

`Iron_Closure` is defined in `iron_runtime.h` as `{void *env; void (*fn)(void*)}`. The fix adds:
```c
typedef struct Iron_List_Iron_Closure {
    Iron_Closure *items;
    int64_t       count;
    int64_t       capacity;
} Iron_List_Iron_Closure;
IRON_LIST_DECL(Iron_Closure, Iron_Closure)
```

And in the corresponding `.c` file (iron_collections.c or similar):
```c
IRON_LIST_IMPL(Iron_Closure, Iron_Closure)
```

**Important:** The `Iron_Closure` typedef must already be visible before the list struct. Verify include order in `iron_runtime.h`.

---

## Issue 5: Func-Type Parameter Type Annotations (Examples 7, 14)

Example 7: `func apply_twice(f: func()) { ... }` — parameter `f` has type annotation `func()`.
Example 14: `func filter(items: [Int], predicate: func(Int) -> Bool) -> [Int]` — parameter `predicate` has type annotation `func(Int) -> Bool`.

Both require `iron_parse_type_annotation` to handle `IRON_TOK_FUNC` as a non-array type annotation (a plain function type, not `[func()]`). This is the same fundamental fix as issue 1 — once `Iron_TypeAnnotation` gains `is_func` support and `iron_parse_type_annotation` handles `IRON_TOK_FUNC`, these work.

In `resolve_type_annotation` (`typecheck.c:171`), when `ann->is_func`, build:
```c
Iron_Type **param_types = ...; // resolve each ann->func_params[i]
Iron_Type *ret = ann->func_return
    ? resolve_type_annotation(ctx, ann->func_return)
    : iron_type_make_primitive(IRON_TYPE_VOID);
return iron_type_make_func(ctx->arena, param_types, param_count, ret);
```

---

## Standard Stack

### Core Files

| File | Role | Phase 33 Change |
|------|------|-----------------|
| `src/parser/ast.h` | `Iron_TypeAnnotation` struct | Add `is_func`, `func_params`, `func_param_count`, `func_return` |
| `src/parser/parser.c` | `iron_parse_type_annotation()` | Handle `IRON_TOK_FUNC` to parse func-type annotations |
| `src/parser/parser.c` | `iron_parse_primary()` | Add `IRON_TOK_IF` case for if-as-expression (example 12) |
| `src/analyzer/typecheck.c` | `resolve_type_annotation()` | Build `IRON_TYPE_FUNC` from `ann->is_func` annotations |
| `src/lir/lir_optimize.c` | `run_dce()` | Add MAKE_CLOSURE with captures != 0 to non-pure guard |
| `src/runtime/iron_runtime.h` | List type instantiation | Add `Iron_List_Iron_Closure` struct + `IRON_LIST_DECL` |
| `src/runtime/iron_collections.c` | List implementations | Add `IRON_LIST_IMPL(Iron_Closure, Iron_Closure)` |

### Already Correct (No Phase 33 Changes)

| File | Status | Reason |
|------|--------|--------|
| `src/analyzer/capture.c` | Phase 32 complete | Capture analysis with val/var distinction |
| `src/hir/hir_to_lir.c` MAKE_CLOSURE | Phase 32 complete | Correct emit of MAKE_CLOSURE with capture operands |
| `src/lir/emit_c.c` MAKE_CLOSURE | Phase 32 complete | Env struct malloc + field population |
| `src/lir/lir_optimize.c` Step 1b | Phase 32 complete | Copy-prop excludes captured allocas |
| `src/lir/lir_optimize.c` inliner | Phase 32 complete | `__lambda_*` functions skipped at line 3304 |
| `src/lir/emit_c.c` IRON_TYPE_FUNC | Correct | Returns `"Iron_Closure"` — correct C type |
| `src/lir/emit_c.c` IRON_TYPE_ARRAY | Correct | Builds `Iron_List_<elem>` — correct for new type once added |

---

## Architecture Patterns

### Pattern 1: Parser Type Annotation Extension

`Iron_TypeAnnotation` is the single AST node for all type annotations. Extend it with optional function-type fields (NULL when `is_func = false`) to avoid changing existing code paths:

```c
// src/parser/ast.h — extend Iron_TypeAnnotation
typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;
    const char   *name;
    bool          is_nullable;
    Iron_Node   **generic_args;
    int           generic_arg_count;
    bool          is_array;
    Iron_Node    *array_size;
    /* Phase 33 additions: */
    bool          is_func;
    Iron_Node   **func_params;      /* array of Iron_TypeAnnotation* */
    int           func_param_count;
    Iron_Node    *func_return;      /* NULL means void */
} Iron_TypeAnnotation;
```

In `iron_parse_type_annotation`, add a func-type branch BEFORE the named-type branch. For the array branch, the func-type element is handled naturally once the named branch can parse `func(...)`.

### Pattern 2: DCE Conditional Purity (Option B)

Do NOT change `iron_lir_instr_is_pure()` signature. Add inline checks only in `run_dce`. The `instr_is_inline_expressible()` function already has a `MAKE_CLOSURE` exclusion at line 1636 — do not modify that.

Two-site edit in `run_dce`:

```c
// Step 1 — seeding live set (line ~1229):
if (!iron_lir_instr_is_pure(in->kind) ||
    (in->kind == IRON_LIR_MAKE_CLOSURE && in->make_closure.capture_count > 0) ||
    in->id == IRON_LIR_VALUE_INVALID) {

// Step 3 — keep-or-remove (line ~1289):
if (!iron_lir_instr_is_pure(in->kind) ||
    (in->kind == IRON_LIR_MAKE_CLOSURE && in->make_closure.capture_count > 0) ||
    in->id == IRON_LIR_VALUE_INVALID) {
    is_live = true;
```

### Pattern 3: Runtime List Instantiation

The runtime uses `IRON_LIST_DECL` / `IRON_LIST_IMPL` macros to generate type-safe list operations. `Iron_Closure` is already defined earlier in `iron_runtime.h`. Add the instantiation adjacent to the existing `Iron_List_Iron_String` instantiation.

Verify `iron_collections.c` exists and contains the `IRON_LIST_IMPL` calls (it does for the pre-existing types). Add `IRON_LIST_IMPL(Iron_Closure, Iron_Closure)` there.

### Recommended Fix Order

1. **Parser: func-type annotations** — unblocks examples 4, 7, 14
2. **Runtime: Iron_List_Iron_Closure** — unblocks array-of-closures storage
3. **DCE fix** — prevents closures from being dropped (may not manifest until harder examples)
4. **Parser: if-as-expression** — unblocks example 12 (or use simplified rewrite)
5. **Test creation + green run** — TDD style

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| List of closures | Custom closure array type | `IRON_LIST_IMPL(Iron_Closure, Iron_Closure)` macro | Consistent with all other list types in the runtime |
| Func-type purity check | New `is_pure_instr(IronLIR_Instr*)` with full signature change | Inline condition in `run_dce` only | 8 call sites would need updating; other callers don't need the fix |

---

## Common Pitfalls

### Pitfall 1: iron_expect Does Not Advance

**What goes wrong:** When `iron_expect(p, KIND)` fails, it emits a diagnostic but does NOT consume the current token. Any code that relies on `iron_expect` to advance past a token on failure will leave the parser stuck.

**Why it happens:** Intentional design — the return value `NULL` signals failure; the caller decides whether to advance or recover. But some recovery paths forget to advance.

**How to avoid:** After any failed `iron_expect` in an error path, explicitly advance past the offending token if you need to continue parsing.

### Pitfall 2: Iron_TypeAnnotation is_func vs is_array Interaction

**What goes wrong:** `[func() -> Int]` needs `is_array = true` AND the element type to be a func type. The element type cannot be stored as `name = "func"` — the resolver would look for a type named "func" in the symbol table.

**How to avoid:** For array-of-func, the parsing sequence is: parse `[` (set is_array=true), then parse element as a func type annotation (a nested `Iron_TypeAnnotation` with `is_func=true`), store it as the element type. The current `ann->name` field is insufficient — the element type needs to be a full `Iron_Node*`. The simplest compatible approach: set `ann->name = "func"` as sentinel, and use a new field `ann->func_elem_type` (a nested `Iron_TypeAnnotation*`) for the full function type. Alternatively, make `ann->name` point to a stringified form and rely on the full `is_func` interpretation in the resolver.

The cleaner approach: in `resolve_type_annotation`, when `ann->is_array` is true, recursively resolve the element type from `ann->elem_type_ann` (a new `Iron_Node*` field) rather than from `ann->name`. This is a bigger refactor. For Phase 33, the pragmatic approach is:

- For plain `[func() -> T]` (no inner params that matter for type resolution): the element type resolves to `IRON_TYPE_FUNC` with the correct signature
- The `emit_type_to_c` for `IRON_TYPE_FUNC` returns `"Iron_Closure"` regardless of param/return types — this is correct since all closures use the same runtime representation

### Pitfall 3: DCE and MAKE_CLOSURE With capture_count == 0

**What goes wrong:** Non-capturing closures (capture_count == 0) should remain pure for DCE — their result is just a function pointer, no side effect. If you mark ALL MAKE_CLOSURE as non-pure, DCE stops eliminating dead non-capturing closures.

**How to avoid:** The fix specifically checks `capture_count > 0`. Non-capturing closures stay pure.

### Pitfall 4: Iron_List_Iron_Closure in iron_runtime.h Include Order

**What goes wrong:** `Iron_Closure` must be typedef'd before `Iron_List_Iron_Closure` can reference it in the struct definition.

**How to avoid:** `Iron_Closure` is defined early in `iron_runtime.h` (before the collection macros section). The new `Iron_List_Iron_Closure` instantiation belongs in the "pre-instantiated common types" section (after line 560), where other list types are defined.

### Pitfall 5: Example 12 if-as-expression Complexity

**What goes wrong:** Adding `if`-as-expression to the parser requires not just `iron_parse_primary` changes but also HIR and LIR handling for if expressions that produce values. The HIR lowerer emits `IRON_HIR_STMT_IF` which has no return value currently.

**How to avoid:** If if-as-expression is too large for Phase 33, rewrite example 12 to avoid `if` as an expression. The semantics of CAPT-02/CAPT-04 don't strictly require it. Example 12 can be rewritten:
```iron
func main() {
    var message = "none"
    val flag = true
    var action = func() { message = "was true" }
    if !flag {
        action = func() { message = "was false" }
    }
    action()
    println(message)
}
```

### Pitfall 6: Example 7 — Calling a Closure with .fn(env) Pattern

Example 7 requires `apply_twice(func() { total += 10 })`. The `f()` call inside `apply_twice` dispatches through `f.fn(f.env)`. The LIR call dispatch for closures must use the `Iron_Closure` `.fn` field. This is already implemented in `emit_c.c` for closure calls — verify by tracing `IRON_LIR_CALL` with a `IRON_LIR_FUNC_REF` vs closure dispatch.

---

## Code Examples

### Correct Capture Test Structure

```iron
// src: tests/integration/capture_04_loop_snapshot.iron
func main() {
    var callbacks: [func() -> Int] = []
    for i in range(5) {
        val captured = i
        callbacks.push(func() -> Int { return captured })
    }
    println("{callbacks[0]()}")
    println("{callbacks[3]()}")
}
// expected: "0\n3\n"
```

```iron
// src: tests/integration/capture_07_callback_arg.iron
func apply_twice(f: func()) {
    f()
    f()
}
func main() {
    var total = 0
    apply_twice(func() { total += 10 })
    println("{total}")
}
// expected: "20\n"
```

```iron
// src: tests/integration/capture_13_capture_in_match.iron
func main() {
    var result = ""
    val handler = func(code: Int) {
        match code {
            200 -> { result = "ok" }
            404 -> { result = "not found" }
            _   -> { result = "error {code}" }
        }
    }
    handler(404)
    println(result)
}
// expected: "not found\n"
```

```iron
// src: tests/integration/capture_14_filter_with_capture.iron
func filter(items: [Int], predicate: func(Int) -> Bool) -> [Int] {
    var out: [Int] = []
    for item in items {
        if predicate(item) {
            out.push(item)
        }
    }
    return out
}
func main() {
    val threshold = 5
    val nums = [1, 3, 5, 7, 9]
    val big = filter(nums, func(n: Int) -> Bool { return n > threshold })
    println("{big.length()}")
}
// expected: "2\n"
```

### Parser Fix Pattern (iron_parse_type_annotation)

```c
// In iron_parse_type_annotation, array branch (after consuming '['):
if (iron_check(p, IRON_TOK_FUNC)) {
    /* func type as array element: [func(T) -> R] */
    Iron_TypeAnnotation *elem_ann = (Iron_TypeAnnotation *)iron_parse_type_annotation(p);
    ann->name = "func"; /* sentinel */
    ann->is_func_elem = true;
    ann->func_elem = (Iron_Node *)elem_ann;
} else if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
    /* error recovery: skip to ] */
    while (!iron_check(p, IRON_TOK_RBRACKET) && !iron_check(p, IRON_TOK_EOF))
        p->pos++;
    ann->name = "<error>";
} else {
    Iron_Token *name_tok = iron_advance(p);
    ann->name = iron_arena_strdup(p->arena, name_tok->value, strlen(name_tok->value));
}

// For standalone func type (non-array):
if (iron_check(p, IRON_TOK_FUNC)) {
    iron_advance(p); /* consume 'func' */
    ann->is_func = true;
    ann->name = "func";
    /* parse ( [TypeAnnotation, ...] ) */
    iron_expect(p, IRON_TOK_LPAREN);
    /* ... collect param type annotations ... */
    iron_expect(p, IRON_TOK_RPAREN);
    /* parse optional -> ReturnType */
    if (iron_match(p, IRON_TOK_ARROW)) {
        ann->func_return = iron_parse_type_annotation(p);
    }
    return (Iron_Node *)ann;
}
```

### DCE Fix Pattern (run_dce)

```c
// Step 1 seeding (around line 1229):
if (!iron_lir_instr_is_pure(in->kind) ||
    (in->kind == IRON_LIR_MAKE_CLOSURE &&
     in->make_closure.capture_count > 0) ||
    in->id == IRON_LIR_VALUE_INVALID) {

// Step 3 keep-or-remove (around line 1289):
if (!iron_lir_instr_is_pure(in->kind) ||
    (in->kind == IRON_LIR_MAKE_CLOSURE &&
     in->make_closure.capture_count > 0) ||
    in->id == IRON_LIR_VALUE_INVALID) {
    is_live = true;
```

### Iron_List_Iron_Closure Addition (iron_runtime.h)

```c
// Add adjacent to other Iron_List_* typedef structs (around line 560-564):
typedef struct Iron_List_Iron_Closure {
    Iron_Closure *items;
    int64_t       count;
    int64_t       capacity;
} Iron_List_Iron_Closure;

// Add adjacent to other IRON_LIST_DECL calls:
IRON_LIST_DECL(Iron_Closure, Iron_Closure)
```

```c
// In iron_collections.c, adjacent to other IRON_LIST_IMPL calls:
IRON_LIST_IMPL(Iron_Closure, Iron_Closure)
```

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Type annotations as name-only strings | Extended with `is_func`, func param/return fields | Phase 33 | `func(T) -> R` type annotations parse correctly |
| MAKE_CLOSURE always pure in DCE | Pure only when capture_count == 0 | Phase 33 | Capturing closures survive DCE |
| No Iron_List_Iron_Closure | Full macro-instantiated list for closures | Phase 33 | `[func() -> T]` arrays work at runtime |

**OPT-02 already complete (Phase 32):** Function inliner skips `__lambda_*` functions at `lir_optimize.c:3304`.

**OPT-03 partially complete (Phase 32):** Copy-prop Step 1b excludes captured allocas. Store-load-elim invalidates escaped allocas on CALL. Verify these still work with the new test cases.

---

## Open Questions

1. **If-as-expression scope**
   - What we know: Example 12 (`val action = if flag { ... } else { ... }`) requires if-as-expression support in the parser and HIR
   - What's unclear: Is implementing if-as-expression (parser + HIR PHI nodes + LIR) within Phase 33's scope, or should example 12 use a simplified rewrite?
   - Recommendation: Rewrite example 12 to avoid if-as-expression. The assignment form (`if flag { action = ... } else { action = ... }`) tests the same capture semantics without the parser/HIR change. Document this as a known simplification.

2. **capture_04 loop variable isolation**
   - What we know: The fix requires `[func() -> Int]` type annotation + `Iron_List_Iron_Closure` + DCE fix
   - What's unclear: Whether the `callbacks.push(MAKE_CLOSURE)` pattern correctly threads the closure into the list (the push result must use the MAKE_CLOSURE value)
   - Recommendation: After the parser and runtime fixes, trace the LIR for the push call to verify MAKE_CLOSURE id is the argument and DCE can't drop it

3. **iron_collections.c existence and pattern**
   - What we know: The IRON_LIST_IMPL macros generate function bodies; existing types use them somewhere
   - What's unclear: Exact file name and location of where IRON_LIST_IMPL calls live
   - Recommendation: `grep -rn "IRON_LIST_IMPL" src/` before writing the implementation file change

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Shell-based integration tests (`tests/run_tests.sh`) |
| Config file | None — convention: `.iron` + `.expected` file pairs in `tests/integration/` |
| Quick run command | `cd /path/to/iron-lang && tests/run_tests.sh integration build/iron 2>&1 \| grep -E "capture_0[1-4]\|capture_07\|capture_1[234]"` |
| Full suite command | `cd /path/to/iron-lang && tests/run_tests.sh integration build/iron` |

### Phase Requirements to Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CAPT-01 | Immutable `val` capture by value | integration | `tests/run_tests.sh integration` — filter `capture_01` | ✅ |
| CAPT-02 | Mutable `var` capture by reference | integration | `tests/run_tests.sh integration` — filter `capture_02` | ✅ |
| CAPT-03 | Multiple captures of different types | integration | `tests/run_tests.sh integration` — filter `capture_03` | ✅ |
| CAPT-04 | Loop variable snapshot capture | integration | `tests/run_tests.sh integration` — filter `capture_04` | ❌ Wave 0 |
| OPT-01 | DCE does not remove capturing MAKE_CLOSURE | integration (captured by capture_04) | Same as CAPT-04 | ❌ Wave 0 |
| OPT-02 | Inliner skips `__lambda_*` | integration (covered by existing tests) | `tests/run_tests.sh integration` | ✅ implied |
| OPT-03 | Copy-prop / store-load-elim respect heap-box | integration (captured by capture_02, capture_03) | `tests/run_tests.sh integration` | ✅ |

### Sampling Rate

- **Per task commit:** `tests/run_tests.sh integration build/iron 2>&1 | grep -E "PASS|FAIL" | grep -E "capture"` 
- **Per wave merge:** `tests/run_tests.sh integration build/iron`
- **Phase gate:** Full integration suite green before `/gsd:verify-work`

### Wave 0 Gaps

- [ ] `tests/integration/capture_04_loop_snapshot.iron` + `capture_04_loop_snapshot.expected` — CAPT-04, OPT-01
- [ ] `tests/integration/capture_07_callback_arg.iron` + `capture_07_callback_arg.expected` — HOF-02 preview
- [ ] `tests/integration/capture_12_capture_in_branch.iron` + `capture_12_capture_in_branch.expected` — conditional capture
- [ ] `tests/integration/capture_13_capture_in_match.iron` + `capture_13_capture_in_match.expected` — match in lambda
- [ ] `tests/integration/capture_14_filter_with_capture.iron` + `capture_14_filter_with_capture.expected` — func-type param

All 5 new pairs must be created before implementing fixes (TDD per locked decision).

---

## Sources

### Primary (HIGH confidence)

- Direct source reading: `src/parser/parser.c` lines 190-702 — complete trace of infinite loop
- Direct source reading: `src/parser/ast.h` line 204-213 — confirmed `Iron_TypeAnnotation` missing func fields
- Direct source reading: `src/lir/lir_optimize.c` lines 2823-2846 — confirmed MAKE_CLOSURE in pure list
- Direct source reading: `src/lir/lir_optimize.c` lines 1211-1310 — DCE algorithm
- Direct source reading: `src/runtime/iron_runtime.h` lines 292-579 — confirmed no `Iron_List_Iron_Closure`
- Direct source reading: `src/lir/emit_c.c` lines 232-253 — confirmed `Iron_List_Iron_Closure` name generation
- Direct source reading: `.planning/phases/32-capture-foundation/32-03-SUMMARY.md` — Phase 32 completion state

### Secondary (MEDIUM confidence)

- `docs/lambda-capture/04-capture-loop-variable.md` — target C output for example 4
- `docs/lambda-capture/07-callback-argument.md` — target C output for example 7
- `docs/lambda-capture/12-capture-in-branch.md` — target C output for example 12 (confirms if-as-expression needed)
- `docs/lambda-capture/13-capture-in-match.md` — target C output for example 13
- `docs/lambda-capture/14-filter-with-capture.md` — target C output for example 14 (confirms func-type param needed)

---

## Metadata

**Confidence breakdown:**
- Infinite loop root cause: HIGH — traced exact token-by-token execution path through parser source
- DCE purity bug: HIGH — confirmed exact line (2840) in lir_optimize.c
- Missing Iron_List_Iron_Closure: HIGH — confirmed by runtime header scan and emit_c.c type mapping
- If-as-expression scope: MEDIUM — confirmed parser does not support it; HIR/LIR changes needed are architectural
- Fix approach (parser): HIGH — Iron_TypeAnnotation extension is well-precedented; resolver change is localized

**Research date:** 2026-04-02
**Valid until:** 2026-05-02 (stable codebase; no external dependencies)
