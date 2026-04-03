# Phase 37: Compiler Dispatch Fixes + Technical Debt - Research

**Researched:** 2026-04-02
**Domain:** Iron compiler internals — method dispatch, build system hygiene, runtime init
**Confidence:** HIGH — all findings from direct source inspection with line-number citations

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Method dispatch model**
- Use method-decl lookup (not hardcoded table) — create `.iron` wrapper declarations for String/List/Map/Set methods, use existing `IRON_NODE_METHOD_DECL` resolution path. More uniform with how math/io/time/log work.
- C function naming: `Iron_String_upper` convention (Type_Method) — matches existing `Iron_List_int64_t_push` pattern (note: lowercase transformation in hir_to_lir.c means C output is `Iron_string_upper`; see Architecture section for exact flow)
- Collection method mangling: reuse the `mangle_generic` pattern from `gen_types.c` — `Iron_List_int64_t_filter`, `Iron_Map_Iron_String_int64_t_keys`, etc.
- Both `typecheck.c` (return type resolution at line 690) and `hir_to_lir.c` (type name extraction at line 776-780) need `IRON_TYPE_STRING` and `IRON_TYPE_ARRAY` branches added

**Import detection**
- Replace `strstr()` with token-level detection — run the lexer first, look for IMPORT tokens. More correct than line-anchored matching.
- Extract into a shared helper function that both `build.c` and `check.c` call — currently both files have duplicate strstr() blocks (build.c:641-732, check.c:161-245)

**Windows portability**
- **Document only, do not implement** — the compiler doesn't currently support Windows
- Document all known gaps: iron_io.c (dirent.h, mkdir), iron_log.c (localtime_r, isatty), iron_math.c (__thread)
- When Windows is eventually supported, use FindFirstFile/FindNextFile for directory listing (not a dirent.h shim)
- COMP-05 and COMP-06 become documentation tasks, not implementation tasks

**argv/args API**
- Store argc/argv in globals during `iron_runtime_init(argc, argv)` — simple global access, os.args() reads the globals
- Change `iron_runtime_init()` signature to accept `(int argc, char** argv)` — breaking change is fine since this is an internal API
- `emit_c.c:3442` already generates `main(int argc, char** argv)` — just need to pass them through to the init call

### Claude's Discretion
- Exact implementation of the token-level import detection helper
- How to handle edge cases in method dispatch (null receivers, error types)
- Static assert mechanism for argv_buf size validation

### Deferred Ideas (OUT OF SCOPE)

None — discussion stayed within phase scope
</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| COMP-01 | Compiler correctly dispatches method calls on `IRON_TYPE_STRING` receivers (typecheck.c + hir_to_lir.c) | typecheck.c:642-653 branch gap confirmed; hir_to_lir.c:776-782 `type_name="Unknown"` fallback confirmed |
| COMP-02 | Compiler correctly dispatches method calls on `IRON_TYPE_ARRAY` receivers for collection types (typecheck.c + hir_to_lir.c) | Same gaps; additional element-type mangling complexity documented in Architecture section |
| COMP-03 | `build.c` argv_buf bumped to 128 with static assert to prevent overflow when adding new modules | build.c:513 `const char *argv_buf[96]` confirmed; current slot count ~23 non-raylib entries |
| COMP-04 | `build.c` import detection replaced with token-level helper (no false positives for `import os` in comments/strings) | build.c:641-732 + check.c:161-245 strstr blocks confirmed; IRON_TOK_IMPORT exists in lexer.h:36 |
| COMP-05 | Windows portability in existing `iron_io.c` (dirent.h, mkdir) and `iron_log.c` (localtime_r, isatty) — documentation task per CONTEXT.md | Gaps confirmed; decision is document-only |
| COMP-06 | `iron_math.c` `__thread` replaced with portable `IRON_THREAD_LOCAL` macro — documentation task per CONTEXT.md | `__thread` at iron_math.c:61,71 confirmed; decision is document-only |
| COMP-07 | `emit_c.c` threads `argc`/`argv` through `iron_runtime_init_with_args()` for `os.args()` support | emit_c.c:3438-3442 + iron_string.c:202 `iron_runtime_init(void)` confirmed |
</phase_requirements>

---

## Summary

Phase 37 is a pure compiler/build-system fix phase — no new language features, no new runtime functions. It clears five pre-existing defects that would silently corrupt or block all subsequent standard library work.

