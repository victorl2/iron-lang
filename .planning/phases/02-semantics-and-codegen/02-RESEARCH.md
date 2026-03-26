# Phase 2: Semantics and Codegen - Research

**Researched:** 2026-03-25
**Domain:** Compiler semantic analysis passes + C code generation
**Confidence:** HIGH

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Escape Analysis**
- One level of call tracking: analyze within a function and into direct callees to determine if a heap value escapes
- Compile error on escape: if a heap value escapes its scope without explicit `free` or `leak`, it's a compile error (not a warning)
- `rc` values are exempt from escape analysis — they manage their own lifetime via reference counting
- FFI (external C functions) assume escape: any heap value passed to an external function is treated as escaping
- Intra-procedural + one-level is the precision boundary — no full inter-procedural analysis

**Generics Monomorphization**
- Full monomorphization: generate a separate C struct/function for each concrete type instantiation (Iron_List_Int, Iron_List_String, etc.)
- Single output file: all modules compiled into one .c file, so deduplication is "don't emit the same instantiation twice"
- Interface constraints enforced: `List[T: Comparable]` validated at semantic analysis time with clear error messages
- Type suffix mangling: `Iron_List_Int_push()`, `Iron_Map_String_Int_get()` — readable C names using concrete type names

**C Symbol Naming / Mangling**
- Iron_ prefix on EVERYTHING: user types, methods, functions, globals — `Iron_Player`, `Iron_Entity`, `Iron_Vec2`
- Method naming: `Iron_Type_method` pattern — `Iron_Player_update(Iron_Player* self, double dt)`
- Nested/anonymous scopes: parent context suffix — `Iron_Player_update_lambda_0`, `Iron_main_lambda_1` for debuggability
- Entry point: real C `main()` calls `Iron_main()` — allows runtime initialization before user code
- Generated C compiles with `clang/gcc -std=c11 -Wall -Werror`

**Type Checking Strictness**
- No implicit numeric conversions: all widening/narrowing requires explicit cast — `Int64(myInt32)` — matches "no implicit conversions" philosophy
- Integer literals default to `Int` (int64_t): `val x = 42` is Int. Float literals default to `Float` (double): `val y = 3.14` is Float
- Flow-sensitive nullable narrowing: `if x == null { return } x.foo()` works — type narrows after any null check including early returns
- `is` keyword narrows type in block: `if entity is Player { entity.name }` — entity becomes Player inside the block, no explicit cast needed
- val immutability enforced: reassignment of val produces compile error (from SEM-05)

**Carried Forward from Phase 1**
- ConstructExpr/CallExpr unified at parse time — semantic analysis must disambiguate based on whether callee resolves to a type or function
- Interface method signatures stored as FuncDecl with body=NULL — semantic analysis validates completeness
- Iron_ prefix naming convention established in all Phase 1 code
- Arena allocator used for all compiler data structures
- E0001-style error codes on all diagnostics
- 55 AST node kinds with Iron_Span on every node

### Claude's Discretion
- Exact scope chain implementation for name resolution
- Symbol table data structure choice (hash map vs tree)
- Specific escape analysis algorithm details
- Order of semantic passes (name resolution before type checking is required, rest flexible)
- C code emission formatting and indentation style
- Specific error message wording and suggestion heuristics
- How to handle the `draw {}` block (contextual keyword vs reserved word)

### Deferred Ideas (OUT OF SCOPE)
None — discussion stayed within phase scope
</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| SEM-01 | Name resolution builds scoped symbol table (global → module → function → block) | Scope tree with parent-pointer chain; stb_ds hash maps per scope; two-pass for forward refs |
| SEM-02 | All identifiers resolve to declarations; undefined variables produce errors | Lookup walks scope chain upward; unresolved ident at global scope = E-code error |
| SEM-03 | Type inference works for val/var declarations without explicit types | Bottom-up inference from initializer expression; literal defaults (Int/Float) |
| SEM-04 | Type checker validates all assignments, function calls, and return types | Each expr node gets `resolved_type` annotation; structural type equality |
| SEM-05 | val immutability is enforced (reassignment produces compile error) | `is_mutable` flag on Symbol; assignment target check |
| SEM-06 | Nullable types require null check before use; compiler narrows type after check | Flow-sensitive narrowing via null-check dominance; `has_value` path tracking |
| SEM-07 | Interface implementation completeness is validated | After name resolution: for each `implements`, verify all method sigs have matching MethodDecl |
| SEM-08 | Generic type parameters are validated and instantiated | Monomorphization registry: (type_name, concrete_args) → generated C name; constraint check at instantiation site |
| SEM-09 | Escape analysis tracks heap allocations and marks non-escaping values for auto-free | Intra-procedural + one-level callee tracking; `auto_free` and `escapes` flags on HeapExpr nodes |
| SEM-10 | Concurrency checks enforce parallel-for body cannot mutate outer non-mutex variables | ForStmt.is_parallel gate; scan body for assignments to outer-scope non-mutex vars |
| SEM-11 | Import resolution locates .iron files by path and builds module graph | Path-to-file mapping; topological sort of import graph before semantic passes |
| SEM-12 | `self` and `super` resolve correctly inside methods | MethodDecl context carries implicit `self` symbol of the owning type; `super` resolves to parent type's method |
| GEN-01 | C code emitted for all Iron language constructs compiles with gcc/clang -std=c11 -Wall -Werror | Full type-mapping table from impl plan Phase 4a; Iron_ prefix on all symbols |
| GEN-02 | Defer statements execute in reverse order at every scope exit including early returns | Per-scope deferred-stmt stack in codegen; drain on every ReturnStmt/break/continue |
| GEN-03 | Object inheritance uses struct embedding; child pointer castable to parent | Embed parent as first field `_base`; type-tag int at offset 0 of root base |
| GEN-04 | Interface dispatch uses vtable structs with function pointers | `Iron_IFace_vtable` struct per interface; objects carry `vtable*`; dispatch via `obj->vtable->method(obj)` |
| GEN-05 | Generics monomorphized to concrete C types | Monomorphization registry in codegen; emit each (type, args) instantiation once |
| GEN-06 | Forward declarations and topological sort prevent C compilation order issues | Emit: includes → forward decls → sorted struct bodies → prototypes → implementations → main |
| GEN-07 | Generated C uses consistent namespace prefix to prevent symbol collisions | Iron_ prefix on all user symbols; blocklist of C/POSIX reserved names |
| GEN-08 | Nullable types generate Optional structs with value + has_value flag | `Iron_Optional_T { T value; bool has_value; }` per distinct nullable type |
| GEN-09 | Lambda expressions generate C function pointers with closure data | `Iron_Closure_N` env struct + function pointer; heap-alloc env when lambda escapes scope |
| GEN-10 | Spawn/await/channel/mutex generate correct thread pool and synchronization code | Spawn body lifted to named C function; pool_submit; await = condvar wait |
| GEN-11 | Parallel-for generates range splitting, chunk submission, and barrier | Chunk fn + pool_submit loop + pool_barrier; sequential fallback for small N |
| TEST-01 | C unit tests cover all compiler internals (lexer, parser, semantic, codegen) | Unity framework already in use; add test_resolver.c, test_typecheck.c, test_escape.c, test_codegen.c |
| TEST-02 | .iron integration tests verify end-to-end compilation and execution | Extend existing tests/integration/ with .iron fixtures + .expected files; shell runner invokes compiled binary |
</phase_requirements>

