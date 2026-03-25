# Architecture Research

**Domain:** Compiled programming language targeting C (transpiler + native binary)
**Researched:** 2026-03-25
**Confidence:** HIGH — patterns confirmed by Nim compiler internals docs, Zig architecture analysis, and established compiler theory

---

## Standard Architecture

### System Overview

```
┌────────────────────────────────────────────────────────────────────────┐
│                          FRONTEND (Source → AST)                        │
├─────────────────────────┬──────────────────────────────────────────────┤
│       Lexer             │             Parser                            │
│  .iron source text  ──► │  token stream ──► AST (unresolved)           │
│  token stream out       │  error recovery: continues past first error  │
└─────────────────────────┴──────────────────────────────────────────────┘
                                        │
                                        ▼
┌────────────────────────────────────────────────────────────────────────┐
│                    SEMANTIC ANALYSIS (AST → Annotated AST)              │
├───────────────────┬─────────────────────┬───────────────────┬──────────┤
│  Name Resolution  │   Type Checking     │  Escape Analysis  │ Concurrency│
│  scope tree       │   annotate every    │   heap/stack      │  checks   │
│  symbol tables    │   expr with type    │   auto-free marks │          │
└───────────────────┴─────────────────────┴───────────────────┴──────────┘
                                        │
                                        ▼ (only if no errors)
┌────────────────────────────────────────────────────────────────────────┐
│                   COMPTIME EVALUATOR (AST → AST with literals)          │
│  Mini interpreter runs comptime-marked call sites                       │
│  Replaces ComptimeExpr nodes with computed literal values               │
└────────────────────────────────────────────────────────────────────────┘
                                        │
                                        ▼
┌────────────────────────────────────────────────────────────────────────┐
│                      C CODE GENERATOR (AST → .c file)                   │
│  Walker emits: includes → forward decls → struct defs → protos → impls  │
│  Topological sort of struct definitions by dependency                   │
└────────────────────────────────────────────────────────────────────────┘
                                        │
                                        ▼
┌────────────────────────────────────────────────────────────────────────┐
│                      C COMPILER INVOCATION (clang/gcc)                  │
│  generated.c + iron_runtime.c + stdlib/*.c → linked binary             │
└────────────────────────────────────────────────────────────────────────┘
```

### Component Responsibilities

| Component | Responsibility | Input / Output |
|-----------|----------------|----------------|
| Lexer | Tokenize source text; track line/column per token | Source string → `Token[]` |
| Parser | Build unresolved AST via recursive descent; error recovery | `Token[]` → `AST` |
| Name Resolution | Build scope tree; resolve all identifiers to declarations | `AST` → `AST + SymbolTable[]` |
| Type Checker | Annotate every expression node with its type; enforce type rules | `AST + SymbolTable[]` → `AnnotatedAST` |
| Escape Analyzer | Mark heap allocations as escaping or non-escaping; emit warnings | `AnnotatedAST` → `AnnotatedAST + escape flags` |
| Concurrency Checker | Validate spawn captures, parallel-for mutation restrictions | `AnnotatedAST` → validated or error list |
| Comptime Evaluator | Interpret comptime call sites; fold to literals in AST | `AnnotatedAST` → `AnnotatedAST with literals` |
| C Code Generator | Walk annotated AST; emit correct C11 text | `AnnotatedAST` → `.c` file |
| CLI Driver | Orchestrate the full pipeline; invoke clang/gcc | flags + file paths → exit code |
| Runtime Library | C implementation of String, List, Map, Set, Rc, threading | Linked at binary build time |
| Standard Library | Iron-level wrappers for math, io, time, log modules | Linked at binary build time |

---

## Recommended Project Structure