The most critical fix is the method dispatch gap: `hir_to_lir.c`'s `IRON_HIR_EXPR_METHOD_CALL` case sets `type_name = "Unknown"` for any receiver that is not `IRON_TYPE_OBJECT`. Since `String` is a primitive (`IRON_TYPE_STRING`) and `List[T]` is `IRON_TYPE_ARRAY`, every string or collection method call silently generates a clang link error (`Iron_Unknown_upper: symbol not found`). The same gap exists in `typecheck.c` where the method-decl lookup only executes when the receiver is an `IRON_TYPE_OBJECT` instance. The fix requires adding two new receiver-type branches in each file, plus creating `.iron` wrapper files (e.g. `string.iron`) that inject `IRON_NODE_METHOD_DECL` nodes so the existing decl-scan path finds them.

The secondary fixes — argv_buf ceiling (COMP-03), import substring detection (COMP-04), argc/argv threading (COMP-07) — are straightforward but must land before any phase adds new stdlib modules. COMP-05 and COMP-06 are documentation-only per the locked decision.

**Primary recommendation:** Fix COMP-01 and COMP-02 first in a single atomic commit (they are in exactly two files each and depend on each other for testing). Write one integration test per receiver type immediately after to confirm the dispatch chain is closed end-to-end before adding any runtime function stubs.

---

## Architecture Patterns

### Method Dispatch Pipeline (Current + Required State)

```
Iron source: name.upper()   (name: String)
      │
      ▼
  [Parser]  → IRON_NODE_METHOD_CALL { object: Ident("name"), method: "upper" }
      │         (no change needed)
      ▼
  [Typechecker]  typecheck.c:634-671
      │  CURRENT: mc->object is IRON_NODE_IDENT, obj_id->resolved_type->kind == IRON_TYPE_STRING
      │           → type_name_mc stays NULL → decl scan skipped → result = IRON_TYPE_VOID (wrong)
      │
      │  REQUIRED (method-decl path):
      │    Add third branch at typecheck.c ~line 650:
      │      else if (obj_id->resolved_type->kind == IRON_TYPE_STRING) {
      │          type_name_mc = "String";   // matches md->type_name in string.iron wrapper
      │      }
      │    → decl scan finds "func String.upper() -> String {}" from string.iron
      │    → result = IRON_TYPE_STRING  (correct)
      ▼
  [HIR Lowering]  hir_lower.c  — no change needed
      ▼
  [HIR → LIR]  hir_to_lir.c:774-850
      │  CURRENT: obj_type->kind == IRON_TYPE_STRING → falls through to type_name = "Unknown"
      │           → mangled name = "Unknown_upper" → C call Iron_Unknown_upper  (link error)
      │
      │  REQUIRED:
      │    Add branch at hir_to_lir.c ~line 779:
      │      if (obj_type->kind == IRON_TYPE_STRING) {
      │          type_name = "string";  // already lowercase; loop won't change it
      │      }
      │    → snprintf: "string_upper"
      │    → lowercasing loop: no-op (already lowercase)
      │    → FUNC_REF stores "string_upper"
      │    → mangle_func_name("string_upper") → "Iron_string_upper"  (correct)
      ▼
  [LIR → C]  emit_c.c  — no change needed for COMP-01
      │  Emits: Iron_string_upper(self)
      ▼
  [build.c / clang]  — no change needed for COMP-01
      │  iron_string.c already in argv_buf via rt_string_out
      ▼
  Native binary calls Iron_string_upper()  — must exist in iron_string.c
```

### Collection Dispatch — Extra Complexity (COMP-02)

Collection types carry an element type. `[Int]` must dispatch to `Iron_List_int64_t_filter`, not `Iron_list_int64_t_filter` (capital L). The existing lowercasing loop in hir_to_lir.c lowercases the prefix before the first `_`, which would produce `Iron_list_int64_t_filter` — a mismatch with the runtime's `Iron_List_int64_t_filter`.

**Critical implementation note for hir_to_lir.c COMP-02:** Do NOT use the generic lowercasing path for collection receivers. Instead, build the full mangled C name directly (e.g. `"Iron_List_int64_t_filter"`) and store it in the FUNC_REF. `mangle_func_name()` in emit_c.c already has a guard: `if (strncmp(name, "Iron_", 5) == 0) return name;` — so passing the fully-qualified C name bypasses re-mangling.

Element-type suffix mapping (from `emit_type_to_c` in emit_c.c, confirmed via iron_collections.c comment):