---

## Summary

Phase 2 takes the parser's AST output (55 node kinds, all with Iron_Span) through four semantic passes and then emits a single C11 file. The semantic passes run in strict order: name resolution builds the scope tree and symbol table; type checking annotates every expression node with its resolved type; escape analysis marks heap nodes for auto-free or compile error; concurrency checks validate parallel-for and spawn captures. Codegen then does a single traversal of the fully-annotated AST to emit the C file.

The most complex implementation challenges are: (1) ConstructExpr/CallExpr disambiguation — the parser emits IRON_NODE_CONSTRUCT for `Foo(...)` calls; semantic analysis must check whether `Foo` resolves to a type declaration or a function and rewrite accordingly; (2) flow-sensitive nullable narrowing — the type narrowing state must track dominance across `if`, `elif`, `else`, early returns, and `is` blocks; (3) defer codegen at all exit points — every `return`, `break`, and `continue` must drain the deferred-statement stack for all enclosing scopes; and (4) inheritance layout — the parent struct must be embedded as the first field `_base` with a type tag at offset 0 to support correct `is` checks at runtime.

The codegen output order is fixed by the implementation plan: includes first, then all forward struct declarations, then topologically-sorted struct bodies, then function prototypes, then function implementations, then the C `main()` wrapper that calls `Iron_main()`. This ordering handles cross-file struct dependencies and circular pointer references.

**Primary recommendation:** Implement the four semantic passes as independent `src/analyzer/` modules (scope.c, resolve.c, typecheck.c, escape.c, concurrency.c), each adding annotations to existing AST node fields. Codegen lives in `src/codegen/` split by concern (types.c, stmts.c, exprs.c). Every pass uses the existing Iron_Arena and stb_ds hash maps.

---

## Standard Stack

### Core (already established in Phase 1 — carry forward)

| Component | Version | Purpose | Status |
|-----------|---------|---------|--------|
| C11 | ISO/IEC 9899:2011 | Compiler implementation language | In use |
| Unity v2.6.1 | ThrowTheSwitch | C unit test framework | In use (CMakeLists.txt) |
| stb_ds.h | current (vendored) | Hash maps for symbol tables and monomorphization registry | In use |
| Iron_Arena | custom | Arena allocator for all compiler data | In use |
| Iron_StrBuf | custom | String builder for C code emission | In use (src/util/strbuf.h) |
| Iron_DiagList / iron_diag_emit | custom | Diagnostic accumulation with E-codes and spans | In use |

### New for Phase 2

| Component | Purpose | Location |
|-----------|---------|----------|
| Iron_Scope / Iron_Symbol | Scope tree node + symbol entry | src/analyzer/scope.h |
| Iron_Type | Type representation (primitive, object, interface, generic, nullable, func, array) | src/analyzer/types.h |
| Iron_Resolver | Name resolution pass | src/analyzer/resolve.h/c |
| Iron_TypeChecker | Type annotation pass | src/analyzer/typecheck.h/c |
| Iron_EscapeAnalyzer | Escape/auto-free analysis | src/analyzer/escape.h/c |
| Iron_ConcurrencyChecker | Parallel-for and spawn validation | src/analyzer/concurrency.h/c |
| Iron_Codegen | C file emitter | src/codegen/codegen.h/c |

### No New External Dependencies

Phase 2 requires no new external libraries. stb_ds.h handles all hash map needs (symbol table, type registry, monomorphization registry, string intern table). Iron_StrBuf handles all string building for code emission.

---

## Architecture Patterns

### Pass Order (Locked)

```
Iron_Program (AST from parser)
    |
    v  Pass 1: Name Resolution
Iron_Scope tree + all Iron_Ident nodes have resolved_sym pointer
    |
    v  Pass 2: Type Checking
All expression nodes have resolved_type; all decl nodes have declared_type
    |
    v  Pass 3: Escape Analysis
All Iron_HeapExpr nodes have escapes/auto_free flags; compile errors emitted for bare escapes
    |
    v  Pass 4: Concurrency Checks
ForStmt.is_parallel bodies validated; SpawnStmt captures validated
    |
    |  (gate: if any errors, stop here)
    v  Codegen
Single generated.c emitted
```

### Annotated AST Fields to Add to Nodes

Rather than creating new node types, semantic passes add annotation fields to existing AST structs. Since the AST nodes are C structs, this requires extending the struct definitions in ast.h:

```c
// Added to Iron_Ident (IRON_NODE_IDENT):
struct Iron_Symbol *resolved_sym;   // set by resolver; NULL = unresolved

// Added to all expression node structs (val/var decl, binary, call, etc.):
struct Iron_Type   *resolved_type;  // set by type checker

// Added to Iron_HeapExpr (IRON_NODE_HEAP):
bool  auto_free;   // set by escape analyzer: true = insert free at scope exit
bool  escapes;     // set by escape analyzer: true = compile error if no free/leak

// Added to Iron_ValDecl / Iron_VarDecl:
bool  is_mutable;  // false for ValDecl (used by assignment check)
```

### Recommended Project Structure for Phase 2