```
iron-lang/
├── src/
│   ├── lexer/
│   │   ├── lexer.h          # Token type enum, Lexer struct, public API
│   │   └── lexer.c          # Tokenization logic, string/number literal handling
│   ├── parser/
│   │   ├── ast.h            # All AST node type definitions and union
│   │   ├── ast.c            # AST node constructors, memory management
│   │   ├── parser.h         # Parser struct, public parse() API
│   │   └── parser.c         # Recursive descent implementation
│   ├── analyzer/
│   │   ├── scope.h          # Scope and SymbolTable structs
│   │   ├── scope.c          # Scope tree construction, lookup
│   │   ├── resolve.h        # Name resolution pass
│   │   ├── resolve.c
│   │   ├── typecheck.h      # Type checking and type annotation pass
│   │   ├── typecheck.c
│   │   ├── escape.h         # Escape analysis pass
│   │   ├── escape.c
│   │   └── concurrency.h/c  # Parallel-for and spawn checks
│   ├── comptime/
│   │   ├── eval.h           # Comptime interpreter public API
│   │   └── eval.c           # AST evaluator for pure functions
│   ├── codegen/
│   │   ├── codegen.h        # Public generate() API
│   │   ├── codegen.c        # Main walker, output buffer management
│   │   ├── types.c          # Iron type → C type mapping
│   │   ├── stmts.c          # Statement emission (if, while, for, defer, etc.)
│   │   └── exprs.c          # Expression emission (calls, binops, heap, rc, etc.)
│   ├── runtime/
│   │   ├── iron_runtime.h   # Public runtime API
│   │   ├── iron_runtime.c   # String, List, Map, Set, Rc, Optional
│   │   └── iron_threads.c   # Pool, Channel, Mutex, Handle (pthreads)
│   ├── stdlib/
│   │   ├── iron_math.h/c
│   │   ├── iron_io.h/c
│   │   ├── iron_time.h/c
│   │   └── iron_log.h/c
│   ├── cli/
│   │   ├── main.c           # Entry point; parse CLI flags
│   │   ├── build.c          # iron build pipeline orchestration
│   │   ├── fmt.c            # iron fmt (AST pretty-printer)
│   │   └── diagnostics.c    # Rust-style error formatting with source snippets
│   └── util/
│       ├── arena.h/c        # Arena allocator for AST nodes
│       ├── strbuf.h/c       # String builder for C code emission
│       └── vec.h/c          # Generic dynamic array (used throughout)
├── tests/
│   ├── lexer/
│   ├── parser/
│   ├── analyzer/
│   ├── codegen/
│   └── integration/         # .iron files compiled and run end-to-end
└── docs/
```

### Structure Rationale

- **analyzer/ subdivided by pass:** Name resolution, type checking, escape, and concurrency are separate passes. Each pass reads the output of the previous one and adds annotations. This makes each checkable in isolation and allows skipping later passes when earlier ones find errors.
- **comptime/ isolated:** The comptime evaluator is a full mini interpreter. It is a distinct subsystem, not interleaved with codegen, which keeps codegen's job simple: always just walk a fully resolved, literal-substituted AST.
- **codegen/ split by concern:** `types.c`, `stmts.c`, and `exprs.c` are large independently. Splitting them keeps each file manageable and maps to distinct sections of the C output spec.
- **runtime/ separated from stdlib/:** The runtime (`iron_runtime`) is always linked; it provides language-level support (String, Rc, threads). The stdlib modules (`iron_math`, etc.) are only linked when imported. This keeps binary size minimal for programs that do not use them.
- **util/arena:** An arena allocator that frees all AST memory in one call after codegen completes is the standard pattern for compilers. Individual `free()` calls on thousands of AST nodes are expensive and error-prone.

---

## Architectural Patterns

### Pattern 1: Multi-Pass Pipeline with Strict Error Gating

**What:** Each phase runs to completion (collecting all errors within that phase) before the next phase starts. If any phase produces errors, later phases do not run.

**When to use:** Always. This is the standard compiler architecture. Semantic analysis should not attempt to run on an unparsed file. Codegen should not run on a type-error-containing AST.

**Trade-offs:** Slightly more setup than a single-pass design, but enables much better error messages because each pass can report all errors in its domain rather than aborting at the first one.

