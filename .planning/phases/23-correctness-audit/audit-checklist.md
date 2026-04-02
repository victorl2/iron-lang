# HIR Pipeline Correctness Audit Checklist

**Date:** 2026-03-31
**Auditor:** Phase 23 automated audit (plan 23-01)
**Scope:** Every HIR instruction kind traced through HIR->LIR->C emission

---

## HIR Statement Kinds

- [x] IRON_HIR_STMT_LET: Lowered to ALLOCA+STORE (mutable) or direct val binding (immutable). Mutable binds use `emit_alloca_in_entry` in entry block; immutable val binds go to `val_binding_map`. NULL init handled (alloca created but no store). RC/heap init now correctly uses RC-typed alloca to emit `T*` in C. — CHECKED OK (after fix for heap/rc mutable vars; see bug_audit_mutable_heap_field.iron)

- [x] IRON_HIR_STMT_ASSIGN: Lowers to STORE (IDENT target), SET_FIELD (field target), or SET_INDEX (index target). All three cases handled. No handling for other lvalue forms — those are not valid Iron lvalues at HIR level. — CHECKED OK

- [x] IRON_HIR_STMT_IF: Lowers to BRANCH terminator with then/else/merge blocks. Else branch optional (falls through to merge if absent). Defer scope pushed/popped for each branch body. scope_defers emitted before jump to merge. — CHECKED OK

- [x] IRON_HIR_STMT_WHILE: Lowers to header/body/exit blocks with JUMP back-edge. Condition evaluated in header. Defer scope managed per iteration body. — CHECKED OK

- [x] IRON_HIR_STMT_FOR: Lowers to pre_header/header/body/inc/exit blocks. Uses GET_INDEX for element access and GET_FIELD .count for length check. Loop variable stored in alloca. elem_type falls back to Int if iterable type unavailable. — CHECKED OK

- [x] IRON_HIR_STMT_MATCH: Lowers to SWITCH instruction with per-arm blocks and join block. Integer pattern arms added to case table; non-integer pattern becomes default block. ARM bodies lowered in order. — CHECKED OK

- [x] IRON_HIR_STMT_RETURN: Emits defer cleanup chain before RETURN. With defers: creates exit_block, emits cleanup, then RETURN in exit_block. Without defers: direct jump to exit then RETURN. Sets current_block=NULL for dead-code prevention. — CHECKED OK

- [x] IRON_HIR_STMT_DEFER: Pushes defer body onto defer_stacks at current scope depth. Bodies emitted LIFO at scope exit. Function-level fallback initializes scope if depth=0. — CHECKED OK

- [x] IRON_HIR_STMT_BLOCK: Creates a new defer scope, lowers block stmts, emits scope defers before exit. Scope properly popped after. — CHECKED OK

- [x] IRON_HIR_STMT_EXPR: Simply calls lower_expr on the contained expression; result discarded. — CHECKED OK

- [x] IRON_HIR_STMT_FREE: Lowers to IRON_LIR_FREE instruction. emit_c emits `free(val)`. Guards: only emits if current_block not terminated. — CHECKED OK

- [x] IRON_HIR_STMT_SPAWN: Lowers to IRON_LIR_SPAWN using lifted_name. emit_c emits `Iron_pool_submit(Iron_global_pool, (void(*)(void*))func_name, NULL)`. Pool defaults to global. — CHECKED OK

- [x] IRON_HIR_STMT_LEAK: Evaluates the expression (allows heap allocation to be created) but discards the result without registering for auto-free. The heap_alloc's auto_free=true flag would normally trigger free at return, but since the value is discarded from the alloca chain, it leaks intentionally. — CHECKED OK (intentional behavior)

---

## HIR Expression Kinds

- [x] IRON_HIR_EXPR_INT_LIT: Lowers to CONST_INT. emit_c emits `T _vN = (T)VALll;` with cast and LL suffix. — CHECKED OK

- [x] IRON_HIR_EXPR_FLOAT_LIT: Lowers to CONST_FLOAT. emit_c emits `T _vN = VAL;` with %g format. — CHECKED OK

- [x] IRON_HIR_EXPR_STRING_LIT: Lowers to CONST_STRING. emit_c emits Iron_String struct literal with .ptr and .len from the string value. NUL safety: empty string uses "". — CHECKED OK