```
src/
├── analyzer/
│   ├── scope.h / scope.c           # Iron_Scope, Iron_Symbol, scope_push/pop/lookup
│   ├── types.h / types.c           # Iron_Type, type constructors, type_equals
│   ├── resolve.h / resolve.c       # Pass 1: name resolution
│   ├── typecheck.h / typecheck.c   # Pass 2: type annotation
│   ├── escape.h / escape.c         # Pass 3: escape analysis
│   └── concurrency.h / concurrency.c  # Pass 4: parallel-for + spawn checks
├── codegen/
│   ├── codegen.h / codegen.c       # Main orchestrator; Iron_Codegen context
│   ├── gen_types.c                 # Iron→C type mapping; Optional/Rc/vtable structs
│   ├── gen_stmts.c                 # Statement emission (if/while/for/match/defer/spawn)
│   └── gen_exprs.c                 # Expression emission (calls/binops/heap/rc/lambda)
tests/
├── test_resolver.c                 # Unity tests for name resolution
├── test_typecheck.c                # Unity tests for type checking
├── test_escape.c                   # Unity tests for escape analysis
├── test_codegen.c                  # Unity tests for C emission
└── integration/
    ├── hello.iron / hello.expected  # already exists
    ├── game.iron / game.expected    # already exists
    ├── variables.iron               # val/var, type inference, immutability
    ├── nullable.iron               # nullable narrowing, null checks
    ├── objects.iron                 # object construction, inheritance, is
    ├── interfaces.iron              # vtable dispatch, polymorphism
    ├── generics.iron                # monomorphized List/Map usage
    ├── memory.iron                  # heap, auto-free, defer
    └── concurrency.iron             # spawn, channel, parallel-for
```

### Pattern 1: Scope Tree as Parent-Pointer Chain

**What:** Each `Iron_Scope` has a pointer to its enclosing scope. Symbol lookup walks up the chain. Scopes are arena-allocated.

```c
// Source: ARCHITECTURE.md Pattern 4, adapted to Iron naming
typedef struct Iron_Scope {
    struct Iron_Scope *parent;     // NULL at global scope
    stb_ds hash map    symbols;    // name (char*) → Iron_Symbol*
    Iron_ScopeKind     kind;       // GLOBAL, MODULE, FUNCTION, BLOCK
} Iron_Scope;

Iron_Symbol *iron_scope_lookup(Iron_Scope *s, const char *name) {
    while (s) {
        Iron_Symbol *sym = shget(s->symbols, name);
        if (sym) return sym;
        s = s->parent;
    }
    return NULL;  // undefined — emit E-code error
}
```

**When to use:** All identifier resolution. The stb_ds `sh` functions (shget, shput, sh_new_strdup) handle string-keyed hash maps.

### Pattern 2: Two-Pass Name Resolution for Forward References

**What:** Iron allows `func Player.update()` to appear before `object Player` in the file (methods are defined outside object blocks). A single-pass resolver would fail to resolve `Player` in the method declaration. Use two passes: first pass collects all top-level declarations into the global scope; second pass resolves all identifier references.

```
Pass 1a: Scan all top-level decls, insert into global scope:
    ObjectDecl → symbol name=Player, kind=TYPE
    FuncDecl   → symbol name=main, kind=FUNCTION
    MethodDecl → attach to owning type's method table

Pass 1b: For each MethodDecl, resolve type_name → ObjectDecl or error

Pass 2: Walk all expressions and statements, resolving Ident nodes
```

**When to use:** Required. Methods declared outside object blocks are the driving case.

### Pattern 3: Iron_Type Representation

**What:** A discriminated union representing every Iron type. Arena-allocated and shared (pointer equality is valid for identical types if you intern them).

```c
typedef enum {
    IRON_TYPE_INT, IRON_TYPE_INT8, IRON_TYPE_INT16, IRON_TYPE_INT32, IRON_TYPE_INT64,
    IRON_TYPE_UINT, IRON_TYPE_UINT8, IRON_TYPE_UINT16, IRON_TYPE_UINT32, IRON_TYPE_UINT64,
    IRON_TYPE_FLOAT, IRON_TYPE_FLOAT32, IRON_TYPE_FLOAT64,
    IRON_TYPE_BOOL, IRON_TYPE_STRING,
    IRON_TYPE_OBJECT,      // user-declared object; carries Iron_ObjectDecl*
    IRON_TYPE_INTERFACE,   // user-declared interface; carries Iron_InterfaceDecl*
    IRON_TYPE_ENUM,        // user-declared enum; carries Iron_EnumDecl*
    IRON_TYPE_NULLABLE,    // T?; carries inner Iron_Type*
    IRON_TYPE_FUNC,        // function pointer type; carries param_types[] + return_type
    IRON_TYPE_ARRAY,       // [T; N] or [T] (dynamic); carries element_type + size
    IRON_TYPE_RC,          // rc T; carries inner Iron_Type*
    IRON_TYPE_GENERIC_PARAM,  // T in generic context; carries constraint Iron_Type*
    IRON_TYPE_VOID,
    IRON_TYPE_NULL,        // the type of the null literal (assignable to any T?)
    IRON_TYPE_ERROR,       // sentinel for propagating type errors without cascades
} Iron_TypeKind;

typedef struct Iron_Type {
    Iron_TypeKind kind;
    union {
        struct { Iron_ObjectDecl    *decl; } object;
        struct { Iron_InterfaceDecl *decl; } interface;
        struct { Iron_EnumDecl      *decl; } enu;
        struct { struct Iron_Type   *inner; } nullable;
        struct { struct Iron_Type   *inner; } rc;
        struct {
            struct Iron_Type **param_types;
            int               param_count;
            struct Iron_Type  *return_type;
        } func;
        struct {
            struct Iron_Type *elem;
            int               size;    /* -1 = dynamic */
        } array;
        struct {
            const char       *name;
            struct Iron_Type  *constraint;  /* NULL = unconstrained */
        } generic_param;
        struct {
            /* monomorphized generic: base type + concrete args */
            struct Iron_Type **args;
            int               arg_count;
            Iron_ObjectDecl  *base_decl;
        } mono;
    };
} Iron_Type;
```

**When to use:** Every expression node and declaration carries an `Iron_Type*`. Intern primitive types (allocate once, share pointer). Allocate object/interface/nullable types on the arena.

### Pattern 4: ConstructExpr/CallExpr Disambiguation

**What:** The parser emits `IRON_NODE_CONSTRUCT` for any `Name(args...)` expression where `Name` starts uppercase (or is otherwise ambiguous). Semantic analysis must resolve:
- If `Name` resolves to an `Iron_ObjectDecl` or `Iron_EnumDecl` → it's a construction
- If `Name` resolves to an `Iron_FuncDecl` or `Iron_MethodDecl` → it's a function call (rewrite node kind or add `is_construct` flag)