**Example (pipeline in main):**
```c
LexResult lex = lexer_run(source);
if (lex.error_count > 0) { report_and_exit(lex.errors); }

ParseResult parse = parser_run(lex.tokens);
if (parse.error_count > 0) { report_and_exit(parse.errors); }

AnalysisResult analysis = analyzer_run(parse.ast);
if (analysis.error_count > 0) { report_and_exit(analysis.errors); }

// Only emit code when the AST is known-clean
codegen_emit(analysis.ast, output_file);
```

### Pattern 2: Annotated AST as the Intermediate Representation

**What:** Rather than lowering to an intermediate representation (IR) like LLVM IR, the annotated AST is the single shared representation used by all compiler phases. Each phase adds information to existing nodes (type fields, resolved symbol pointers, escape flags) rather than producing a new data structure.

**When to use:** Appropriate for a compiler targeting a high-level language like C, where the target preserves enough structure to represent most Iron constructs directly. Avoids the complexity of designing and maintaining a separate IR.

**Trade-offs:** Works well for a C-targeting compiler. Would not scale to a machine-code compiler that needs register allocation and instruction selection. For Iron's scope this is the right call — it is what Nim uses for its C backend.

**C struct example:**
```c
typedef struct Expr {
    ExprKind kind;
    // set by parser:
    SourceLocation loc;
    // set by type checker:
    Type *resolved_type;
    // set by escape analyzer:
    bool heap_escapes;
    union {
        IntLiteralExpr int_lit;
        CallExpr call;
        BinaryExpr binary;
        HeapExpr heap;
        // ... etc
    };
} Expr;
```

### Pattern 3: Recursive Descent Parser (Hand-Written)

**What:** Each grammar rule maps to a C function. Functions call each other recursively, consuming tokens from the lexer one at a time.

**When to use:** For a language of Iron's complexity this is the standard approach. GCC and Clang both use hand-written recursive descent parsers. Parser generator tools (yacc, bison) produce harder-to-understand error messages and less flexible error recovery.

**Trade-offs:** More code than a generated parser, but much better control over error messages, error recovery, and grammar evolution.

**Error recovery:** The parser should use synchronization tokens (`;`, `}`, end of line after a newline-sensitive construct) to skip past a broken statement and continue parsing the rest of the file. This allows reporting multiple errors per file instead of stopping at the first.

### Pattern 4: Scope Tree as a Parent-Pointer Tree

**What:** Each scope has a pointer to its enclosing scope. Lookup walks up the chain. When a block ends, the local scope is simply popped; its memory can be freed or arenaed.

**When to use:** All multi-pass compilers. The scope tree must survive beyond the parsing phase so that semantic passes can query it.

```c
typedef struct Scope {
    struct Scope *parent;  // enclosing scope; NULL at global scope
    HashMap *symbols;      // name → Symbol*
} Scope;

Symbol *scope_lookup(Scope *s, const char *name) {
    while (s != NULL) {
        Symbol *sym = hashmap_get(s->symbols, name);
        if (sym) return sym;
        s = s->parent;
    }
    return NULL;  // undefined
}
```

### Pattern 5: Defer via Deferred-Statement Stack in Codegen

**What:** The C code generator maintains a per-scope stack of deferred statements. At every scope exit (end of block, every `return` statement), the stack is drained in reverse order before emitting the exit.

**When to use:** Required for correct `defer` semantics. Go, Zig, and C3 all implement defer this way in their compilers.

**Trade-offs:** The tricky case is `return` inside a deeply nested block — the codegen must emit all enclosing defers in order from innermost to outermost before emitting the return.

```c
// Generated C for:
//   val f = open("x"); defer close(f)
//   val g = open("y"); defer close(g)
//   return result
//
// Emitted as:
File f = open("x");
File g = open("y");
int result = compute();
close(g);   // defer 2, reverse order
close(f);   // defer 1
return result;
```

### Pattern 6: Topological Sort for Struct Emission Order

**What:** Before emitting struct definitions, build a dependency graph (struct A contains struct B → A depends on B). Emit in topological order. Circular dependencies (impossible in Iron due to no pointers between objects of equal types) cause a compile error.