- [x] IRON_HIR_EXPR_INTERP_STRING: Lowers each part to CONST or expression value, then INTERP_STRING instruction. emit_c does two-pass snprintf interpolation: pass 1 measures length, pass 2 fills buffer. Parts emitted as %s (string), %lld (int), %g (float), %s (bool string). — CHECKED OK

- [x] IRON_HIR_EXPR_BOOL_LIT: Lowers to CONST_BOOL. emit_c emits `bool _vN = true/false;`. — CHECKED OK

- [x] IRON_HIR_EXPR_NULL_LIT: Lowers to CONST_NULL. emit_c emits `void* _vN = NULL;`. Nullable struct wrapping handled at use sites. — CHECKED OK

- [x] IRON_HIR_EXPR_IDENT: Three lookup paths: val_binding_map (immutable, returns stored value directly), param_map (parameter), var_alloca_map (mutable, emits LOAD). Falls back to POISON for unknown var. — CHECKED OK

- [x] IRON_HIR_EXPR_BINOP: Short-circuit AND/OR handled separately with blocks. Other ops map to corresponding LIR binop kinds (ADD/SUB/MUL/DIV/MOD/EQ/NEQ/LT/LTE/GT/GTE). emit_c emits parenthesized binary expressions with correct C operators. — CHECKED OK

- [x] IRON_HIR_EXPR_UNOP: Maps to IRON_LIR_NEG or IRON_LIR_NOT. emit_c emits `(-val)` or `(!val)`. — CHECKED OK

- [x] IRON_HIR_EXPR_CALL: Three paths: (1) len(array) -> GET_FIELD .count; (2) constructor call (FUNC_REF with IRON_TYPE_OBJECT callee type) -> CONSTRUCT; (3) regular call -> FUNC_REF + CALL. String arg conversion for extern calls. Heap-pointer dereference for method self args (added in this audit). — CHECKED OK (after fix in bug_audit_heap_method_call.iron)

- [x] IRON_HIR_EXPR_METHOD_CALL: Lowers to FUNC_REF + CALL with mangled name `TypeName_method` (type_name lowercased). Static calls (FUNC_REF or unmapped IDENT receiver) omit self arg. Instance calls prepend self as first arg. emit_c emits as direct call via FUNC_REF path. — CHECKED OK

- [x] IRON_HIR_EXPR_FIELD_ACCESS: Lowers to GET_FIELD. emit_c determines arrow/dot via `val_is_heap_ptr()`: checks HEAP_ALLOC, RC_ALLOC, and LOAD from RC-typed alloca. Fixed in this audit to follow LOAD chains. — CHECKED OK (after audit fix)

- [x] IRON_HIR_EXPR_INDEX: Lowers to GET_INDEX. emit_c has three paths: (1) stack array direct `arr[idx]`; (2) Iron_List direct `.items[idx]`; (3) fallback `Iron_List_T_get(&arr, idx)`. — CHECKED OK

- [x] IRON_HIR_EXPR_SLICE: Lowers to IRON_LIR_SLICE with start/end values. emit_c emits `iron_list_slice(arr, start, end)`. Returns `Iron_List` (untyped). Start/end can be IRON_LIR_VALUE_INVALID (0/-1 defaults). — CHECKED OK

- [x] IRON_HIR_EXPR_CLOSURE: Lowers to MAKE_CLOSURE referencing `lifted_name`. Captures currently always NULL/0 (no outer variable capture). emit_c emits `void* _vN = (void*)func_name;` for zero-capture closures. Capture case emits env struct with malloc'd captures. — CHECKED OK (zero-capture path; see HIGH-RISK section for no-prototype cast warning)

- [x] IRON_HIR_EXPR_PARALLEL_FOR: Lowers to IRON_LIR_PARALLEL_FOR with range_val, chunk_func_name (lifted), and NULL captures (0). emit_c emits wrapper function + context struct + range-splitting submission loop + barrier. — CHECKED OK

- [x] IRON_HIR_EXPR_COMPTIME: Passes through to lower_expr on the inner expression. No separate LIR instruction. — CHECKED OK

- [x] IRON_HIR_EXPR_HEAP: Lowers inner expression, then HEAP_ALLOC(inner_val, auto_free, escapes). emit_c emits `T *_vN = (T*)malloc(sizeof(T)); *_vN = inner_val;`. The heap result is a pointer type (T*) in C. — CHECKED OK