```c
// During type checking of Iron_ConstructExpr:
Iron_Symbol *sym = iron_scope_lookup(ctx->scope, node->type_name);
if (!sym) { emit_error(E_UNDEFINED, node->span, node->type_name); return TYPE_ERROR; }
if (sym->kind == SYM_TYPE) {
    // construction — validate arg count and types against field order
    node->resolved_sym = sym;
    return sym->type;
} else if (sym->kind == SYM_FUNCTION) {
    // rewrite: this is actually a function call
    node->base.kind = IRON_NODE_CALL;
    // ... validate as call
}
```

### Pattern 5: Flow-Sensitive Nullable Narrowing

**What:** Track a "narrowed types" map per control-flow branch. After `if x == null { return }`, `x`'s type in the continuation is `T` (non-nullable). After `if x != null { ... }`, `x`'s type inside the block is `T`.

```c
// NarrowingEnv maps symbol → narrowed Iron_Type*
// Merging envs at join points (after if/else) uses intersection:
// if both branches narrow x to T, the join narrows x to T
// if only one branch narrows, the join does not narrow

typedef struct {
    char         *sym_name;    // stb_ds key
    Iron_Type    *narrowed_to; // value
} NarrowEntry;

// After visiting an if condition:
// if condition is (x == null), mark x as non-nullable in the else branch / after early return
// if condition is (x != null), mark x as non-nullable inside the if body
// `is` expr: mark x as the tested type inside the block
```

**When to use:** TypeChecker pass. Track narrowing state as a stb_ds hash map that is saved/restored at branch points. Early returns (`if x == null { return }`) narrow in the continuation of the enclosing block.

### Pattern 6: Defer Stack in Codegen

**What:** The codegen context maintains a stack of deferred statement arrays, one per scope level. On entering a block, push a new level. On exiting (end of block, return, break, continue), drain all deferred stmts from the current level outward in reverse order before emitting the exit.

```c
// Codegen context carries:
typedef struct {
    Iron_Node ***defer_stacks;  // dynamic array of dynamic arrays
    int          defer_depth;
} Iron_Codegen;

// On IRON_NODE_BLOCK enter:
arrpush(ctx->defer_stacks, NULL);  // push empty deferred list
ctx->defer_depth++;

// On IRON_NODE_DEFER:
arrpush(ctx->defer_stacks[ctx->defer_depth - 1], stmt->expr);

// On IRON_NODE_RETURN:
// emit all defers from depth-1 down to function scope, innermost first
for (int d = ctx->defer_depth - 1; d >= function_scope_depth; d--) {
    int n = arrlen(ctx->defer_stacks[d]);
    for (int i = n - 1; i >= 0; i--) {
        emit_expr(ctx, ctx->defer_stacks[d][i]);
    }
}
// then emit return

// On IRON_NODE_BLOCK exit:
// emit defers for this scope (non-return exit)
// then pop the level
```

**When to use:** Codegen pass. The critical rule: any exit from a scope must drain that scope's defers and all enclosing function-level defers.

### Pattern 7: Inheritance Layout and Type Tags

**What:** Child structs embed parent as the first field `_base`. The root of every inheritance hierarchy carries an `int32_t iron_type_tag` as its very first field. The type tag is a compile-time constant assigned to each object type.

```c
// Iron: object Entity { var pos: Vec2; var hp: Int }
// Iron: object Player extends Entity { val name: String }

// Generated C:
typedef struct Iron_Entity {
    int32_t     iron_type_tag;  // IRON_TAG_ENTITY = 1
    Iron_Vec2   pos;
    int64_t     hp;
} Iron_Entity;

typedef struct Iron_Player {
    Iron_Entity _base;          // first field — layout-compatible with Iron_Entity
    Iron_String name;
} Iron_Player;

// Cast: (Iron_Entity*)&player == &player._base (guaranteed by C struct layout rules)
// is check: ((Iron_Entity*)ptr)->iron_type_tag == IRON_TAG_PLAYER
```

**When to use:** Codegen pass. Assign type tags during the codegen struct emission phase. The type tag must be at the same offset in all structs in the hierarchy — guaranteed by `_base` embedding at position 0.

### Pattern 8: Interface Vtable Dispatch

**What:** Each interface becomes a vtable struct with function pointers. Each implementing type gets a statically allocated vtable instance. Objects that implement an interface store a `vtable*` pointer.

```c
// Iron: interface Drawable { func draw() }
// Iron: object Player implements Drawable { ... }

// Generated C:
typedef struct Iron_Drawable_vtable {
    void (*draw)(void* self);
} Iron_Drawable_vtable;

// Static vtable for Player implementing Drawable:
static Iron_Drawable_vtable Iron_Player_Drawable_vtable = {
    .draw = (void(*)(void*))Iron_Player_draw,
};

// A [Drawable] collection item:
typedef struct {
    void                   *object;
    Iron_Drawable_vtable   *vtable;
} Iron_Drawable_ref;

// Dispatch:
item.vtable->draw(item.object);
```

**When to use:** Codegen pass. Build the vtable struct during interface type emission. Assign vtable instances per (implementing_type, interface) pair.

### Pattern 9: Monomorphization Registry

**What:** A hash map keyed by `(type_name, concrete_arg_types_mangled)` → already emitted flag. When the type checker sees `List[Enemy]`, it computes the mangled name `Iron_List_Iron_Enemy`, checks if this instantiation is already in the registry; if not, records it and queues it for emission.

```c
typedef struct {
    char  *mangled_name;       // stb_ds key: "Iron_List_Iron_Enemy"
    bool   emitted;
} MonoEntry;

// stb_ds hash map: shget/shput with string keys
MonoEntry *mono_registry = NULL;
sh_new_strdup(mono_registry);

// On encountering List[Enemy]:
char *key = mangle_generic("Iron_List", args, arg_count, arena);
MonoEntry *entry = shgetp_null(mono_registry, key);
if (!entry || !entry->emitted) {
    shput(mono_registry, key, (MonoEntry){.mangled_name = key, .emitted = false});
    // queue for codegen
}
```

**When to use:** Both type checker (to validate constraints) and codegen (to emit instantiations). The registry must be shared across both phases.

### Anti-Patterns to Avoid