**When to use:** Required. C requires types to be defined before use in struct fields. Emitting in declaration order from the source file is incorrect whenever type B appears before type A in source but A's definition contains B.

**Trade-offs:** Adds a dependency analysis step in codegen. Straightforward DFS implementation.

---

## Data Flow

### Full Compilation Pipeline

```
.iron source files (N files)
    │
    ▼ Lexer (per file)
Token streams (one per file)
    │
    ▼ Parser (per file, in parallel if desired)
AST forest (one AST per file, imports unresolved)
    │
    ▼ Module Graph Builder
Module dependency order (topological sort of imports)
    │
    ▼ Name Resolution (all modules, respecting import order)
AST + symbol tables (identifiers resolved to declarations)
    │
    ▼ Type Checker (all modules)
Annotated AST (every expression has a resolved_type)
    │
    ▼ Escape Analyzer
Annotated AST + escape flags (heap nodes marked escaping/non-escaping)
    │
    ▼ Concurrency Checker
Validated AST (or error list)
    │
    ▼ Comptime Evaluator
AST with ComptimeExpr nodes replaced by concrete literals
    │
    ▼ C Code Generator
Single generated.c (all modules inlined)
    │
    ▼ clang / gcc
    │  -std=c11 -O2 generated.c iron_runtime.c stdlib/*.c -lpthread
    ▼
Standalone native binary
```

### How Iron's Specific Features Affect Data Flow

| Feature | Where Handled | Data Flow Impact |
|---------|--------------|-----------------|
| `rc T` | Type checker + codegen | Type checker marks `rc` types; codegen wraps in `Iron_Rc_T` struct with retain on assignment, release on scope exit |
| `defer` | Codegen | Codegen maintains a deferred-statement stack per scope. Every return/block-end drains the stack before exiting |
| `heap` / `free` / `leak` | Escape analyzer + codegen | Escape analyzer decides auto-free or warn; codegen emits `malloc`, `free`, or nothing based on escape flags |
| `spawn` / `await` | Codegen | Spawn body is lifted into a standalone C function; `pool_submit(pool, fn, args)` is emitted; `await handle` emits `Iron_handle_wait(&h)` |
| `parallel for` | Semantic analyzer + codegen | Semantic pass forbids outer-variable mutation; codegen splits range into chunks and emits `pool_submit` + barrier |
| `comptime` | Comptime evaluator (own phase) | Runs between semantic analysis and codegen; replaces AST nodes with literals so codegen sees no comptime nodes at all |
| `is` type check | Type checker + codegen | Type checker validates legality; codegen emits a tag comparison if the type hierarchy uses tagged unions, or pointer comparison for interface dispatch |
| Interface dispatch | Codegen | Each interface becomes a vtable struct in C. Objects implementing an interface have a pointer to a statically allocated vtable. `item.draw()` emits `item.__vtable->draw(item)` |
| String interpolation | Codegen (expr emission) | Interpolated segments are emitted as a series of `Iron_String_concat` or `snprintf` calls at the expression site |
| Nullable `T?` | Codegen + type checker | Every `T?` becomes `Iron_Optional_T { T value; bool has_value; }`. Null checks narrow the type in the annotated AST; codegen emits `.has_value` guards |

---

## Runtime Integration

The runtime (`iron_runtime.c`) is statically linked into every binary. The generated C file `#include`s `iron_runtime.h` at the top. The codegen always emits this include — it is unconditional.

```
generated.c
    #include "iron_runtime.h"   ← always included
    #include "iron_math.h"      ← only if `import math` present
    #include "iron_io.h"        ← only if `import io` present
    ...

Compile command:
    clang -std=c11 -O2 \
        -o output \
        generated.c \
        iron_runtime.c \
        iron_math.c \       ← only if imported
        iron_io.c \         ← only if imported
        -lpthread
```

The runtime is **not** a separate shared library. There is no `.so` or `.dll` for the Iron runtime. It compiles into the final binary, keeping the output self-contained.