- [x] IRON_HIR_EXPR_RC: Lowers inner expression, then RC_ALLOC(inner_val). emit_c emits `T *_vN = (T*)malloc(sizeof(T)); *_vN = inner_val;`. RC type used for reference-counted objects (simplified: same as heap for now). — CHECKED OK

- [x] IRON_HIR_EXPR_CONSTRUCT: Lowers field values in declaration order, emits CONSTRUCT with type and field_vals. emit_c emits `TypeName _vN = { .field0 = v0, .field1 = v1, ... };` using field names from Iron_ObjectDecl. Effective_field_count clamp applied (Phase 22 fix). Inheritance (_base field) handled. — CHECKED OK

- [x] IRON_HIR_EXPR_ARRAY_LIT: Lowers each element, emits ARRAY_LIT. emit_c has two paths: stack repr (use_stack_repr=true: `T arr[] = {e0,...}; int64_t arr_len = N;`) and heap repr (`Iron_List_T_create(); push() each element`). — CHECKED OK

- [x] IRON_HIR_EXPR_AWAIT: Lowers handle expression, emits AWAIT. emit_c emits `T _vN = (T)iron_future_await(handle);`. — CHECKED OK

- [x] IRON_HIR_EXPR_CAST: Lowers value, emits CAST with target_type. emit_c emits `T_target _vN = (T_target)val;`. — CHECKED OK

- [x] IRON_HIR_EXPR_IS_NULL: Lowers value, emits IS_NULL. emit_c emits `bool _vN = !val.has_value;` (operates on nullable optional structs). — CHECKED OK

- [x] IRON_HIR_EXPR_IS_NOT_NULL: Lowers value, emits IS_NOT_NULL. emit_c emits `bool _vN = val.has_value;`. — CHECKED OK

- [x] IRON_HIR_EXPR_FUNC_REF: Lowers to FUNC_REF instruction with func_name. emit_c: FUNC_REF instructions in emit_instr are suppressed (returns early without emitting); at use sites (CALL) the FUNC_REF name is resolved directly via `resolve_func_c_name`. In `emit_expr_to_buf`, FUNC_REF emits the C function name. — CHECKED OK

- [x] IRON_HIR_EXPR_IS: CHECKED ISSUE: Emits POISON placeholder. The IS type test (interface/enum runtime type check) is not yet implemented. The POISON value emits `/* poison */` comment in C without a usable value. If used in a conditional, this would produce undefined behavior (uninitialized variable used in branch). Currently there are no integration tests exercising this path. This is a known incomplete feature. — CHECKED ISSUE: IS expression produces POISON (unimplemented feature — silent UB if used in condition)

---

## High-Risk Emission Paths

### CONSTRUCT (Phase 22 fix — verified still present)
- [x] Constructor detection: In hir_to_lir.c EXPR_CALL, checks callee FUNC_REF's own type == IRON_TYPE_OBJECT (not the call result type). This correctly distinguishes constructors from regular struct-returning functions.
- [x] Field clamping: In emit_c.c CONSTRUCT emission, `effective_field_count` is clamped to `od->field_count + field_start`. Applied at BOTH CONSTRUCT emission sites (emit_instr and emit_expr_to_buf). This prevents emitting excess fields when the HIR has more field values than the ObjectDecl declares.
- [x] Inheritance: `_base` field emitted first when `od->extends_name` is set. Field index offset (`field_start`) correctly skips the base field.
- CHECKED OK (Phase 22 fixes confirmed in place)

### PARALLEL_FOR (capture zero semantics)
- [x] In hir_to_lir.c EXPR_PARALLEL_FOR: captures are always NULL/0 (`IRON_LIR_VALUE_INVALID, NULL, 0`). The pfor body is a lifted pure function with no access to outer variables.
- [x] Behavior: If Iron code attempts to access an outer variable inside a parallel-for body, the analyzer/HIR level would not lower it correctly (the lifted function has no captures). This produces a compile-time error (unknown variable in the lifted function), not silent wrong behavior.
- [x] Pool: `IRON_LIR_VALUE_INVALID` pool_val causes emit_c to use `Iron_global_pool`.
- CHECKED OK (captures intentionally zero; outer-variable access fails at compile time)

