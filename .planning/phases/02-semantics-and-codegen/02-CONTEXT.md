# Phase 2: Semantics and Codegen - Context

**Gathered:** 2025-03-25
**Status:** Ready for planning

<domain>
## Phase Boundary

Semantic analysis fully annotates the AST and the code generator emits valid C11 that compiles and executes correctly. This phase delivers: name resolution, type checking, nullable narrowing, interface validation, generic instantiation, escape analysis, concurrency checks, and C code emission for all language constructs. Runtime library, standard library, CLI toolchain, and comptime are separate phases.

</domain>

<decisions>
## Implementation Decisions

### Escape Analysis
- One level of call tracking: analyze within a function and into direct callees to determine if a heap value escapes
- Compile error on escape: if a heap value escapes its scope without explicit `free` or `leak`, it's a compile error (not a warning)
- `rc` values are exempt from escape analysis — they manage their own lifetime via reference counting
- FFI (external C functions) assume escape: any heap value passed to an external function is treated as escaping
- Intra-procedural + one-level is the precision boundary — no full inter-procedural analysis

### Generics Monomorphization
- Full monomorphization: generate a separate C struct/function for each concrete type instantiation (Iron_List_Int, Iron_List_String, etc.)
- Single output file: all modules compiled into one .c file, so deduplication is "don't emit the same instantiation twice"
- Interface constraints enforced: `List[T: Comparable]` validated at semantic analysis time with clear error messages
- Type suffix mangling: `Iron_List_Int_push()`, `Iron_Map_String_Int_get()` — readable C names using concrete type names

### C Symbol Naming / Mangling
- Iron_ prefix on EVERYTHING: user types, methods, functions, globals — `Iron_Player`, `Iron_Entity`, `Iron_Vec2`
- Method naming: `Iron_Type_method` pattern — `Iron_Player_update(Iron_Player* self, double dt)`
- Nested/anonymous scopes: parent context suffix — `Iron_Player_update_lambda_0`, `Iron_main_lambda_1` for debuggability
- Entry point: real C `main()` calls `Iron_main()` — allows runtime initialization before user code
- Generated C compiles with `clang/gcc -std=c11 -Wall -Werror` (from PROJECT.md constraint)

### Type Checking Strictness
- No implicit numeric conversions: all widening/narrowing requires explicit cast — `Int64(myInt32)` — matches "no implicit conversions" philosophy
- Integer literals default to `Int` (int64_t): `val x = 42` is Int. Float literals default to `Float` (double): `val y = 3.14` is Float
- Flow-sensitive nullable narrowing: `if x == null { return } x.foo()` works — type narrows after any null check including early returns
- `is` keyword narrows type in block: `if entity is Player { entity.name }` — entity becomes Player inside the block, no explicit cast needed
- val immutability enforced: reassignment of val produces compile error (from SEM-05)

### Carried Forward from Phase 1
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

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Language Specification
- `docs/language_definition.md` — Complete type system, nullable rules, inheritance, interfaces, generics, memory keywords, concurrency
- `docs/implementation_plan.md` — Phase 3 (semantic analysis sub-passes), Phase 4 (C code generation mappings, defer implementation, inheritance, type mapping table)

### Research
- `.planning/research/ARCHITECTURE.md` — Compiler pipeline, annotated AST as IR, comptime placement between semantic and codegen
- `.planning/research/PITFALLS.md` — C naming collisions, defer multi-exit traversal, monomorphization deduplication, lambda closure escaping
- `.planning/research/STACK.md` — Arena allocation, stb_ds for hash maps

### Phase 1 Code (MUST READ before implementing)
- `src/parser/ast.h` — All 55 AST node kinds, Iron_Visitor, iron_ast_walk, Iron_Span
- `src/parser/ast.c` — Walk dispatch over all node kinds
- `src/lexer/lexer.h` — Token types, Iron_Token struct
- `src/diagnostics/diagnostics.h` — Iron_DiagList, iron_diag_emit, E-code constants
- `src/util/arena.h` — Iron_Arena, ARENA_ALLOC macro

### Project Context
- `.planning/PROJECT.md` — Core value, constraints (C11, clang preferred, cross-platform)
- `.planning/REQUIREMENTS.md` — SEM-01..12, GEN-01..11, TEST-01, TEST-02 mapped to this phase
- `.planning/phases/01-frontend/01-CONTEXT.md` — Phase 1 decisions that established patterns

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `src/parser/ast.h` / `ast.c`: 55-kind AST with visitor pattern — semantic passes can use iron_ast_walk or direct switch
- `src/diagnostics/diagnostics.h`: Diagnostic system with E-codes, spans, and 3-line context — new semantic/codegen errors use same system
- `src/util/arena.h`: Arena allocator — use for symbol tables, type annotations, generated code strings
- `src/util/strbuf.h`: String builder — use for C code emission
- `src/vendor/stb_ds.h`: Hash map (sh_new_strdup, shget, shput) — use for symbol tables and type registries
- `src/parser/printer.c`: Pretty-printer using switch dispatch (430 lines) — reference for AST traversal patterns

### Established Patterns
- Iron_ prefix on all types and functions
- Arena allocation for all compiler data structures (no individual malloc/free)
- E0001-style error codes with Iron_Span source locations
- Unity test framework with CMake FetchContent
- ASan/UBSan enabled in debug builds
- bsearch for keyword lookup (sorted arrays)

### Integration Points
- Parser output (Iron_Program with list of top-level declarations) feeds into name resolution
- Annotated AST (after semantic analysis) feeds into C code generator
- C code generator outputs a single .c file
- Generated C invokes clang/gcc as subprocess to produce binary
- Diagnostic system shared across all compiler phases

</code_context>

<specifics>
## Specific Ideas

- The implementation plan's type mapping table (docs/implementation_plan.md, Phase 4a) is the authoritative Iron→C type mapping
- The implementation plan's C code emission order (Phase 4b) specifies: includes, forward decls, struct defs (topologically sorted), prototypes, implementations, main() wrapper
- The `draw {}` block is a game-dev specific construct that needs semantic handling — research flagged it as needing a decision
- ConstructExpr vs CallExpr disambiguation is a Phase 2 responsibility (Phase 1 deferred this explicitly)

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 02-semantics-and-codegen*
*Context gathered: 2025-03-25*