Raylib is the exception: it is **dynamically linked** (`-lraylib`) because game binaries conventionally use the platform's installed graphics library.

---

## Build Order

The component dependency graph determines what must exist before each phase can be built and tested:

```
1. util/ (arena, strbuf, vec)   — no dependencies
         │
2. lexer/                       — depends on util/
         │
3. parser/ast                   — depends on util/
         │
4. parser/parser                — depends on lexer, ast
         │
5. analyzer/scope               — depends on ast
         │
6. analyzer/resolve             — depends on scope, ast
         │
7. analyzer/typecheck           — depends on resolve, ast
         │
8. analyzer/escape              — depends on typecheck
         │
9. analyzer/concurrency         — depends on typecheck
         │
10. comptime/eval               — depends on typecheck (needs resolved types)
         │
11. codegen/                    — depends on all analyzer passes + comptime
         │
12. runtime/                    — independent of compiler (pure C library)
         │
13. stdlib/                     — depends on runtime
         │
14. cli/                        — depends on all of the above
```

**Suggested phase build order for the roadmap:**
1. Lexer (standalone, highly testable in isolation)
2. Parser + AST (builds on lexer; produces the core data structure all later phases share)
3. Semantic analysis (name resolution → type checking → escape → concurrency checks; each sub-pass added incrementally)
4. C codegen (can start with a minimal subset — literals and functions — and expand feature by feature)
5. Runtime library (can be developed in parallel with codegen; needed before end-to-end tests work)
6. Standard library modules (built after runtime)
7. CLI toolchain (wraps everything; can provide a minimal `iron build` early and expand commands)
8. Comptime evaluator (requires a fully working semantic pass; added last because it is optional for correctness)

Phases 5 and 6 are largely independent of the compiler front-end and can be developed concurrently.

---

## Anti-Patterns

### Anti-Pattern 1: Interleaving Parsing and Semantic Analysis

**What people do:** Check types or resolve names inside the parser as tokens are consumed.

**Why it's wrong:** Makes error recovery nearly impossible. A name that is forward-referenced cannot be resolved during parsing. Iron's method declarations (`func Player.update(...)`) are defined outside the `object` block and appear after object fields — resolving method-to-type links requires a full symbol table built after parsing.

**Do this instead:** Full separation of parsing (builds AST) and semantic analysis (validates and annotates AST). This is confirmed correct by both Nim and Clang internals.

### Anti-Pattern 2: Generating C Code Directly from the Parser

**What people do:** Emit C output as tokens are parsed, without building an AST first.

**Why it's wrong:** Defer requires knowing all defers in a scope before emitting the first one (reverse order at every return). Struct emission requires topological sort across all types. String interning requires a complete view of all literal strings. None of these are possible in a single-pass codegen.

**Do this instead:** Build a complete AST, run all analysis passes, then walk the final AST to emit C.

### Anti-Pattern 3: One Giant C File for the Compiler

**What people do:** Write the entire compiler as a single `.c` file, monolithically.

**Why it's wrong:** While single-file approaches are fine for tiny tools, a compiler with 8 distinct phases will be 10,000–30,000 lines. A single file makes it impossible to test phases independently and causes full rebuilds on every edit.

**Do this instead:** One `.c` / `.h` pair per phase (lexer, parser, resolver, typecheck, codegen, etc.). Each header defines the public API; the `.c` file is the implementation. This matches the structure Nim uses and is the standard pattern for multi-phase compilers in C.

### Anti-Pattern 4: Calling `free()` on Every AST Node Individually

**What people do:** Track every AST allocation and `free()` each one after codegen.

**Why it's wrong:** Thousands of `free()` calls for what amounts to a single compilation adds measurable overhead and is a common source of double-free bugs in compilers.