| Iron type kind | C suffix |
|----------------|----------|
| `IRON_TYPE_INT` | `int64_t` |
| `IRON_TYPE_INT32` | `int32_t` |
| `IRON_TYPE_FLOAT` | `double` |
| `IRON_TYPE_BOOL` | `bool` |
| `IRON_TYPE_STRING` | `Iron_String` |
| `IRON_TYPE_OBJECT` (named) | `Iron_<decl->name>` |

**Collection method dispatch flow for `items.filter(fn)` where `items: List[Int]`:**

```
hir_to_lir.c COMP-02 patch:
  obj_type->kind == IRON_TYPE_ARRAY
  elem_type = obj_type->array.elem
  Build: "Iron_List_int64_t_filter"  (using elem suffix table above)
  Store directly in FUNC_REF (already has Iron_ prefix → skip lowercasing)
  → emit_c.c emits: Iron_List_int64_t_filter(self, closure)
```

**typecheck.c COMP-02 patch:** Add IRON_TYPE_ARRAY branch in method-decl lookup. The type_name_mc must match the `md->type_name` in the collection wrapper declaration. For `items.filter(fn)` where `items: List[Int]`, `type_name_mc = "List"` is sufficient if the wrapper declares `func List.filter(fn: Func) -> List {}`. However, since the return type depends on the element type (filter returns same-element-type list), the return type resolution may need element-type awareness.

**Option (simpler and correct):** For `IRON_TYPE_ARRAY` receivers, resolve the return type heuristically in typecheck.c based on method name — no decl scan needed. Map `filter/map/slice/unique` → same array type; `sort/reverse/for_each` → void; `any/all` → bool; `find` → nullable element type. This avoids needing element-type-aware wrapper declarations.

### Import Detection Architecture (COMP-04)

**Current state in both build.c and check.c:**
```c
if (strstr(source, "import raylib") != NULL) { ... }
if (strstr(source, "import math") != NULL)   { ... }
if (strstr(source, "import io") != NULL)     { ... }
if (strstr(source, "import time") != NULL)   { ... }
if (strstr(source, "import log") != NULL)    { ... }
```

**Decision (CONTEXT.md):** Replace with token-level detection using the existing lexer.

**Required approach — shared token-level helper:**

The lexer API is clean and standalone:
```c
// lexer.h
Iron_Lexer iron_lexer_create(const char *src, const char *filename,
                              Iron_Arena *arena, Iron_DiagList *diags);
Iron_Token *iron_lex_all(Iron_Lexer *l);
// IRON_TOK_IMPORT exists at lexer.h:36
// IRON_TOK_IDENTIFIER follows for the module name
```

The shared helper scans the token stream for `IRON_TOK_IMPORT` followed immediately by `IRON_TOK_IDENTIFIER` with the target name. This correctly ignores `import` inside string literals and comments (the lexer never produces those as IRON_TOK_IMPORT tokens).

**Implementation in a shared file (e.g., `src/cli/iron_import_detect.h/.c`):**
```c
// Returns true if source contains "import <module_name>" as a real import statement
// Scans the lex token stream, not the raw text — immune to comments and string literals
bool iron_detect_import(const char *source, const char *filename,
                        const char *module_name, Iron_Arena *arena);
```