- **Interleaving passes:** Do not resolve names inside the type checker or emit code while type checking. Passes must be strictly sequential.
- **Single-pass name resolution:** Iron allows forward references (methods before object declarations). Two sub-passes (collect declarations, then resolve uses) are required.
- **Defer as tail emission only:** The implementation plan's defer example only shows the happy-path end-of-function case. Every `return`, `break`, and `continue` must also drain defers. Test with a function that has three early returns.
- **Field flattening for inheritance:** Do NOT copy parent fields into child struct. Embed `_base` struct to guarantee pointer aliasing under C's struct layout rules.
- **Emitting structs in source order:** Must topologically sort struct definitions by dependency before emission or forward-declared but body-undefined types cause C errors.
- **Missing Iron_ prefix:** Any user symbol emitted without the `Iron_` prefix risks colliding with C standard library names (`signal`, `error`, `list`, `string`, `file`). The prefix is established in Phase 1 naming conventions and must be consistent.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Hash maps for symbol tables | Custom hash table | stb_ds `shget/shput/sh_new_strdup` | Already vendored and in use; handles char* keys natively |
| String building for C emission | Custom string concat | Iron_StrBuf + iron_strbuf_appendf | Already implemented in src/util/strbuf.h; handles growing buffer |
| AST traversal | Custom recursive walk | iron_ast_walk with Iron_Visitor | Already implemented in ast.c; handles all 55 node kinds |
| Struct field layout arithmetic | Manual offset calculation | Rely on C struct layout rules (`_base` first field = same address) | C11 standard guarantees first-field address equality |
| Topological sort | Custom DFS | DFS with gray/black coloring (textbook algorithm, ~30 lines) | Standard algorithm; trivial to implement correctly once |
| Error accumulation | Global error flags | Iron_DiagList + iron_diag_emit | Already implemented; supports E-codes, spans, suggestions |

---

## Common Pitfalls

### Pitfall 1: ConstructExpr Disambiguation Missed

**What goes wrong:** `Player(a, b)` is parsed as IRON_NODE_CONSTRUCT. If the type checker encounters a ConstructExpr whose `type_name` resolves to a function (e.g., a factory function `Player` returning Player), it must rewrite the node kind to IRON_NODE_CALL or handle it as a call. Missing this rewrite causes the codegen to emit `(Iron_Player){ .field1=a, .field2=b }` for what should be a function call.
**Why it happens:** The parser made a syntactic guess. The semantic layer must correct it.
**How to avoid:** In the type checker, when processing IRON_NODE_CONSTRUCT, check what the resolved symbol kind is. If SYM_FUNCTION, treat as a call. If SYM_TYPE, treat as construction. Emit E0002-style error if the symbol is undefined.

### Pitfall 2: Interface Body=NULL FuncDecl Leaks into Codegen

**What goes wrong:** Interface method signatures are stored as FuncDecl nodes with `body=NULL`. If semantic analysis passes do not explicitly validate interface completeness and the codegen walks a FuncDecl with body=NULL, it either emits an empty function body (broken C) or crashes with a null dereference.
**How to avoid:** In the type checker (after name resolution), for each ObjectDecl that has `implements_names`, verify every named method sig from the interface has a corresponding MethodDecl with a non-NULL body. Emit E-code error for missing implementations. In codegen, assert body != NULL before emitting a function body and skip interface method sig FuncDecls entirely.

### Pitfall 3: Defer Missing Early Returns

**What goes wrong:** Defer only runs at end of function, not on early returns.
**How to avoid:** See Pattern 6. Test explicitly: write `func foo() { val f = open(); defer close(f); if err { return } }` and verify `close(f)` appears before the early return in the generated C.
**Warning signs:** Defer unit tests only exercise single-return functions.

### Pitfall 4: Nullable Narrowing Not Propagated Past Early Returns

**What goes wrong:** The flow-sensitive narrowing pattern `if x == null { return } x.foo()` narrows `x` only when the analyzer explicitly handles "exit from the current block narrows the continuation." A naive implementation only narrows inside the `if` body, not in the code following the early return.
**How to avoid:** After visiting an `if` block, check if the block always exits (contains a return/break on all paths). If so, the narrowing from the condition applies in the continuation. This is the "early-exit narrowing" rule.
**Warning signs:** Test case `if x == null { return } / x.method()` produces a "cannot call method on nullable" error.

### Pitfall 5: Topological Sort Fails on Pointer-Only Cycles

**What goes wrong:** Two object types that hold pointer references to each other (`Player` holds `rc Enemy`, `Enemy` holds `rc Player`) appear as a cycle in a naive dependency graph. The topological sort aborts with a "circular dependency" error when no real circularity exists for struct layout.
**How to avoid:** Build the dependency graph on VALUE-type containment only (fields whose types require the full struct definition). Pointer/rc/nullable fields require only a forward declaration. In the sort, a pointer-to-T dependency uses the forward declaration (emitted in pass 1), not the full definition. A value-type containment cycle IS a real error (infinite size struct) and should be caught in semantic analysis.

### Pitfall 6: Lambda Closure with Stack-Captured Variables

**What goes wrong:** Lambdas capture outer variables by reference. If a lambda is stored in an object field (`val btn = Button(... func() { score += 1 })`), the captured `score` pointer becomes dangling after the declaring scope exits.
**How to avoid:** In escape analysis: if a lambda (IRON_NODE_LAMBDA) is assigned to an object field or passed to `spawn`, check if any captured variables are stack-allocated locals. If so: compile error or promote the captured variable to heap. Flag the lambda as "escaping" and generate a heap-allocated closure environment struct.
**Warning signs:** Segfault when a button's `on_click` callback fires after the function that created it has returned.

### Pitfall 7: Monomorphization Infinite Recursion

**What goes wrong:** A generic type `List[T]` whose methods take `List[T]` parameters can cause the monomorphization to infinitely recurse: instantiating `List[Enemy]` triggers instantiation of `List[List[Enemy]]` etc.
**How to avoid:** The monomorphization registry (see Pattern 9) provides an "in progress" state. If the registry shows a type as "in progress", skip re-emission. The recursion is stopped by the fact that generic methods only operate on already-instantiated types.

### Pitfall 8: `draw {}` Block — Unhandled Contextual Keyword

