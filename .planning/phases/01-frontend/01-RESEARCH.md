# Phase 1: Frontend - Research

**Researched:** 2026-03-25
**Domain:** C compiler frontend — hand-written lexer, recursive descent parser, arena-allocated AST, Rust-style diagnostics
**Confidence:** HIGH — all major decisions locked by CONTEXT.md; patterns verified against production compilers (Clang, GCC, chibicc, Nim)

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Error Diagnostic Format**
- 3-line context window: line above, error line, line below (Rust-style default)
- Fix suggestions included: "did you mean X?" when the fix is obvious
- One block per error: each error gets its own source snippet and message
- Error codes: E0001-style unique codes on every diagnostic (enables --explain flag and documentation later)
- Colored terminal output for errors (already decided in PROJECT.md)

**Token and AST Design**
- Struct per node type: each AST node is its own C struct, linked via pointers (not tagged unions)
- Strings copied into arena: identifiers and literals are copied from source buffer into the arena allocator — source can be freed after parsing
- Iron_ prefix naming convention: Iron_IfStmt, Iron_FuncDecl, iron_parse_expr() — consistent with runtime naming
- Visitor pattern with callbacks: generic AST walk function that calls visitor struct methods per node type — reusable across semantic passes
- Arena allocator for all AST nodes (research recommendation, confirmed)

**Test File Format**
- Separate .expected files: tests/hello.iron + tests/hello.expected for integration tests
- Error diagnostic tests check error code + line number only (not exact text) — less brittle, message wording can evolve
- C test harness: test runner built in C alongside the compiler — no external dependency
- Four test categories in Phase 1: lexer token tests, parser AST tests, error message tests, round-trip tests (parse then pretty-print)

**Project Scaffolding**
- Follow implementation plan directory layout: src/lexer/, src/parser/, src/analyzer/, src/codegen/, src/runtime/, src/stdlib/, src/cli/ + tests/
- Move existing docs/ (language_definition.md, implementation_plan.md) into .planning/
- CMake + Ninja build system (research recommendation)
- Unity test framework for C unit tests (research recommendation)
- Defer CI (GitHub Actions) to Phase 3
- Include example .iron files (examples/hello.iron, examples/game.iron) as parser test inputs

### Claude's Discretion
- Exact arena allocator implementation details
- CMake configuration specifics
- Token enum ordering and internal structure
- Error message wording and suggestion heuristics
- AST visitor callback function signature design

### Deferred Ideas (OUT OF SCOPE)
None — discussion stayed within phase scope
</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| LEX-01 | Compiler tokenizes all Iron keywords, operators, literals, and delimiters | Token table in implementation_plan.md is authoritative; 37 keywords + full operator/delimiter set documented below |
| LEX-02 | Every token carries source span (file, line, column) for diagnostics | Iron_Span struct design; must be on every token from the first commit |
| LEX-03 | Lexer reports errors for unterminated strings and invalid characters with location | Error recovery: collect errors, continue tokenizing; use Iron_Diagnostic with E0001-style codes |
| LEX-04 | Comments (-- to end of line) are recognized and skipped | Single-token COMMENT type; skip in lexer, not exposed to parser |
| PARSE-01 | Recursive descent parser produces complete AST for all Iron syntax | Full node type table in implementation_plan.md Phase 2; grammar priorities documented below |
| PARSE-02 | Parser recovers from errors and reports multiple diagnostics per file | ErrorNode pattern; synchronization tokens; exactly N errors for N independent mistakes |
| PARSE-03 | String interpolation segments are parsed into AST nodes | Interpolation handled inside string literal scanning; produces InterpolatedStringExpr with segment list |
| PARSE-04 | Operator precedence is correctly handled for all binary/unary operators | Full precedence table documented below; Pratt parser for expressions |
| PARSE-05 | AST pretty-printer can dump tree back to readable Iron for debugging | Visitor pattern makes this natural; round-trip testing validates parser correctness |
| TEST-03 | Error diagnostic tests verify specific error messages for specific mistakes | .iron + .expected file pairs; check error code + line number only |
</phase_requirements>

---

## Summary

Phase 1 builds the compiler frontend from scratch: a hand-written lexer that tokenizes Iron source into a typed token stream with full source spans, and a recursive descent parser that produces a complete, span-annotated AST. The phase also establishes the foundational infrastructure — arena allocator, error diagnostic system, visitor pattern, CMake build — that all subsequent phases depend on.

The critical architectural decisions are already locked: arena allocation, struct-per-node AST, Iron_ naming prefix, error codes, Unity test framework, CMake + Ninja. Research focus is on the specific implementation details within those locked decisions: exact token enum design, operator precedence table, string interpolation parsing strategy, error recovery synchronization points, and the visitor callback signature.

The biggest risk in Phase 1 is getting the foundational contracts wrong — span tracking, arena API, visitor signature, and error code scheme — because these are consumed by every subsequent phase. Design these interfaces more carefully than the implementation. The implementation can be replaced; the interfaces become load-bearing immediately.

**Primary recommendation:** Implement in build order — util/arena first, then lexer, then parser/ast, then parser/parser. Each layer is independently testable. Establish Iron_Span on every token and node from commit 1; retrofitting span tracking is the most common Phase 1 mistake in compiler implementations.

---

## Standard Stack

### Core

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| C11 | ISO/IEC 9899:2011 | Compiler implementation language | Mandated. `stdbool.h`, `stdint.h`, `_Static_assert`, `_Generic` all available. Sufficient for clean compiler code. |
| CMake | 3.25+ | Build system generator | Cross-platform (macOS/Linux/Windows). Generates Ninja files. `SYSTEM` keyword for clean third-party includes. `FetchContent` for Unity. |
| Ninja | 1.11+ | CMake backend | 10-50x faster no-op rebuilds vs GNU Make. Use as default `-G Ninja` backend in dev. |
| Unity (ThrowTheSwitch) | v2.6.1 | C unit test framework | Two headers + one .c file. Zero configuration. Works with GCC, Clang, MSVC. Full assertion macro set for strings, integers, byte arrays. |