**Do this instead:** Use an arena allocator. All AST nodes are allocated from the arena. After codegen completes, call `arena_free(&arena)` once. This is the standard pattern in production compilers (Clang, TCC, Zig's bootstrap compiler).

### Anti-Pattern 5: Failing on the First Error

**What people do:** Return an error code from the parser or type checker at the first failure point.

**Why it's wrong:** Forces users to fix one error, recompile, find the next, repeat. The first five lines of an Iron file might have five distinct type errors. Reporting one at a time is a poor developer experience.

**Do this instead:** Accumulate errors in a `DiagnosticList`. Each phase runs to completion within its domain. The pipeline stops at phase boundaries, not mid-phase. This is the model used by GCC, Clang, and Rust.

---

## Integration Points

### External Services

| Service | Integration Pattern | Notes |
|---------|---------------------|-------|
| clang / gcc | CLI subprocess invocation | `execvp()` or `system()` with constructed argument list; clang preferred, gcc fallback detected at configure time |
| Raylib | Dynamic link at binary build time | `-lraylib` passed to the final clang/gcc invocation; never linked into the Iron compiler itself |
| pthreads | Linked into the runtime | `iron_runtime.c` uses `#include <pthread.h>`; the final binary always links `-lpthread` when any concurrency features are used |

### Internal Boundaries

| Boundary | Communication | Notes |
|----------|---------------|-------|
| Lexer → Parser | `Token *` array (or iterator) | Parser owns lifetime; lexer can be freed after parsing |
| Parser → Analyzer | `ASTNode *` root (tree of nodes in arena) | Analyzer adds fields to existing nodes; does not copy the tree |
| Analyzer → Codegen | `ASTNode *` root (fully annotated) | Codegen reads `resolved_type` and `heap_escapes` fields set by analyzer |
| Codegen → C compiler | Write `.c` file to temp dir; invoke subprocess | The generated `.c` file is the handoff point; can be inspected with `--verbose` |
| CLI → all phases | Function calls within same process | All phases run in-process; no inter-process communication within the Iron compiler itself |
| Runtime → generated code | `#include "iron_runtime.h"` | Runtime types and functions are referenced by name in generated C; the compiler knows the runtime API and emits calls matching it |

---

## Sources

- [Nim Compiler Internals](https://nim-lang.org/docs/intern.html) — confirmed multi-pass architecture, AST as IR, C backend structure (HIGH confidence, official docs)
- [Zig Self-Hosted Compiler Architecture](https://kristoff.it/blog/zig-self-hosted-now-what/) — confirmed comptime implementation as a separate evaluator phase, ZIR → AIR pipeline (HIGH confidence, official blog)
- [Zig comptime documentation](https://zig.guide/language-basics/comptime/) — confirmed comptime approach: evaluate at compile time, embed result as literal (HIGH confidence, official docs)
- [Recursive Descent Parsers — GCC and Clang](https://en.wikipedia.org/wiki/Recursive_descent_parser) — confirmed both GCC (since 2004) and Clang use hand-written recursive descent parsers (HIGH confidence, verified)
- [A new constant expression interpreter for Clang](https://developers.redhat.com/articles/2024/10/21/new-constant-expression-interpreter-clang) — confirmed two comptime patterns: recursive AST evaluation vs bytecode interpreter; recursive AST eval is simpler to build first (HIGH confidence, official Red Hat/LLVM source)
- [Defer reference implementations](https://btmc.substack.com/p/implementing-scoped-defer-in-c) — confirmed LIFO defer execution and scope-exit emission pattern (MEDIUM confidence, community)
- [Topological sort in compiler design](https://medium.com/@23bt04151/topological-sort-in-compiler-design-ordering-compilation-of-dependent-files-e7f79015212b) — confirmed topological sort for dependency-ordered code emission (MEDIUM confidence, community)
- [Designing thread-safe reference counting in C](https://www.codeproject.com/Articles/1192293/Designing-Efficient-Thread-Safe-Reference-Counting) — confirmed atomic retain/release pattern for rc (MEDIUM confidence)
- [Escape analysis — Wikipedia](https://en.wikipedia.org/wiki/Escape_analysis) — confirmed conservative algorithm: prove local → stack, cannot prove → heap / warn (HIGH confidence)

---

*Architecture research for: Iron — compiled language targeting C*
*Researched: 2026-03-25*