**What goes wrong:** The language spec mentions `draw { }` as a game-dev block that wraps begin/end drawing calls. The lexer does NOT have a `draw` keyword (it's not in the 37-entry keyword list). The parser treats it as an identifier followed by a block. Semantic analysis receives this as an identifier `draw` used as a statement.
**How to avoid:** For Phase 2, treat `draw { }` as a special function call form: if semantic analysis sees an identifier `draw` used in a statement position followed by a block, lower it to a call to a built-in `Iron_begin_drawing()` / `Iron_end_drawing()` pair with the block body in between. The context string `draw` is recognized by the semantic pass, not the lexer. This is the "contextual keyword" approach.

---

## Code Examples

### Scope Lookup with stb_ds

```c
// Source: stb_ds.h + ARCHITECTURE.md Pattern 4
#include "vendor/stb_ds.h"

typedef struct {
    char          *key;   // stb_ds requires this field name for string-keyed maps
    Iron_Symbol   *value;
} Iron_SymbolEntry;

typedef struct Iron_Scope {
    struct Iron_Scope  *parent;
    Iron_SymbolEntry   *symbols;  // stb_ds hash map
    Iron_ScopeKind      kind;
} Iron_Scope;

Iron_Scope *iron_scope_create(Iron_Arena *arena, Iron_Scope *parent, Iron_ScopeKind kind) {
    Iron_Scope *s = ARENA_ALLOC(arena, Iron_Scope);
    s->parent  = parent;
    s->symbols = NULL;
    s->kind    = kind;
    sh_new_strdup(s->symbols);
    return s;
}

Iron_Symbol *iron_scope_lookup(Iron_Scope *s, const char *name) {
    for (; s; s = s->parent) {
        ptrdiff_t idx = shgeti(s->symbols, name);
        if (idx >= 0) return s->symbols[idx].value;
    }
    return NULL;
}

void iron_scope_define(Iron_Scope *s, const char *name, Iron_Symbol *sym) {
    shput(s->symbols, name, sym);
}
```

### Type Equality Check

```c
// Structural equality — NOT pointer equality (except for interned primitives)
bool iron_type_equals(const Iron_Type *a, const Iron_Type *b) {
    if (a == b) return true;  // pointer equality: same interned primitive
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case IRON_TYPE_NULLABLE:
            return iron_type_equals(a->nullable.inner, b->nullable.inner);
        case IRON_TYPE_OBJECT:
        case IRON_TYPE_INTERFACE:
        case IRON_TYPE_ENUM:
            return a->object.decl == b->object.decl;  // pointer identity of the decl
        case IRON_TYPE_FUNC:
            if (a->func.param_count != b->func.param_count) return false;
            for (int i = 0; i < a->func.param_count; i++)
                if (!iron_type_equals(a->func.param_types[i], b->func.param_types[i])) return false;
            return iron_type_equals(a->func.return_type, b->func.return_type);
        case IRON_TYPE_ARRAY:
            return a->array.size == b->array.size &&
                   iron_type_equals(a->array.elem, b->array.elem);
        default:
            return a->kind == b->kind;  // primitive: kind equality is sufficient
    }
}
```

### C Emission for a Simple Object

```c
// Iron: object Vec2 { var x: Float; var y: Float }
// Generated:
//   Forward decl (pass 1):    typedef struct Iron_Vec2 Iron_Vec2;
//   Struct body (pass 2):     typedef struct Iron_Vec2 { double x; double y; } Iron_Vec2;

void emit_object_forward_decl(Iron_StrBuf *sb, const char *mangled_name) {
    iron_strbuf_appendf(sb, "typedef struct %s %s;\n", mangled_name, mangled_name);
}

void emit_object_body(Iron_StrBuf *sb, Iron_ObjectDecl *decl, Iron_Codegen *ctx) {
    const char *mangled = mangle_type_name(decl->name, ctx->arena);
    iron_strbuf_appendf(sb, "struct %s {\n", mangled);
    if (decl->extends_name) {
        // embed parent as first field
        const char *parent = mangle_type_name(decl->extends_name, ctx->arena);
        iron_strbuf_appendf(sb, "    %s _base;\n", parent);
    } else if (participates_in_is_checks(decl, ctx)) {
        // root of inheritance hierarchy: embed type tag
        iron_strbuf_appendf(sb, "    int32_t iron_type_tag;\n");
    }
    for (int i = 0; i < decl->field_count; i++) {
        emit_field(sb, (Iron_Field*)decl->fields[i], ctx);
    }
    iron_strbuf_appendf(sb, "};\n");
}
```

### Defer Drain on Return

```c
// Codegen: emit deferred stmts before every scope exit
void emit_defers_for_return(Iron_Codegen *ctx, Iron_StrBuf *sb, int function_depth) {
    for (int d = ctx->defer_depth - 1; d >= function_depth; d--) {
        int n = (int)arrlen(ctx->defer_stacks[d]);
        for (int i = n - 1; i >= 0; i--) {
            emit_stmt_or_expr(sb, ctx->defer_stacks[d][i], ctx);
            iron_strbuf_appendf(sb, ";\n");
        }
    }
}

// Called at every IRON_NODE_RETURN during codegen walk:
case IRON_NODE_RETURN: {
    Iron_ReturnStmt *n = (Iron_ReturnStmt*)node;
    emit_defers_for_return(ctx, sb, ctx->current_function_depth);
    iron_strbuf_appendf(sb, "return ");
    if (n->value) emit_expr(sb, n->value, ctx);
    iron_strbuf_appendf(sb, ";\n");
    break;
}
```

### Nullable Optional Struct Emission

```c
// Iron: var target: Enemy? = null
// Generated:
//   typedef struct { Iron_Enemy value; bool has_value; } Iron_Optional_Iron_Enemy;
//   Iron_Optional_Iron_Enemy target = { .has_value = false };

// Null check narrowing in generated C:
// Iron: if target != null { target.attack() }
// Generated:
//   if (target.has_value) {
//       Iron_Enemy_attack(&target.value);
//   }
```

---

## Type Mapping Table (Authoritative — from implementation_plan.md Phase 4a)

| Iron Type | C Type | Notes |
|-----------|--------|-------|
| `Int` | `int64_t` | Platform int (always 64-bit by spec) |
| `Int8` | `int8_t` | |
| `Int16` | `int16_t` | |
| `Int32` | `int32_t` | |
| `Int64` | `int64_t` | |
| `UInt` | `uint64_t` | |
| `UInt8` | `uint8_t` | Also used as `byte` |
| `UInt16` | `uint16_t` | |
| `UInt32` | `uint32_t` | |
| `UInt64` | `uint64_t` | |
| `Float` | `double` | Default float = double |
| `Float32` | `float` | |
| `Float64` | `double` | |
| `Bool` | `bool` | from `<stdbool.h>` |
| `String` | `Iron_String` | Runtime struct (Phase 3); stub for Phase 2 |
| `object Foo` | `typedef struct Iron_Foo Iron_Foo;` | With Iron_ prefix |
| `enum Bar` | `typedef enum Iron_Bar Iron_Bar;` | With Iron_ prefix |
| `[T; N]` | `T name[N]` | Fixed-size array |
| `T?` | `Iron_Optional_Iron_T` | Generated struct: `{ T value; bool has_value; }` |
| `rc T` | `Iron_Rc_Iron_T` | Runtime-managed (stub for Phase 2) |
| `func(A) -> R` | `R (*name)(A)` | Function pointer |
| `List[T]` | `Iron_List_Iron_T` | Monomorphized; runtime-backed |
| `Map[K,V]` | `Iron_Map_Iron_K_Iron_V` | Monomorphized; runtime-backed |
| `Set[T]` | `Iron_Set_Iron_T` | Monomorphized; runtime-backed |

---

## C Emission Order (Fixed — from implementation_plan.md Phase 4b)

```
1. #include directives
   - #include <stdint.h>
   - #include <stdbool.h>
   - #include <stdlib.h>     (for malloc/free)
   - #include <string.h>
   - #include <pthread.h>    (if concurrency features used)
   - #include "iron_runtime.h"
   - #include "iron_math.h"  (if import math present)
   - #include "iron_io.h"    (if import io present)

2. Forward declarations of all structs
   typedef struct Iron_Player Iron_Player;
   typedef struct Iron_Enemy  Iron_Enemy;
   ...

3. Struct definitions (topologically sorted by value-type field dependency)
   typedef struct Iron_Vec2 { double x; double y; } Iron_Vec2;
   typedef struct Iron_Entity { int32_t iron_type_tag; Iron_Vec2 pos; int64_t hp; } Iron_Entity;
   typedef struct Iron_Player { Iron_Entity _base; Iron_String name; } Iron_Player;

4. Function prototypes (all functions declared before any body)
   void Iron_Player_update(Iron_Player* self, double dt);
   ...

5. Function implementations (in any order, all types already declared)
   void Iron_Player_update(Iron_Player* self, double dt) { ... }
   ...

6. C main() entry point
   int main(int argc, char** argv) {
       iron_runtime_init();
       Iron_main();
       iron_runtime_shutdown();
       return 0;
   }
```

---

## State of the Art

| Old Approach | Current Approach | Impact for Iron |
|--------------|------------------|-----------------|
| Single-pass semantic analysis | Multi-pass: resolve → typecheck → escape → concurrency | Required for forward references and incremental error reporting |
| Lowering to custom IR | Annotated AST as IR | Appropriate for C-targeting transpiler; no IR design needed |
| Borrow checker for memory safety | Escape analysis + explicit `heap`/`free`/`leak` + `rc` | Iron's deliberate choice; Phase 2 implements the escape half |
| Separate compilation units | Single .c output (all modules inlined) | Simplifies Phase 2 linking; deduplication via monomorphization registry |
| Pointer-based inheritance (vtable in every struct) | Struct embedding + separate vtable per interface | C-idiomatic; avoids runtime overhead for non-interface code paths |

---

## Open Questions

1. **draw {} lowering specifics**
   - What we know: `draw` is not a keyword; the block is a game-dev convenience that wraps `BeginDrawing()` / `EndDrawing()` from raylib
   - What's unclear: Should semantic analysis lower it to a built-in pseudo-function, or should it be treated as a recognized identifier calling a codegen-defined macro?
   - Recommendation: In the semantic pass, recognize an `Iron_Ident(name="draw")` used as a statement with a following block as a special form. Lower to a `draw_block` node kind or mark with a flag. Codegen emits `BeginDrawing(); { ... } EndDrawing();`. This is scoped within Phase 2 discretion.

2. **`val` on object parameters — C const semantics**
   - What we know: `player: Player` (immutable reference) should become `const Iron_Player* self`. But C does not allow reassigning a `const*` to a non-const pointer in expressions.
   - What's unclear: How strictly should `const` propagate through generated C? The `-Wall -Werror` constraint will reject `const*` passed to non-const parameter functions.
   - Recommendation: For Phase 2, generate `const Iron_Player*` only for immutable function parameters; methods always use non-const `self` (Iron semantics: self is always mutable). Track this as a codegen detail.

3. **Multiple return values — C translation**
   - What we know: Iron allows `return a, b` from functions with `-> T1, T2` return types
   - What's unclear: The implementation plan does not specify a C translation for multiple returns
   - Recommendation: Generate an anonymous result struct `Iron_Result_T1_T2 { T1 v0; T2 v1; }` and return it by value. At the call site, destructure into the declared variables. This keeps generated code clean and avoids out-parameters.

---

## Validation Architecture

nyquist_validation is enabled in .planning/config.json.

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Unity v2.6.1 (ThrowTheSwitch) |
| Config file | CMakeLists.txt (FetchContent unity, add_executable per test) |
| Quick run command | `cmake --build build && ctest --test-dir build -R "test_resolver\|test_typecheck\|test_escape\|test_concurrency\|test_codegen" --output-on-failure` |
| Full suite command | `cmake --build build && ctest --test-dir build --output-on-failure` |

### Phase Requirements to Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SEM-01 | Scope tree built correctly; symbols accessible at correct scopes | unit | `ctest --test-dir build -R test_resolver -x` | Wave 0 |
| SEM-02 | Undefined variable produces E-code error | unit | `ctest --test-dir build -R test_resolver -x` | Wave 0 |
| SEM-03 | Type inference for val/var without explicit type | unit | `ctest --test-dir build -R test_typecheck -x` | Wave 0 |
| SEM-04 | Assignment/call type mismatch produces error | unit | `ctest --test-dir build -R test_typecheck -x` | Wave 0 |
| SEM-05 | val reassignment produces compile error | unit | `ctest --test-dir build -R test_typecheck -x` | Wave 0 |
| SEM-06 | Nullable null check required; narrowing works | unit | `ctest --test-dir build -R test_typecheck -x` | Wave 0 |
| SEM-07 | Missing interface method produces error | unit | `ctest --test-dir build -R test_typecheck -x` | Wave 0 |
| SEM-08 | Generic constraint violation produces error | unit | `ctest --test-dir build -R test_typecheck -x` | Wave 0 |
| SEM-09 | Escaping heap value without free/leak produces error; auto-free generates free() | unit | `ctest --test-dir build -R test_escape -x` | Wave 0 |
| SEM-10 | Parallel-for outer mutation produces error | unit | `ctest --test-dir build -R test_concurrency -x` | Wave 0 |
| SEM-11 | Import resolution finds files; module graph built | unit | `ctest --test-dir build -R test_resolver -x` | Wave 0 |
| SEM-12 | self/super resolve inside methods | unit | `ctest --test-dir build -R test_resolver -x` | Wave 0 |
| GEN-01 | Generated C compiles with -std=c11 -Wall -Werror | integration | `ctest --test-dir build -R integration_` | Wave 0 |
| GEN-02 | Defer runs on all exit paths | unit | `ctest --test-dir build -R test_codegen -x` | Wave 0 |
| GEN-03 | Inheritance: child pointer castable to parent; is check correct | integration | `ctest --test-dir build -R integration_objects` | Wave 0 |
| GEN-04 | Interface dispatch calls correct implementation | integration | `ctest --test-dir build -R integration_interfaces` | Wave 0 |
| GEN-05 | Monomorphized generic compiles and runs correctly | integration | `ctest --test-dir build -R integration_generics` | Wave 0 |
| GEN-06 | Multi-type program compiles without forward decl errors | integration | `ctest --test-dir build -R integration_` | Wave 0 |
| GEN-07 | Program using identifiers signal/error/file compiles | unit | `ctest --test-dir build -R test_codegen -x` | Wave 0 |
| GEN-08 | Nullable Optional struct generated; has_value check works | integration | `ctest --test-dir build -R integration_nullable` | Wave 0 |
| GEN-09 | Lambda stored in object field and triggered later — no segfault | integration | `ctest --test-dir build -R integration_lambda` | Wave 0 |
| GEN-10 | Spawn/await/channel/mutex compile and execute | integration | `ctest --test-dir build -R integration_concurrency` | Wave 0 |
| GEN-11 | Parallel-for produces correct results | integration | `ctest --test-dir build -R integration_concurrency` | Wave 0 |
| TEST-01 | All semantic and codegen passes covered by unit tests | unit | `ctest --test-dir build --output-on-failure` | Wave 0 |
| TEST-02 | .iron integration tests compile and produce correct output | integration | `ctest --test-dir build -R integration_` | Wave 0 |

### Sampling Rate

- **Per task commit:** `cmake --build build && ctest --test-dir build -R test_resolver -x` (or the relevant pass's test)
- **Per wave merge:** `cmake --build build && ctest --test-dir build --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps

- [ ] `tests/test_resolver.c` — covers SEM-01, SEM-02, SEM-11, SEM-12
- [ ] `tests/test_typecheck.c` — covers SEM-03, SEM-04, SEM-05, SEM-06, SEM-07, SEM-08
- [ ] `tests/test_escape.c` — covers SEM-09
- [ ] `tests/test_concurrency.c` — covers SEM-10
- [ ] `tests/test_codegen.c` — covers GEN-02, GEN-07
- [ ] `tests/integration/variables.iron` + `.expected` — covers GEN-01, GEN-08
- [ ] `tests/integration/objects.iron` + `.expected` — covers GEN-03
- [ ] `tests/integration/interfaces.iron` + `.expected` — covers GEN-04
- [ ] `tests/integration/generics.iron` + `.expected` — covers GEN-05, GEN-06
- [ ] `tests/integration/nullable.iron` + `.expected` — covers GEN-08
- [ ] `tests/integration/lambda.iron` + `.expected` — covers GEN-09
- [ ] `tests/integration/concurrency.iron` + `.expected` — covers GEN-10, GEN-11
- [ ] CMakeLists.txt entries for: test_resolver, test_typecheck, test_escape, test_concurrency, test_codegen, and all integration test targets
- [ ] Integration test runner script — compiles .iron files through the full pipeline and diffs output against .expected

---

## Sources

### Primary (HIGH confidence)

- `docs/implementation_plan.md` — Phase 3 (semantic sub-passes), Phase 4 (type mapping table, emission order, C translations)
- `docs/language_definition.md` — Complete type system, nullable rules, inheritance, interfaces, generics, memory keywords, concurrency spec
- `src/parser/ast.h` — All 55 AST node kinds; Iron_Visitor; Iron_Span; actual struct fields available for annotation
- `src/parser/ast.c` — Full iron_ast_walk dispatch; all child traversal patterns
- `src/diagnostics/diagnostics.h` — Iron_DiagList, iron_diag_emit, existing E-code constants (1-106)
- `src/util/arena.h` — ARENA_ALLOC macro; iron_arena_alloc/strdup API
- `src/util/strbuf.h` — iron_strbuf_appendf; Iron_StrBuf API
- `.planning/research/ARCHITECTURE.md` — Pipeline, annotated AST as IR, scope tree pattern, defer stack pattern, topological sort
- `.planning/research/PITFALLS.md` — C naming collisions, defer multi-exit, monomorphization deduplication, lambda closure escaping, inheritance layout, type inference error attribution
- `.planning/research/STACK.md` — stb_ds usage patterns; Unity test framework; arena allocator rationale
- `.planning/phases/02-semantics-and-codegen/02-CONTEXT.md` — All locked decisions: escape analysis contract, monomorphization strategy, naming conventions, type strictness

### Secondary (MEDIUM confidence)

- Architecture confirmed by Nim compiler internals (nim-lang.org/docs/intern.html): multi-pass, annotated AST as IR
- Flow-sensitive narrowing pattern confirmed by TypeScript implementation (GitHub issue #37802)
- Struct embedding for inheritance confirmed by C struct layout rules (C11 §6.7.2.1)

### Tertiary (LOW confidence — needs verification in implementation)

- draw {} block treatment as contextual keyword: approach inferred from spec + language design; specific lowering strategy is Claude's discretion

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all libraries already in use from Phase 1
- Architecture patterns: HIGH — confirmed by Phase 1 code, implementation plan, and prior research
- Type mapping: HIGH — authoritative from implementation_plan.md Phase 4a
- Emission order: HIGH — authoritative from implementation_plan.md Phase 4b
- Pitfalls: HIGH — all documented in prior research with specific prevention strategies
- draw {} handling: LOW — language spec incomplete; approach is reasoned inference

**Research date:** 2026-03-25
**Valid until:** 2026-06-25 (stable domain; changes only if language spec changes)