### Supporting

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| stb_ds.h | master (public domain) | Dynamic arrays and hash maps | String interning table; token list; any dynamic collection inside the compiler. Single header, vendored into `src/vendor/`. |
| Hand-written arena allocator | custom (~100 lines) | Pool allocator for all AST nodes and token strings | Arena-allocate everything in the compilation unit; single `arena_free()` at the end. Standard pattern from Clang, TCC, chibicc. |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Hand-written recursive descent | Flex + Bison / ANTLR | Parser generators produce poor error messages, shift/reduce conflicts are opaque, less control over recovery. Clang, GCC, Go all use hand-written parsers. Locked decision. |
| Unity | Criterion | Criterion adds shared library linking complexity. Unity is simpler for cross-platform C projects. Locked decision. |
| Custom arena | `malloc` everywhere | Per-node malloc/free is slow and leak-prone for thousands of same-lifetime allocations. Arena is the standard compiler pattern. Locked decision. |

**Installation:**
```bash
# macOS
brew install cmake ninja llvm

# Linux (Ubuntu/Debian)
apt install cmake ninja-build clang

# Unity via CMake FetchContent (add to CMakeLists.txt):
include(FetchContent)
FetchContent_Declare(
  Unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG        v2.6.1
)
FetchContent_MakeAvailable(Unity)
```

---

## Architecture Patterns

### Recommended Project Structure

The CONTEXT.md mandates the implementation_plan directory layout. For Phase 1 specifically:

```
iron-lang/
├── CMakeLists.txt               # Root build file
├── src/
│   ├── util/
│   │   ├── arena.h              # Iron_Arena struct + API
│   │   ├── arena.c              # Bump-pointer allocator (~100 lines)
│   │   ├── strbuf.h             # String builder (for pretty-printer output)
│   │   └── strbuf.c
│   ├── lexer/
│   │   ├── lexer.h              # Iron_Token, Iron_TokenKind enum, Iron_Lexer, iron_lex()
│   │   └── lexer.c              # Tokenization, string/number scanning, error collection
│   ├── parser/
│   │   ├── ast.h                # Iron_Span, all AST node structs, Iron_Node base
│   │   ├── ast.c                # Node constructors (arena-allocated)
│   │   ├── parser.h             # Iron_Parser struct, iron_parse() API
│   │   └── parser.c             # Recursive descent, error recovery, visitor
│   ├── diagnostics/
│   │   ├── diagnostics.h        # Iron_Diagnostic, Iron_DiagList, error codes
│   │   └── diagnostics.c        # Rust-style formatting: snippet + arrow + suggestion
│   └── vendor/
│       └── stb_ds.h             # Vendored, no modifications
├── tests/
│   ├── lexer/
│   │   ├── test_keywords.c      # Unity tests: token type per keyword
│   │   ├── test_literals.c      # Unity tests: int, float, string, bool, null
│   │   ├── test_operators.c     # Unity tests: all operator tokens
│   │   └── test_errors.c        # Unity tests: unterminated strings, invalid chars
│   ├── parser/
│   │   ├── test_decls.c         # Unity tests: object, func, interface, enum decls
│   │   ├── test_stmts.c         # Unity tests: if/elif/else, while, for, match, defer
│   │   ├── test_exprs.c         # Unity tests: operator precedence, calls, indexing
│   │   ├── test_interp.c        # Unity tests: string interpolation segments
│   │   └── test_errors.c        # Unity tests: error count, error codes, line numbers
│   └── integration/
│       ├── hello.iron
│       ├── hello.expected
│       ├── game.iron            # From README/examples
│       └── game.expected
└── examples/
    ├── hello.iron
    └── game.iron
```

### Pattern 1: Arena Allocator

**What:** A single arena is created per compilation unit. All AST nodes, copied strings, and token strings are allocated from it. The arena is freed once after the entire pipeline completes.

**When to use:** All AST and token string allocations. Never `malloc` individual AST nodes.