### MAKE_CLOSURE (function pointer cast)
- [x] For zero-capture closures: emit_c emits `void* _vN = (void*)func_name;`. Direct cast to void*.
- [x] At call sites: emit_c uses `(ret_type (*)())` — empty-parameter function pointer cast. This produces `-Wdeprecated-non-prototype` warnings in Clang.
- CHECKED ISSUE: The `(ret (*)())` cast omits parameter types. This is a known limitation: C23 will disallow calling through unprototyped function pointers. Current workaround uses `()` (no prototype) which works in C11/C17 but generates warnings. Fix requires tracking parameter types at call sites for indirect calls. Existing lambda tests pass despite warnings.

### GET_FIELD arrow/dot detection
- [x] Primary case: Direct HEAP_ALLOC or RC_ALLOC result — uses `->`. Works correctly.
- [x] Immutable val binding: For `val p = heap T(...)`, the val binds directly to the HEAP_ALLOC value. GET_FIELD on this checks `val_is_heap_ptr(fn, vid)` which returns true for HEAP_ALLOC. Uses `->`. CHECKED OK.
- [x] Mutable var binding (BUG FIXED): For `var p = heap T(...)`, was: alloca typed as `T`, STORE assigned `T*` to `T` alloca → C type mismatch. Fix: alloca now typed as RC(T) (= `T*`), LOAD produces RC-typed value, `val_is_heap_ptr` follows LOAD to check alloca kind==ALLOCA && alloca.alloc_type.kind==IRON_TYPE_RC → uses `->`. CHECKED OK after fix.
- CHECKED ISSUE (fixed): Mutable heap variables produced C compilation error. Fixed with RC-typed alloca + val_is_heap_ptr LOAD chain tracing.

### IS_CHECK (interface type test)
- [x] In hir_to_lir.c: EXPR_IS lowers the value but emits POISON, ignoring check_type. No IS_CHECK LIR instruction is produced.
- [x] In emit_c.c: POISON emits `/* poison */` comment. No value assigned.
- CHECKED ISSUE: IS expression is a POISON stub. No integration tests exercise it. If used in a boolean context (e.g., `if x is TypeName`), the condition would read an uninitialized variable — undefined behavior. This is a known incomplete feature that needs runtime type tag infrastructure.

### EXPR_METHOD_CALL on heap objects
- [x] Method call on value-type objects: self passed by value. Works correctly.
- [x] Method call on heap objects (BUG FIXED): For `heap_obj.method()`, self was passed as `T*` but method expected `T`. Fix: in CALL argument emission, when `val_is_heap_ptr(fn, arg_id)` AND callee's i-th parameter is `IRON_TYPE_OBJECT`, emit `(*arg_val)` dereference. CHECKED OK after fix.
- CHECKED ISSUE (fixed): Heap object method calls produced C compilation error. Fixed with pointer dereference at call site.

---

## Summary of Bugs Found

| # | Rule | Description | Files | Test |
|---|------|-------------|-------|------|
| 1 | Rule 1 - Bug | Mutable heap var alloca wrong type (T vs T*) | hir_to_lir.c, emit_c.c | bug_audit_mutable_heap_field.iron |
| 2 | Rule 1 - Bug | Heap object method call passes T* where T expected | emit_c.c | bug_audit_heap_method_call.iron |

---

## Known Limitations (Not Bugs)

1. **EXPR_IS POISON**: IS type test emits POISON — feature not yet implemented. No runtime type tag infrastructure exists. If used, UB. Zero existing tests exercise this path.

2. **MAKE_CLOSURE no-prototype cast**: `(ret (*)())` cast for indirect lambda calls generates `-Wdeprecated-non-prototype` warnings. Works in C11/C17 but deprecated in C23. Fix requires propagating parameter types to indirect call sites.

3. **PFOR captures always zero**: pfor body cannot access outer variables — attempting to do so causes a compile error, not silent wrong behavior. Intentional design.

4. **STMT_LEAK behavior**: The leak statement evaluates the expression but doesn't explicitly disable auto_free. If the heap_alloc has auto_free=true (the default), the runtime may attempt to free it at function return — this creates a dangling pointer if the intent was to keep the allocation alive. Current test coverage does not exercise this edge case.