Both `build.c` and `check.c` call this helper. The helper creates a throwaway arena and lexer, scans for the pattern, and returns. The arena is freed after the call (or the caller's arena can be reused).

**Note:** The lexer requires an `Iron_Arena` and `Iron_DiagList`. For import detection at CLI entry, a small stack-allocated arena (e.g. 16KB) is sufficient — module names are short and token arrays are compact. Lex errors from malformed source can be safely ignored for this purpose (detection falls back to "not found").

### argv_buf Fix (COMP-03)

Current state in `build.c:513`:
```c
const char *argv_buf[96];
int ai = 0;
```

Current slot count on non-Windows (confirmed by code audit): ~23 entries + NULL = 24 used. Raylib on macOS adds ~8 more = ~32. Ceiling of 96 has headroom now, but new stdlib modules added in phases 38-42 each consume one slot.

**Fix:**
```c
const char *argv_buf[128];
_Static_assert(sizeof(argv_buf)/sizeof(argv_buf[0]) >= 128,
               "argv_buf too small — bump if adding new stdlib modules");
```

The `_Static_assert` fires at **compile time of the Iron compiler itself** if the ceiling constant is ever reduced. This is the right gate — it protects against future edits that reduce the size.

Also add a runtime bounds check before each `argv_buf[ai++]` increment, or add a `_Static_assert` that the maximum possible `ai` (number of fixed entries + maximum module additions) stays under 128.

### iron_runtime_init Signature Change (COMP-07)

**Current state:**
- `iron_string.c:202`: `void iron_runtime_init(void)`
- `iron_runtime.h:266`: `void iron_runtime_init(void);`
- `emit_c.c:3438-3442`: Emits `(void)argc; (void)argv;` then `iron_runtime_init();`

**Required state:**
- `iron_runtime.h`: `void iron_runtime_init(int argc, char **argv);`
- `iron_string.c`: Accept and store args in file-scope globals
- `emit_c.c`: Change emitted `iron_runtime_init()` → `iron_runtime_init(argc, argv)`

**Global storage pattern** (in iron_string.c or a dedicated iron_runtime_globals.c):
```c
static int    s_iron_argc = 0;
static char **s_iron_argv = NULL;

void iron_runtime_init(int argc, char **argv) {
    s_iron_argc = argc;
    s_iron_argv = argv;
    // ... existing intern table init ...
    iron__ensure_intern_lock();
    // ...
}
```

A future `iron_os_args()` function reads `s_iron_argc`/`s_iron_argv` directly.

**Breakage scope:** `iron_runtime_init` is only called from generated `main()` wrappers (emit_c.c) and from unit test `setUp()` functions (`tests/unit/test_runtime_string.c:10`). The unit test setUp calls `iron_runtime_init()` with no args — this will break and must be updated to `iron_runtime_init(0, NULL)`.

---

## Standard Stack

No new external dependencies. All work is within the existing C11 Iron codebase.

| Component | File | Purpose | Change Type |
|-----------|------|---------|-------------|
| Method dispatch (typecheck) | `src/analyzer/typecheck.c` | Add IRON_TYPE_STRING + IRON_TYPE_ARRAY branches in method-decl lookup | Add ~20 lines |
| Method dispatch (LIR) | `src/hir/hir_to_lir.c` | Set correct type_name for String; build full mangled name for collections | Add ~30 lines |
| String wrapper | `src/stdlib/string.iron` | New file: declares `func String.upper() -> String {}` etc. for decl-scan path | New file |
| Collection wrapper | `src/stdlib/list.iron` | New file: declares `func List.filter(fn: Func) -> List {}` etc. | New file |
| argv_buf | `src/cli/build.c` | Bump 96→128, add _Static_assert | Change ~3 lines |
| Import detection | `src/cli/build.c` + `check.c` | Replace strstr with token-level helper | Refactor ~130 lines total |
| Import detection helper | New shared file | `iron_detect_import()` using iron_lex_all | New ~50-line helper |
| Runtime init signature | `src/runtime/iron_string.c` + `iron_runtime.h` | Accept (int argc, char** argv), store globals | Change ~10 lines |
| Emit main wrapper | `src/lir/emit_c.c` | Pass argc/argv to iron_runtime_init | Change ~2 lines |
| Unit test setUp | `tests/unit/test_runtime_string.c` (and other test files) | Update iron_runtime_init() calls to (0, NULL) | Change ~5 lines |

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Import token detection | Custom regex or line-anchor parser | `iron_lex_all()` from lexer.h | Lexer already handles comments, string literals, escape sequences correctly |
| Collection type name mangling | New naming convention | Same suffix table as `emit_type_to_c()` in emit_c.c | Must match exactly or generated calls won't link |
| Static assert | Runtime overflow check on argv_buf | `_Static_assert` (C11, already used elsewhere) | Fires at compiler compile time, not at user program runtime |

---

## Common Pitfalls

### Pitfall 1: Method-Decl Lookup for String Requires a New Branch — Not Just a New .iron File

**What goes wrong:** Adding `string.iron` with `func String.upper() -> String {}` does nothing unless `typecheck.c` is also patched. The decl scan at typecheck.c:654-667 only executes when `type_name_mc != NULL`. For a `String` variable receiver, `type_name_mc` stays NULL because neither the `IRON_SYM_TYPE` branch (line 645-648) nor the `IRON_TYPE_OBJECT` branch (line 649-652) fires.

**How to avoid:** The patch adds a third branch before line 654: when `obj_id->resolved_type->kind == IRON_TYPE_STRING`, set `type_name_mc = "String"`. Then the existing decl scan finds the matching `IRON_NODE_METHOD_DECL` from `string.iron`.

**Warning signs:** `name.upper()` type-checks to `void` (no error, wrong type). The call compiles but returns garbage or crashes.

### Pitfall 2: hir_to_lir.c Lowercasing Loop Breaks Collection Names

**What goes wrong:** For `IRON_TYPE_ARRAY`, if `type_name` is set to `"List_int64_t"` and the generic lowercasing loop runs, it produces `"list_int64_t_filter"`. The emitter then produces `Iron_list_int64_t_filter` which does not exist — the runtime declares `Iron_List_int64_t_filter` (capital L after Iron_).

**How to avoid:** For collection receivers, build the full C name including the `Iron_` prefix (e.g. `"Iron_List_int64_t_filter"`) and store it directly in the FUNC_REF. `mangle_func_name()` skips names already starting with `"Iron_"`. Do not pass the name through the lowercasing loop.

**Warning signs:** Clang link error `Iron_list_int64_t_*: symbol not found` (lowercase l).

### Pitfall 3: iron_runtime_init Signature Change Breaks Unit Test setUp

**What goes wrong:** `tests/unit/test_runtime_string.c:10` and other unit test files call `iron_runtime_init()` in their `setUp()` function. After the signature changes to `iron_runtime_init(int argc, char** argv)`, these calls become compile errors.

**How to avoid:** Update all `iron_runtime_init()` call sites to `iron_runtime_init(0, NULL)` as part of COMP-07. Run the full unit test suite as the final verification step.

**Warning signs:** Unit test compilation fails after the signature change.

### Pitfall 4: Token-Level Import Detection Requires an Arena

**What goes wrong:** `iron_lex_all()` requires an `Iron_Arena *` and `Iron_DiagList *`. In `build.c`, the main source arena is created after import detection (line 753 in build.c flow). If the helper uses the main arena, it must be allocated earlier. If it uses a throwaway arena, that arena must be freed.

**How to avoid:** The detection helper creates a small local arena (`iron_arena_create(16 * 1024)`) and frees it before returning. This keeps the helper self-contained and does not affect the caller's arena lifetime.

**Warning signs:** Memory leak in the detection helper; or "use after free" if the arena is freed while the token array is still in scope.

### Pitfall 5: Method-Decl for IRON_TYPE_ARRAY — Return Type Resolution

**What goes wrong:** The decl-scan approach for `IRON_TYPE_ARRAY` would need `func List.filter(fn: Func) -> List {}` in `list.iron`. But the return type `List` is ambiguous — it carries no element type. The typechecker would resolve the return to a bare `IRON_TYPE_ARRAY` with no `elem` field set, which later passes incorrect type info downstream.

**How to avoid:** For `IRON_TYPE_ARRAY` receiver methods in typecheck.c, use a **direct return-type table** instead of decl scan. This is the one case where a small hardcoded table is cleaner than the decl-scan path. Specifically:

```c
if (obj_type->kind == IRON_TYPE_ARRAY) {
    static const struct { const char *name; int ret_kind; bool same_array; } coll_methods[] = {
        { "filter",   -1, true  },   // same array type
        { "map",      -1, true  },   // same array type (same-element only)
        { "sort",     IRON_TYPE_VOID, false },
        { "reverse",  IRON_TYPE_VOID, false },
        { "for_each", IRON_TYPE_VOID, false },
        { "any",      IRON_TYPE_BOOL, false },
        { "all",      IRON_TYPE_BOOL, false },
        { "find",     -1, false },  // nullable element type
    };
    // scan and set result
}
```

This is within Claude's Discretion per CONTEXT.md ("How to handle edge cases in method dispatch").

---

## Code Examples

Verified patterns from direct source inspection:

### COMP-01: typecheck.c IRON_TYPE_STRING Branch (method-decl path)

```c
// src/analyzer/typecheck.c — inside case IRON_NODE_METHOD_CALL
// Insert after line 652 (IRON_TYPE_OBJECT branch), before line 654 (if type_name_mc):
} else if (obj_id->resolved_type &&
           obj_id->resolved_type->kind == IRON_TYPE_STRING) {
    /* String instance method: receiver is a String variable.
     * type_name_mc = "String" matches IRON_NODE_METHOD_DECL nodes
     * injected from string.iron wrapper. */
    type_name_mc = "String";
}
```

### COMP-01: hir_to_lir.c IRON_TYPE_STRING Branch

```c
// src/hir/hir_to_lir.c — inside case IRON_HIR_EXPR_METHOD_CALL
// Replace line 780-782 block:
if (obj_type->kind == IRON_TYPE_OBJECT && obj_type->object.decl) {
    type_name = obj_type->object.decl->name;
} else if (obj_type->kind == IRON_TYPE_STRING) {
    type_name = "string";  // lowercase: snprintf produces "string_upper"
                           // mangle_func_name prepends Iron_ → Iron_string_upper
}
// Note: IRON_TYPE_ARRAY handled separately below (full name bypass)
```

### COMP-02: hir_to_lir.c Collection Name Construction (bypass lowercasing)

```c
// src/hir/hir_to_lir.c — inside case IRON_HIR_EXPR_METHOD_CALL, after is_static_call detection
if (expr->method_call.object && expr->method_call.object->type &&
    expr->method_call.object->type->kind == IRON_TYPE_ARRAY) {
    Iron_Type *elem = expr->method_call.object->type->array.elem;
    const char *elem_suffix = iron_elem_type_to_c_suffix(elem); // helper using same table as emit_type_to_c
    // Build: "Iron_List_<suffix>_<method>"
    size_t mlen = 10 + strlen(elem_suffix) + 1 + strlen(expr->method_call.method) + 1;
    char *mangled = (char *)iron_arena_alloc(ctx->lir_arena, mlen, 1);
    snprintf(mangled, mlen, "Iron_List_%s_%s", elem_suffix, expr->method_call.method);
    // Store directly in FUNC_REF — mangle_func_name sees "Iron_" prefix and returns as-is
    IronLIR_Instr *fref = iron_lir_func_ref(ctx->current_func, ctx->current_block,
                                              mangled, NULL, span);
    // ... emit call as normal ...
}
```

### COMP-03: argv_buf Bump with Static Assert

```c
// src/cli/build.c:513 — inside invoke_clang()
const char *argv_buf[128];
_Static_assert(sizeof(argv_buf)/sizeof(argv_buf[0]) >= 128,
               "argv_buf ceiling too small — increase when adding stdlib modules");
int ai = 0;
```

### COMP-04: Token-Level Import Detection Helper

```c
// New file: src/cli/iron_import_detect.c (or inline in build.c/check.c)
// Uses: lexer.h (iron_lexer_create, iron_lex_all), arena.h, stb_ds.h (arrfree)
bool iron_detect_import(const char *source, const char *module_name) {
    Iron_Arena arena = iron_arena_create(16 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Lexer lexer = iron_lexer_create(source, "<import-detect>", &arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lexer);
    bool found = false;
    ptrdiff_t n = arrlen(tokens);
    for (ptrdiff_t i = 0; i + 1 < n; i++) {
        if (tokens[i].kind == IRON_TOK_IMPORT &&
            tokens[i+1].kind == IRON_TOK_IDENTIFIER &&
            tokens[i+1].value &&
            strcmp(tokens[i+1].value, module_name) == 0) {
            found = true;
            break;
        }
    }
    arrfree(tokens);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    return found;
}
```

Replaces each `strstr(source, "import X")` call with `iron_detect_import(source, "X")`.

### COMP-07: iron_runtime_init Signature Update

```c
// src/runtime/iron_runtime.h — change line 266:
void iron_runtime_init(int argc, char **argv);

// src/runtime/iron_string.c — change line 202:
static int    s_iron_argc = 0;
static char **s_iron_argv = NULL;

void iron_runtime_init(int argc, char **argv) {
    s_iron_argc = argc;
    s_iron_argv = argv;
    iron__ensure_intern_lock();
    IRON_MUTEX_LOCK(s_intern_lock);
    if (s_intern_table == NULL) {
        // ... existing init body continues unchanged ...
    }
    // ...
}

// src/lir/emit_c.c — change line 3440-3442:
iron_strbuf_appendf(&ctx.main_wrapper,
                     "    iron_runtime_init(argc, argv);\n");  // was iron_runtime_init()
// Remove the (void)argc; (void)argv; lines

// tests/unit/test_runtime_string.c (and any other unit test setUp):
void setUp(void) { iron_runtime_init(0, NULL); }
```

### string.iron Wrapper Pattern (for COMP-01 method-decl lookup)

```iron
-- string.iron — Iron wrapper for String built-in methods
-- Empty bodies signal "extern C implementation" — same pattern as math.iron, io.iron

func String.upper() -> String {}
func String.lower() -> String {}
func String.trim() -> String {}
func String.len() -> Int {}
-- ... (full list added in Phase 38; Phase 37 only needs the dispatch infrastructure)
```

This file is prepended when `import` is detected — but String methods need no explicit import. Alternative: always prepend `string.iron` unconditionally in the build pipeline (since String is always available). This is the cleanest approach and avoids adding a new import keyword for built-ins.

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Unity v2.6.1 (via FetchContent in CMakeLists.txt) |
| Config file | CMakeLists.txt (root), uses `add_test()` |
| Quick run command | `cd /Users/victor/code/iron-lang && ctest --test-dir build -R "runtime_string\|typecheck" -V` |
| Full suite command | `cd /Users/victor/code/iron-lang && ctest --test-dir build -V` |
| Integration tests | `cd /Users/victor/code/iron-lang/tests && ./run_tests.sh integration` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| COMP-01 | `name.upper()` on String routes to `Iron_string_upper()` | integration | `./run_tests.sh integration` (dispatch_string_method.iron) | ❌ Wave 0 |
| COMP-01 | typecheck resolves String method return type correctly | unit | `ctest --test-dir build -R test_typecheck -V` | ✅ (extend) |
| COMP-02 | `items.filter(fn)` on List[Int] routes to `Iron_List_int64_t_filter()` | integration | `./run_tests.sh integration` (dispatch_list_method.iron) | ❌ Wave 0 |
| COMP-03 | argv_buf[128] compiles without static-assert failure | unit/compile | `cmake --build build` (fires at compile time) | N/A |
| COMP-04 | `-- import os` comment does not trigger os.iron prepend | integration | `./run_tests.sh integration` (import_detect_no_false_positive.iron) | ❌ Wave 0 |
| COMP-04 | `import os` on first line triggers os.iron prepend | integration | `./run_tests.sh integration` (import_detect_positive.iron) | ❌ Wave 0 |
| COMP-05 | Windows gaps documented in iron_io.c and iron_log.c | manual | Review comments in source | N/A — doc only |
| COMP-06 | IRON_THREAD_LOCAL macro documented in iron_math.c | manual | Review comments in source | N/A — doc only |
| COMP-07 | Generated main() calls iron_runtime_init(argc, argv) | integration | `./run_tests.sh integration` (argv_threads_through.iron) | ❌ Wave 0 |
| COMP-07 | Unit test setUp compiles with new signature | unit | `ctest --test-dir build -R test_runtime_string -V` | ✅ (update setUp) |

### Sampling Rate
- **Per task commit:** `ctest --test-dir build -R "runtime_string\|typecheck" -V`
- **Per wave merge:** `ctest --test-dir build -V && cd tests && ./run_tests.sh integration`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/integration/dispatch_string_method.iron` + `.expected` — covers COMP-01 (smoke: `name.upper()` compiles and produces correct output)
- [ ] `tests/integration/dispatch_list_method.iron` + `.expected` — covers COMP-02 (smoke: `items.filter(fn)` routes correctly; requires a minimal filter stub in iron_collections.c)
- [ ] `tests/integration/import_detect_no_false_positive.iron` + `.expected` — covers COMP-04 (a comment containing "import os" does not break compilation)
- [ ] `tests/integration/import_detect_positive.iron` + `.expected` — covers COMP-04 (real import statement works)
- [ ] `tests/integration/argv_threads_through.iron` + `.expected` — covers COMP-07 (os.args() returns a non-null list; requires os module stub)

**Note:** COMP-02 Wave 0 test needs a minimal `Iron_List_int64_t_filter` stub to exist in the runtime before the integration test can link. Add a no-op stub in `iron_collections.c` as part of the COMP-02 task; the real implementation ships in Phase 40.

---

## State of the Art

| Old Approach | Current Approach (after this phase) | Impact |
|--------------|-------------------------------------|--------|
| `strstr()` substring import detection | Token-level `iron_detect_import()` using lexer | No false positives in comments/strings |
| `argv_buf[96]` hard ceiling | `argv_buf[128]` + `_Static_assert` | Compile-time safety for future module additions |
| `iron_runtime_init(void)` | `iron_runtime_init(int argc, char **argv)` | Enables `os.args()` in Phase 41 |
| Method calls on primitives route to `Iron_Unknown_*` (link error) | String/Array receivers dispatch correctly | All Phase 38-40 runtime work is now testable |

---

## Open Questions

1. **string.iron Always-Prepend vs Import-Triggered**
   - What we know: Math/IO/Time/Log require explicit `import` to get their `.iron` stubs. String methods work without any import (String is always in scope).
   - What's unclear: Should `string.iron` be prepended unconditionally to every compilation, or triggered by detecting a string method call (which the lexer can't easily detect pre-parse)?
   - Recommendation: Prepend `string.iron` unconditionally in `build_src_list()`. The file is small (~30 lines for all 19 methods). This matches how `string.iron` semantically works — `String` is a built-in type, not a module.

2. **list.iron Return-Type Ambiguity for COMP-02**
   - What we know: A `list.iron` wrapper declaring `func List.filter(fn: Func) -> List {}` cannot express the element type in the return type `List`.
   - What's unclear: Whether the decl-scan path will work if the wrapper returns a bare `List` type (no element).
   - Recommendation: Use the hardcoded return-type table in typecheck.c for `IRON_TYPE_ARRAY` methods (within Claude's Discretion). No `list.iron` wrapper needed for COMP-02; only hir_to_lir.c needs the mangling fix.

3. **Additional iron_runtime_init Call Sites**
   - What we know: `test_runtime_string.c:10` calls `iron_runtime_init()` in setUp. Likely other unit test files do too.
   - What's unclear: Full list of call sites beyond iron_string.c and emit_c.c.
   - Recommendation: Before patching, run `grep -rn "iron_runtime_init" src/ tests/` to find all call sites. Update each to `(0, NULL)` where applicable.

---

## Sources

### Primary (HIGH confidence — direct source inspection)
- `src/analyzer/typecheck.c` lines 634-671 — IRON_NODE_METHOD_CALL dispatch, confirmed gap at line 649-652
- `src/hir/hir_to_lir.c` lines 774-850 — IRON_HIR_EXPR_METHOD_CALL lowering, `type_name = "Unknown"` fallback at line 776, lowercasing loop at lines 837-841
- `src/lir/emit_c.c` lines 88-136, 235-253, 3435-3450 — `mangle_func_name` guard (Iron_ prefix passthrough at line 116), `emit_type_to_c` array naming at lines 240-248, main() wrapper at lines 3438-3442
- `src/cli/build.c` lines 339-513, 641-752 — `argv_buf[96]` at line 513, strstr blocks at 641-732
- `src/cli/check.c` lines 155-264 — duplicate strstr import detection blocks at 161-245
- `src/lexer/lexer.h` lines 36, 134-141 — `IRON_TOK_IMPORT` confirmed, `iron_lex_all()` API
- `src/runtime/iron_string.c` lines 199-210 — `iron_runtime_init(void)` signature, intern table init
- `src/runtime/iron_runtime.h` lines 262-266 — `iron_runtime_init(void)` declaration
- `src/stdlib/iron_math.c` lines 61-72 — `__thread` usage confirmed
- `src/analyzer/types.h` lines 16-54 — all `IRON_TYPE_*` kind constants, `IRON_TYPE_ARRAY.array.elem` field
- `src/runtime/iron_collections.c` lines 3-13 — mangle_generic naming convention confirmed (`Iron_List_int64_t`, capital L)
- `tests/unit/test_runtime_string.c` line 10 — `iron_runtime_init()` in setUp (no args — will break)

### Secondary (MEDIUM confidence)
- `.planning/research/ARCHITECTURE.md` — prior research confirming dispatch gap and component boundaries
- `.planning/research/PITFALLS.md` — prior research confirming argv_buf overflow risk, strstr false-positive risk, Windows portability gaps

---

## Metadata

**Confidence breakdown:**
- Compiler dispatch (COMP-01, COMP-02): HIGH — direct code inspection confirms exact lines needing change, naming convention verified via emit_type_to_c + mangle_func_name chain
- Build system (COMP-03, COMP-04): HIGH — argv_buf[96] and strstr blocks confirmed at exact line numbers
- Runtime signature (COMP-07): HIGH — iron_runtime_init(void) signature confirmed in both .h and .c; emit_c.c (void)argc suppression confirmed at line 3440
- Windows docs (COMP-05, COMP-06): HIGH — gaps confirmed in iron_io.c, iron_log.c, iron_math.c; decision is document-only per locked constraint

**Research date:** 2026-04-02
**Valid until:** 2026-05-02 (stable codebase, low churn risk)