**Implementation (Claude's discretion area):**
```c
// src/util/arena.h
typedef struct {
    uint8_t *base;
    size_t   used;
    size_t   capacity;
} Iron_Arena;

Iron_Arena  iron_arena_create(size_t capacity);
void       *iron_arena_alloc(Iron_Arena *a, size_t size, size_t align);
char       *iron_arena_strdup(Iron_Arena *a, const char *src, size_t len);
void        iron_arena_free(Iron_Arena *a);

// Convenience macro
#define ARENA_ALLOC(arena, T) \
    ((T*)iron_arena_alloc((arena), sizeof(T), _Alignof(T)))
```

**Key constraint:** Strings copied into arena (locked decision). The source buffer can be freed after parsing. Every `Iron_Token.value` and `Iron_StringLiteral.value` is a pointer into arena memory, not into the source buffer.

### Pattern 2: Token Design

**What:** A flat array of `Iron_Token` structs produced by the lexer. The parser walks this array by index (or via a cursor pointer). No heap allocation per token — all tokens live in an arena-backed array.

**Token struct:**
```c
// src/lexer/lexer.h
typedef enum {
    // Literals
    IRON_TOK_INTEGER,
    IRON_TOK_FLOAT,
    IRON_TOK_STRING,
    IRON_TOK_BOOL,
    IRON_TOK_NULL,

    // Keywords (in keyword-table order)
    IRON_TOK_VAL,
    IRON_TOK_VAR,
    IRON_TOK_FUNC,
    IRON_TOK_OBJECT,
    IRON_TOK_ENUM,
    IRON_TOK_INTERFACE,
    IRON_TOK_IMPORT,
    IRON_TOK_IF,
    IRON_TOK_ELIF,
    IRON_TOK_ELSE,
    IRON_TOK_FOR,
    IRON_TOK_WHILE,
    IRON_TOK_MATCH,
    IRON_TOK_RETURN,
    IRON_TOK_HEAP,
    IRON_TOK_FREE,
    IRON_TOK_LEAK,
    IRON_TOK_DEFER,
    IRON_TOK_RC,
    IRON_TOK_PRIVATE,
    IRON_TOK_EXTENDS,
    IRON_TOK_IMPLEMENTS,
    IRON_TOK_SUPER,
    IRON_TOK_IS,
    IRON_TOK_SPAWN,
    IRON_TOK_AWAIT,
    IRON_TOK_PARALLEL,
    IRON_TOK_POOL,
    IRON_TOK_NOT,
    IRON_TOK_AND,
    IRON_TOK_OR,
    IRON_TOK_SELF,
    IRON_TOK_COMPTIME,
    IRON_TOK_IN,
    IRON_TOK_TRUE,
    IRON_TOK_FALSE,
    IRON_TOK_NULL_KW,

    // Operators
    IRON_TOK_PLUS,
    IRON_TOK_MINUS,
    IRON_TOK_STAR,
    IRON_TOK_SLASH,
    IRON_TOK_PERCENT,
    IRON_TOK_ASSIGN,
    IRON_TOK_EQUALS,
    IRON_TOK_NOT_EQUALS,
    IRON_TOK_LESS,
    IRON_TOK_GREATER,
    IRON_TOK_LESS_EQ,
    IRON_TOK_GREATER_EQ,
    IRON_TOK_DOT,
    IRON_TOK_DOTDOT,
    IRON_TOK_COMMA,
    IRON_TOK_COLON,
    IRON_TOK_ARROW,
    IRON_TOK_QUESTION,
    IRON_TOK_MINUS_MINUS,   // for comment detection

    // Delimiters
    IRON_TOK_LPAREN,
    IRON_TOK_RPAREN,
    IRON_TOK_LBRACKET,
    IRON_TOK_RBRACKET,
    IRON_TOK_LBRACE,
    IRON_TOK_RBRACE,
    IRON_TOK_SEMICOLON,

    // Special
    IRON_TOK_NEWLINE,
    IRON_TOK_EOF,
    IRON_TOK_ERROR,         // lexer error token
    IRON_TOK_IDENTIFIER,
} Iron_TokenKind;

typedef struct {
    Iron_TokenKind  kind;
    const char     *value;   // arena-copied; NULL for punctuation
    uint32_t        line;
    uint32_t        col;
    uint32_t        len;     // byte length of token in source
} Iron_Token;
```

**Keyword recognition:** Use a sorted table + binary search, or a hash map (stb_ds). Do not use a chain of `strcmp` in a switch — unmaintainable as the keyword list grows to 37+ entries.

### Pattern 3: Source Span

**What:** Every token and every AST node carries a span identifying its location in the source. The span is the foundation for all error diagnostics.

**Design (Claude's discretion area):**
```c
// src/parser/ast.h
typedef struct {
    const char *filename;  // interned string, not per-span copy
    uint32_t    line;      // 1-indexed
    uint32_t    col;       // 1-indexed, byte column
    uint32_t    end_line;
    uint32_t    end_col;
} Iron_Span;
```

**Critical rule:** Iron_Span is on every AST node. There is no situation where a node lacks a span. Diagnostic code that receives a node always has a location to report. Establish this invariant at the type level: the `Iron_Node` base struct includes `Iron_Span span` as its first field.

### Pattern 4: AST Node Design (Struct per Node Type)

The CONTEXT.md locks struct-per-node-type (not tagged unions). Each node type is a distinct C struct. The approach enables clean field naming without union accessor noise.

**Base node pattern:**
```c
// All AST nodes embed Iron_Span as their first member.
// The visitor pattern uses function pointers keyed on a NodeKind tag.

typedef enum {
    IRON_NODE_PROGRAM,
    IRON_NODE_IMPORT_DECL,
    IRON_NODE_OBJECT_DECL,
    IRON_NODE_INTERFACE_DECL,
    IRON_NODE_ENUM_DECL,
    IRON_NODE_FUNC_DECL,
    IRON_NODE_METHOD_DECL,
    // statements
    IRON_NODE_VAL_DECL,
    IRON_NODE_VAR_DECL,
    IRON_NODE_ASSIGN,
    IRON_NODE_RETURN,
    IRON_NODE_IF,
    IRON_NODE_WHILE,
    IRON_NODE_FOR,
    IRON_NODE_MATCH,
    IRON_NODE_DEFER,
    IRON_NODE_FREE,
    IRON_NODE_LEAK,
    IRON_NODE_SPAWN,
    // expressions
    IRON_NODE_INT_LIT,
    IRON_NODE_FLOAT_LIT,
    IRON_NODE_STRING_LIT,
    IRON_NODE_INTERP_STRING,
    IRON_NODE_BOOL_LIT,
    IRON_NODE_NULL_LIT,
    IRON_NODE_IDENT,
    IRON_NODE_BINARY,
    IRON_NODE_UNARY,
    IRON_NODE_CALL,
    IRON_NODE_METHOD_CALL,
    IRON_NODE_FIELD_ACCESS,
    IRON_NODE_INDEX,
    IRON_NODE_SLICE,
    IRON_NODE_LAMBDA,
    IRON_NODE_HEAP,
    IRON_NODE_RC,
    IRON_NODE_COMPTIME,
    IRON_NODE_IS,
    IRON_NODE_CONSTRUCT,
    IRON_NODE_ARRAY_LIT,
    IRON_NODE_AWAIT,
    IRON_NODE_ERROR,         // error recovery node
} Iron_NodeKind;

// Concrete node example:
typedef struct {
    Iron_Span      span;   // ALWAYS FIRST
    Iron_NodeKind  kind;   // IRON_NODE_IF
    struct Iron_Node *condition;
    struct Iron_Node *body;        // block
    struct Iron_Node **elif_conds; // stb_ds array
    struct Iron_Node **elif_bodies;
    struct Iron_Node *else_body;   // NULL if absent
} Iron_IfStmt;

// All node pointers are typed as Iron_Node* in collections,
// with kind checked at visitor dispatch time.
typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;
} Iron_Node;  // base — cast to concrete type by kind
```

### Pattern 5: Operator Precedence Table

Iron's expression grammar requires a correct precedence table. Use a Pratt parser (top-down operator precedence) for expressions — cleaner than encoding precedence in recursive grammar rules.

**Precedence levels (highest to lowest):**

| Level | Operators | Associativity |
|-------|-----------|---------------|
| 10 (highest) | `.` (field access), `[...]` (index), `(...)` (call), `[..]` (slice) | Left |
| 9 | Unary: `-`, `not` | Right (prefix) |
| 8 | `*`, `/`, `%` | Left |
| 7 | `+`, `-` | Left |
| 6 | `<`, `>`, `<=`, `>=` | Left |
| 5 | `==`, `!=` | Left |
| 4 | `and` | Left |
| 3 | `or` | Left |
| 2 | `is` (type check) | Left |
| 1 (lowest) | Assignment `=` | Right |

**Special forms parsed as expressions:**
- `heap expr` — prefix keyword, parses like a unary operator
- `rc expr` — prefix keyword
- `comptime expr` — prefix keyword
- `await expr` — prefix keyword
- `func(...) { }` — lambda; parsed as an expression
- `spawn("name", pool?) { }` — parsed as a statement (not expression)

### Pattern 6: String Interpolation Parsing

**What:** Iron string literals with `{expr}` inside them require special handling in the lexer and parser.

**Strategy:** Handle in lexer. When the lexer encounters a `"` and then finds `{` inside the string, it does NOT produce a single STRING token. Instead:

1. Produce `INTERP_STRING_START` with the literal prefix before `{`
2. Lex the expression tokens normally until matching `}`
3. Produce `INTERP_STRING_MID` or `INTERP_STRING_END` with the next literal segment

Alternatively — and simpler for a first implementation — the lexer produces a single `STRING` token with the raw source text (including `{...}`), and the parser's string-handling code does a second pass to split segments. This avoids complex lexer state.

**Recommended approach for Phase 1 (Claude's discretion):** The lexer produces a special `IRON_TOK_INTERP_STRING` token whose `value` is the raw string content. The parser handles splitting into an `Iron_InterpStringExpr` with `Iron_Node **segments` (alternating literal string and expression nodes). This keeps the lexer simple and puts the complexity in the parser where it belongs.

```c
// Resulting AST node for: "Hello {name}, you have {hp} HP"
typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;   // IRON_NODE_INTERP_STRING
    // Alternating: Iron_StringLiteral, expr, Iron_StringLiteral, expr, ...
    Iron_Node   **parts;  // stb_ds array
    int           part_count;
} Iron_InterpStringExpr;
```

### Pattern 7: Error Recovery in the Parser

**What:** The parser must produce exactly N error messages for N independent syntax errors. It must not cascade. Use the `ErrorNode` approach: insert a sentinel node instead of aborting, then synchronize to a known-good position.

**Synchronization token sets:**
- **Top-level:** `func`, `object`, `interface`, `enum`, `import`, EOF
- **Statement-level:** next keyword (`if`, `for`, `while`, `val`, `var`, `return`, `defer`, `free`, `leak`, `spawn`), `}`, EOF
- **Expression-level:** `NEWLINE`, `;`, `)`, `]`, `}`, EOF

**Implementation:**
```c
// When the parser encounters an unexpected token:
// 1. Emit a diagnostic to the DiagList
// 2. Create an Iron_ErrorNode at the current span
// 3. Call iron_parser_sync(parser) to advance to next sync token
// 4. Return the ErrorNode (not NULL — NULL propagation causes crashes)

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  // IRON_NODE_ERROR
    // No children — ErrorNode is a leaf
} Iron_ErrorNode;

// The error count invariant: a file with N independent syntax errors
// (each in a separate top-level declaration or separate statement)
// produces exactly N diagnostic messages.
```

**Cascade suppression:** After inserting an ErrorNode and synchronizing, set a `in_error_recovery` flag. Suppress additional errors until the next successful parse step. This prevents one bad token from generating dozens of follow-on errors.

### Pattern 8: Diagnostic System

**What:** Every error or warning is an `Iron_Diagnostic` with a code, span, message, and optional suggestion. The diagnostic formatter produces Rust-style output with 3-line context.

**Error code scheme (E0001-style):**

| Range | Category |
|-------|----------|
| E0001–E0099 | Lexer errors |
| E0100–E0299 | Parse errors |
| E0300–E0499 | Semantic errors (Phase 2) |
| E0500–E0699 | Type errors (Phase 2) |
| E0700–E0899 | Warnings |

**Phase 1 error codes (reserved range):**

| Code | Trigger |
|------|---------|
| E0001 | Unterminated string literal |
| E0002 | Invalid character in source |
| E0003 | Invalid numeric literal |
| E0101 | Unexpected token |
| E0102 | Expected expression |
| E0103 | Expected `}` to close block |
| E0104 | Expected `)` to close argument list |
| E0105 | Expected `:` in type annotation |
| E0106 | Expected `->` in function return type |

```c
// src/diagnostics/diagnostics.h
typedef enum {
    IRON_DIAG_ERROR,
    IRON_DIAG_WARNING,
    IRON_DIAG_NOTE,
} Iron_DiagLevel;

typedef struct {
    Iron_DiagLevel  level;
    int             code;       // E-code number
    Iron_Span       span;
    const char     *message;    // arena-allocated
    const char     *suggestion; // NULL if none; "did you mean X?"
} Iron_Diagnostic;

typedef struct {
    Iron_Diagnostic *items;     // stb_ds dynamic array
    int              count;
    int              error_count;
} Iron_DiagList;

// Formatting: produces colored Rust-style output
void iron_diag_print(const Iron_Diagnostic *d, const char *source_text);
```

**3-line context window output format:**
```
error[E0001]: unterminated string literal
  --> main.iron:5:10
   |
 4 | val msg = greet("Victor")
 5 | val bad = "unterminated
   |           ^
 6 | val next = 42
   |
   = help: add a closing `"` at the end of the string
```

### Pattern 9: Visitor Pattern

**What:** A single `iron_ast_walk()` function that takes a root node and a `Iron_Visitor` struct with function pointer fields for each node kind. The pretty-printer, the semantic analysis passes, and the codegen all use this interface.

**Visitor signature design (Claude's discretion area):**
```c
// src/parser/ast.h

// Forward declaration
typedef struct Iron_Visitor Iron_Visitor;

// Per-node-kind callbacks return a bool (true = continue walk, false = stop)
// The visitor also has pre/post hooks for enter/exit of each node.
typedef struct Iron_Visitor {
    void *ctx;  // user data passed to all callbacks

    // Return false to stop descending into children of this node
    bool (*visit_if)(Iron_Visitor *v, Iron_IfStmt *node);
    bool (*visit_while)(Iron_Visitor *v, Iron_WhileStmt *node);
    bool (*visit_for)(Iron_Visitor *v, Iron_ForStmt *node);
    bool (*visit_func_decl)(Iron_Visitor *v, Iron_FuncDecl *node);
    bool (*visit_object_decl)(Iron_Visitor *v, Iron_ObjectDecl *node);
    bool (*visit_val_decl)(Iron_Visitor *v, Iron_ValDecl *node);
    bool (*visit_var_decl)(Iron_Visitor *v, Iron_VarDecl *node);
    bool (*visit_binary)(Iron_Visitor *v, Iron_BinaryExpr *node);
    bool (*visit_call)(Iron_Visitor *v, Iron_CallExpr *node);
    // ... one per node kind
    // NULL function pointer = default "continue walking children"
} Iron_Visitor;

void iron_ast_walk(Iron_Node *root, Iron_Visitor *v);
```

### Anti-Patterns to Avoid

- **Span added later:** Adding spans to nodes after the initial implementation requires touching every node type and every constructor. Add `Iron_Span` as the first field of every node struct from day 1.
- **Returning NULL on parse error:** NULL propagation in recursive descent causes segfaults deep in the call stack. Always return an `Iron_ErrorNode` — never NULL.
- **Keyword table as a chain of strcmp:** Unmaintainable with 37+ keywords. Use a sorted table with `bsearch()` or stb_ds hash map.
- **Source buffer aliasing in token values:** Token `value` pointers must point into arena memory, not source buffer memory. The lexer must copy strings into the arena during tokenization. Violating this causes use-after-free when the source buffer is freed.
- **Single error per file:** The parser must use error recovery from the start. Adding it after the fact is difficult. Implement `Iron_ErrorNode` and `iron_parser_sync()` in the first parser commit.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Dynamic arrays for token lists, node lists | Custom growable array | `stb_ds.h` arrput/arrlen | Correct grow semantics, type-safe macros, public domain, single header |
| Hash map for keyword table and string interning | Custom open-addressing table | `stb_ds.h` shput/shget | Proven implementation, handles string keys natively |
| Formatted string output in diagnostics | `sprintf` chain | `snprintf` with `strbuf` wrapper | Size-bounded, no buffer overflow risk |
| Cross-platform ANSI color codes | Platform ifdef spaghetti | Detect TTY with `isatty()` + ANSI escape codes | Three platforms need consistent behavior; isatty() disables color when piped to file |

**Key insight:** The stb_ds.h dynamic array and hash map handle the two most common "write this yourself" mistakes in C compilers — custom growable arrays that forget to handle reallocation, and custom string hash maps that collide. Vendoring stb_ds (copy into `src/vendor/`) costs nothing and eliminates these entire problem classes.

---

## Common Pitfalls

### Pitfall 1: Missing Span Tracking (from PITFALLS.md — Phase 1 variant)

**What goes wrong:** Span fields are added to the `Iron_Token` struct but not to AST nodes, or only added to "important" nodes. When the diagnostic system tries to report an error location from a node, it either crashes (NULL span) or reports the wrong location (inherited from a parent node).

**Why it happens:** Spans feel like a secondary concern. Developers build the AST structure first and plan to "add spans later." The moment any diagnostic is needed, the missing span causes a problem.

**How to avoid:** Define `Iron_Span` before writing a single node struct. Make it the first field of every node type (not a pointer — embed it directly). Every node constructor takes an `Iron_Span` parameter. There is no `iron_make_if_stmt()` that does not accept a span.

**Warning signs:** Any node constructor signature that lacks a span parameter. Any `Iron_Visitor` callback that has to reach for `node->children[0]->span` to find a location.

### Pitfall 2: Parser Error Cascade (from PITFALLS.md — Critical)

**What goes wrong:** A single syntax error (missing `}`) causes the parser to produce hundreds of spurious follow-on errors. The CONTEXT.md and the requirement PARSE-02 both specify "exactly N errors for N independent mistakes." Failing this produces an unusable developer experience.

**Why it happens:** The parser returns early on error (or propagates NULL) instead of inserting an ErrorNode and synchronizing.

**How to avoid:** Every parse function returns a valid node (possibly `Iron_ErrorNode`), never NULL. The `iron_parser_sync()` function advances to the next synchronization token at the appropriate grammar level. Tests must verify the error count, not just that an error exists.

**Warning signs:** Error count tests pass only when there is exactly one error in the file. A file with two errors produces more than two error messages.

### Pitfall 3: String Interpolation Regression in Lexer

**What goes wrong:** The lexer handles regular strings correctly but chokes on `{` inside strings. This crashes the parser or silently drops the interpolation.

**Why it happens:** String scanning stops at `{` without a state machine to handle nested expressions.

**How to avoid:** Decide on the interpolation strategy before writing the lexer string scanning function. The recommended approach (produce a raw INTERP_STRING token, parse segments in the parser) avoids complex lexer state. Test with: `"Hello {name}"`, `"{a + b}"`, `"{obj.field}"`, `"prefix {x} middle {y} suffix"`, and nested braces `"{func({x})}"`.

**Warning signs:** Round-trip test (parse then pretty-print) produces wrong output for any string with `{`.

### Pitfall 4: Keyword vs Identifier Collision

**What goes wrong:** `in` is both a keyword (for-in loop) and a valid identifier in many contexts. `self` is a keyword inside methods but could appear as an identifier elsewhere. The lexer keyword table must be complete and the parser must handle contextual keywords correctly.

**How to avoid:** All 37 keywords (including `in`, `self`, `true`, `false`, `null`) are in the keyword table from the start. The parser uses token kind, not string comparison, for all keyword checks. Context-sensitive interpretation (e.g., `parallel` appearing after a for-loop range) is handled in the parser, not the lexer.

**Warning signs:** Any `strcmp(token.value, "in")` in the parser. Lexer test that verifies `in` is tokenized as IDENTIFIER when it should be a keyword.

### Pitfall 5: `--` Comment vs `--` Prefix (Minus-Minus)

**What goes wrong:** Iron uses `--` as the comment delimiter. This means `x--` and `--x` are ambiguous at the lexer level: are they a decrement operator (which Iron does not have) or the start of a comment?

**Resolution:** Iron has no `--` operator. Any occurrence of `--` is always a comment delimiter. The lexer, upon seeing `-`, peeks at the next character: if it is also `-`, it scans to end of line and produces a COMMENT (or NEWLINE, skipping the comment text). This is unambiguous because Iron lacks `--` as an operator.

**How to avoid:** Add a specific test case: `x - --this is a comment` should tokenize as `IDENT MINUS COMMENT` (or just `IDENT MINUS` with the comment skipped).

---

## Code Examples

### Arena Allocator

```c
// Source: chibicc compiler pattern + rfleury arena allocator article

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint8_t *base;
    size_t   used;
    size_t   capacity;
} Iron_Arena;

Iron_Arena iron_arena_create(size_t capacity) {
    Iron_Arena a;
    a.base = malloc(capacity);
    a.used = 0;
    a.capacity = capacity;
    return a;
}

void *iron_arena_alloc(Iron_Arena *a, size_t size, size_t align) {
    // Align up
    size_t aligned = (a->used + align - 1) & ~(align - 1);
    if (aligned + size > a->capacity) {
        // Grow: double capacity
        size_t new_cap = a->capacity * 2;
        while (new_cap < aligned + size) new_cap *= 2;
        a->base = realloc(a->base, new_cap);
        a->capacity = new_cap;
    }
    void *ptr = a->base + aligned;
    a->used = aligned + size;
    return ptr;
}

char *iron_arena_strdup(Iron_Arena *a, const char *src, size_t len) {
    char *dst = iron_arena_alloc(a, len + 1, 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

void iron_arena_free(Iron_Arena *a) {
    free(a->base);
    a->base = NULL;
    a->used = a->capacity = 0;
}

#define ARENA_ALLOC(arena, T) \
    ((T*)iron_arena_alloc((arena), sizeof(T), _Alignof(T)))
```

### Lexer Core Loop

```c
// Source: chibicc lexer structure adapted to Iron token types

static Iron_Token iron_lex_next(Iron_Lexer *l) {
    iron_skip_whitespace(l);  // skip spaces and tabs; do NOT skip newlines

    if (l->pos >= l->len) {
        return iron_make_token(l, IRON_TOK_EOF, NULL, 0);
    }

    char c = l->src[l->pos];
    uint32_t start_line = l->line;
    uint32_t start_col  = l->col;

    // Comments: -- to end of line
    if (c == '-' && l->pos + 1 < l->len && l->src[l->pos + 1] == '-') {
        while (l->pos < l->len && l->src[l->pos] != '\n') l->pos++;
        // Produce NEWLINE (the comment consumed the line)
        return iron_make_token(l, IRON_TOK_NEWLINE, NULL, 0);
    }

    // Newlines are significant
    if (c == '\n') {
        l->pos++; l->line++; l->col = 1;
        return iron_make_token(l, IRON_TOK_NEWLINE, NULL, 0);
    }

    // String literals (including interpolated)
    if (c == '"') return iron_lex_string(l);

    // Numbers
    if (isdigit(c)) return iron_lex_number(l);

    // Identifiers and keywords
    if (isalpha(c) || c == '_') return iron_lex_identifier(l);

    // Operators and punctuation
    return iron_lex_punctuation(l);
}
```

### Recursive Descent Function Shape

```c
// Source: Clang recursive descent pattern adapted for Iron

// Every parse function follows this signature contract:
// - Takes Iron_Parser* (owns cursor, arena, diag list)
// - Returns Iron_Node* (never NULL; returns Iron_ErrorNode on failure)
// - Advances the token cursor past all consumed tokens

static Iron_Node *iron_parse_if_stmt(Iron_Parser *p) {
    Iron_Span span = iron_current_span(p);

    iron_expect(p, IRON_TOK_IF);  // consume 'if'

    Iron_Node *cond = iron_parse_expr(p);
    Iron_Node *body = iron_parse_block(p);

    // elif and else branches
    Iron_Node **elif_conds  = NULL;  // stb_ds array
    Iron_Node **elif_bodies = NULL;
    Iron_Node *else_body = NULL;

    while (iron_peek(p) == IRON_TOK_ELIF) {
        iron_advance(p);
        arrput(elif_conds, iron_parse_expr(p));
        arrput(elif_bodies, iron_parse_block(p));
    }
    if (iron_peek(p) == IRON_TOK_ELSE) {
        iron_advance(p);
        else_body = iron_parse_block(p);
    }

    Iron_IfStmt *node = ARENA_ALLOC(p->arena, Iron_IfStmt);
    node->span        = iron_span_to(span, iron_current_span(p));
    node->kind        = IRON_NODE_IF;
    node->condition   = cond;
    node->body        = body;
    node->elif_conds  = elif_conds;
    node->elif_bodies = elif_bodies;
    node->else_body   = else_body;
    return (Iron_Node*)node;
}

// Error recovery helper
static void iron_parser_sync(Iron_Parser *p, Iron_TokenKind *sync_set, int n) {
    while (iron_peek(p) != IRON_TOK_EOF) {
        for (int i = 0; i < n; i++) {
            if (iron_peek(p) == sync_set[i]) return;
        }
        iron_advance(p);
    }
}
```

### Unity Test Shape

```c
// Source: Unity v2.6.1 documentation pattern

#include "unity.h"
#include "lexer/lexer.h"

void setUp(void) {}
void tearDown(void) {}

void test_keyword_val_tokenizes_correctly(void) {
    Iron_Arena arena = iron_arena_create(4096);
    Iron_DiagList diags = {0};
    Iron_Lexer lexer = iron_lexer_create("val x = 10", "test.iron", &arena, &diags);

    Iron_Token *tokens = iron_lex_all(&lexer);

    TEST_ASSERT_EQUAL(IRON_TOK_VAL,        tokens[0].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_IDENTIFIER, tokens[1].kind);
    TEST_ASSERT_EQUAL_STRING("x",          tokens[1].value);
    TEST_ASSERT_EQUAL(IRON_TOK_ASSIGN,     tokens[2].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_INTEGER,    tokens[3].kind);
    TEST_ASSERT_EQUAL_STRING("10",         tokens[3].value);
    TEST_ASSERT_EQUAL(IRON_TOK_EOF,        tokens[4].kind);

    // Span checks
    TEST_ASSERT_EQUAL(1, tokens[0].line);
    TEST_ASSERT_EQUAL(1, tokens[0].col);
    TEST_ASSERT_EQUAL(1, tokens[1].line);
    TEST_ASSERT_EQUAL(5, tokens[1].col);

    iron_arena_free(&arena);
}

void test_three_independent_lex_errors_produce_three_diagnostics(void) {
    const char *src =
        "val a = \"unterminated\n"   // E0001
        "val b = @\n"                 // E0002
        "val c = \"also unterminated\n"; // E0001
    Iron_Arena arena = iron_arena_create(4096);
    Iron_DiagList diags = {0};
    Iron_Lexer lexer = iron_lexer_create(src, "test.iron", &arena, &diags);
    iron_lex_all(&lexer);

    TEST_ASSERT_EQUAL(3, diags.error_count);

    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_keyword_val_tokenizes_correctly);
    RUN_TEST(test_three_independent_lex_errors_produce_three_diagnostics);
    return UNITY_END();
}
```

### CMakeLists.txt Structure

```cmake
# CMakeLists.txt (root)
cmake_minimum_required(VERSION 3.25)
project(iron C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
add_compile_options(-Wall -Wextra -Werror)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# Fetch Unity test framework
include(FetchContent)
FetchContent_Declare(
    Unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.1
)
FetchContent_MakeAvailable(Unity)

# Compiler library (Phase 1 sources)
add_library(iron_compiler STATIC
    src/util/arena.c
    src/util/strbuf.c
    src/lexer/lexer.c
    src/parser/ast.c
    src/parser/parser.c
    src/diagnostics/diagnostics.c
)
target_include_directories(iron_compiler PUBLIC src src/vendor)

# Unit tests
enable_testing()

add_executable(test_lexer
    tests/lexer/test_keywords.c
    tests/lexer/test_literals.c
    tests/lexer/test_operators.c
    tests/lexer/test_errors.c
)
target_link_libraries(test_lexer iron_compiler unity)
add_test(NAME lexer COMMAND test_lexer)

add_executable(test_parser
    tests/parser/test_decls.c
    tests/parser/test_stmts.c
    tests/parser/test_exprs.c
    tests/parser/test_interp.c
    tests/parser/test_errors.c
)
target_link_libraries(test_parser iron_compiler unity)
add_test(NAME parser COMMAND test_parser)
```

---

## State of the Art

| Old Approach | Current Approach | Notes |
|--------------|------------------|-------|
| Flex + Bison for lexer/parser | Hand-written recursive descent | GCC, Clang, Go all use hand-written parsers. Locked decision. |
| Global malloc for AST nodes | Arena allocator per compilation unit | Standard pattern in Clang, TCC, chibicc. Locked decision. |
| Tagged union AST | Struct per node type | Locked decision. Enables clean field names, avoids union accessor noise. |
| Valgrind for memory debugging | ASan + UBSan | Valgrind does not work on macOS Apple Silicon. ASan is built into clang. |
| Makefiles for build | CMake + Ninja | Cross-platform, faster no-op rebuilds. Locked decision. |

**Note on `parallel` keyword:** Iron uses `parallel` as a modifier on for-loops (`for i in range(n) parallel { }`). This means `parallel` appears in a non-leading position in the statement. The parser's statement parsing must handle this: after parsing the for-loop range expression, check if the next token is `parallel` and branch accordingly.

**Note on `draw { }` block:** The `draw { }` syntax is a special statement form (not a keyword-expression form). The lexer produces `IRON_TOK_IDENTIFIER` for `draw` (it is not a keyword). The parser recognizes the pattern `identifier LBRACE` as a potential draw block. This avoids polluting the keyword table but requires the parser to handle `draw` as a contextual keyword. For Phase 1, parsing `draw { }` as a `DrawBlock` node is sufficient — semantic validation that it maps to raylib `BeginDrawing()`/`EndDrawing()` is Phase 2+.

---

## Open Questions

1. **`draw` as contextual keyword or reserved word**
   - What we know: `draw` is not in the keyword table in the language spec
   - What's unclear: Should it be in the keyword table (IRON_TOK_DRAW) or parsed as `IDENTIFIER LBRACE` in the parser with special handling?
   - Recommendation: Add as a reserved keyword in the lexer (simpler, prevents users from naming variables `draw`). Alternatively handle as contextual keyword in the parser if user-defined types named `draw` are needed. Claude's discretion.

2. **Newline as statement terminator vs noise**
   - What we know: Iron uses `}` and newlines to delimit statements. The implementation_plan lists NEWLINE as a token.
   - What's unclear: Does the parser consume NEWLINEs explicitly, or does the lexer skip them and the parser only sees `}`?
   - Recommendation: Produce NEWLINE tokens in the lexer. The parser skips them in most contexts but uses them for error recovery (a NEWLINE after an error expression is a sync point). This gives more precise error recovery without requiring a complex grammar-level newline rule.

3. **`in` keyword ambiguity**
   - What we know: `in` is used as `for x in collection` (loop iteration keyword). It appears in the keyword table.
   - What's unclear: Can users name a variable `in`? A function `in`? What about `contains_in` — is that parsed as `contains_KEYWORD_in`?
   - Recommendation: `in` is a keyword — any identifier-position occurrence of the string "in" is tokenized as `IRON_TOK_IN`. Identifiers containing "in" as a substring (e.g., `index`, `integer`, `incoming`) are tokenized as `IDENTIFIER` normally (keyword check is whole-word only).

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Unity v2.6.1 |
| Config file | None — included via CMake FetchContent |
| Quick run command | `cmake --build build && ctest --test-dir build -R lexer -R parser --output-on-failure` |
| Full suite command | `cmake --build build && ctest --test-dir build --output-on-failure` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| LEX-01 | All token types recognized | unit | `ctest --test-dir build -R lexer` | ❌ Wave 0 |
| LEX-02 | Every token has correct line/col span | unit | `ctest --test-dir build -R lexer` | ❌ Wave 0 |
| LEX-03 | 3 independent lex errors → exactly 3 diagnostics | unit | `ctest --test-dir build -R test_lexer_errors` | ❌ Wave 0 |
| LEX-04 | Comments skipped; not in token stream | unit | `ctest --test-dir build -R lexer` | ❌ Wave 0 |
| PARSE-01 | Complete AST for all syntax forms | unit | `ctest --test-dir build -R parser` | ❌ Wave 0 |
| PARSE-02 | 3 independent parse errors → exactly 3 diagnostics | unit | `ctest --test-dir build -R test_parser_errors` | ❌ Wave 0 |
| PARSE-03 | String interpolation segments in AST | unit | `ctest --test-dir build -R test_interp` | ❌ Wave 0 |
| PARSE-04 | Operator precedence: `2 + 3 * 4` → `BinaryExpr(+, 2, BinaryExpr(*, 3, 4))` | unit | `ctest --test-dir build -R test_exprs` | ❌ Wave 0 |
| PARSE-05 | Round-trip: parse then pretty-print produces equivalent Iron | unit | `ctest --test-dir build -R test_roundtrip` | ❌ Wave 0 |
| TEST-03 | Error diagnostic tests: error code + line number correct | unit + integration | `ctest --test-dir build -R test_errors` | ❌ Wave 0 |

### Sampling Rate

- **Per task commit:** `cmake --build build && ctest --test-dir build --output-on-failure`
- **Per wave merge:** `cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja -B build . && cmake --build build && ctest --test-dir build --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps

Everything is new. Required test infrastructure before any implementation:

- [ ] `CMakeLists.txt` — root build file with Unity FetchContent, iron_compiler library, test executables
- [ ] `tests/lexer/test_keywords.c` — Unity tests for all 37 keywords
- [ ] `tests/lexer/test_literals.c` — Unity tests for int, float, string, bool, null literals
- [ ] `tests/lexer/test_operators.c` — Unity tests for all operator/delimiter tokens
- [ ] `tests/lexer/test_errors.c` — Unity tests for E0001, E0002, error count invariant
- [ ] `tests/parser/test_decls.c` — Unity tests for object, func, interface, enum, import declarations
- [ ] `tests/parser/test_stmts.c` — Unity tests for if/elif/else, while, for, match, defer, free, leak, spawn
- [ ] `tests/parser/test_exprs.c` — Unity tests for all operator precedences, calls, field access, index, slice
- [ ] `tests/parser/test_interp.c` — Unity tests for string interpolation AST structure
- [ ] `tests/parser/test_errors.c` — Unity tests for error count invariant, error codes, line numbers
- [ ] `tests/integration/hello.iron` + `tests/integration/hello.expected` — round-trip test
- [ ] `tests/integration/game.iron` + `tests/integration/game.expected` — parse README example

---

## Sources

### Primary (HIGH confidence)

- `docs/implementation_plan.md` — authoritative token list (Phase 1), AST node types (Phase 2), grammar priorities
- `docs/language_definition.md` — complete language spec: all keywords, operators, syntax forms
- `.planning/research/STACK.md` — CMake 3.25+, Ninja 1.11+, Unity v2.6.1, stb_ds.h, arena allocator — all confirmed HIGH confidence
- `.planning/research/ARCHITECTURE.md` — recursive descent, multi-pass pipeline, arena pattern, visitor pattern — HIGH confidence, verified against Nim/Clang/Zig
- `.planning/research/PITFALLS.md` — Parser error cascade (Pitfall 13), span tracking (implicit in all pitfalls), keyword collision (Pitfall 1 variant) — HIGH confidence
- `.planning/research/FEATURES.md` — span tracking as P1 requirement; error diagnostics as highest-leverage UX investment — HIGH confidence
- `.planning/phases/01-frontend/01-CONTEXT.md` — locked decisions: arena, struct-per-node, Iron_ prefix, Unity, CMake, visitor pattern, error codes

### Secondary (MEDIUM confidence)

- chibicc (Rui Ueyama's C compiler) — arena allocator pattern, recursive descent structure — MEDIUM confidence
- rfleury arena allocator article — bump pointer + realloc growth strategy — MEDIUM confidence
- Resilient recursive descent parsing (tratt.net) — error recovery synchronization strategy — MEDIUM confidence

### Tertiary (LOW confidence)

- None for Phase 1 — all claims are verified against project's own specifications or well-established compiler patterns.

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — CMake, Unity, stb_ds, arena all locked by CONTEXT.md and verified by STACK.md
- Architecture: HIGH — recursive descent, struct-per-node, arena, visitor all locked by CONTEXT.md and verified against production compilers
- Token/grammar: HIGH — implementation_plan.md is authoritative; language_definition.md provides complete syntax
- Pitfalls: HIGH — span tracking and error recovery are well-documented compiler construction problems with known solutions

**Research date:** 2026-03-25
**Valid until:** 2026-06-25 (C11 standard, CMake, Unity are stable; no fast-moving dependencies in Phase 1)
