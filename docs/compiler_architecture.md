# Iron Compiler Architecture

This document describes the internal architecture of the Iron compiler. Iron compiles source code to C, then invokes clang (or gcc) to produce native binaries. The compiler is written in C (C11).

## Pipeline Overview

```
Iron Source (.iron)
       |
       v
  [1. Lexer]          src/lexer/
       |
       v
  [2. Parser]         src/parser/
       |
       v
  [3. Analyzer]       src/analyzer/
       |
       v
  [4. HIR Lowering]   src/hir/hir_lower.c
       |
       v
  [5. HIR-to-LIR]     src/hir/hir_to_lir.c
       |
       v
  [6. LIR Optimizer]  src/lir/lir_optimize.c
       |
       v
  [7. C Emission]     src/lir/emit_c.c
       |
       v
  [8. clang/gcc]
       |
       v
  Native Binary
```

Each stage has its own data structures, arena allocator, and validation. Errors at any stage halt the pipeline and produce Rust-style diagnostics with source snippets and suggestions.

---

## Stage 1: Lexer

**Directory:** `src/lexer/`
**Files:** `lexer.h`, `lexer.c`
**Input:** Raw source text (char*)
**Output:** Array of `Iron_Token`

Tokenizes Iron source into a flat token array. Handles all keywords, operators, literals (int, float, string with interpolation markers, bool), delimiters, and comments. Every token carries an `Iron_Span` (file, line, column, length) for diagnostics.

Errors (unterminated strings, invalid characters) are reported with exact source locations. Multiple errors can be reported in a single pass.

---

## Stage 2: Parser

**Directory:** `src/parser/`
**Files:** `parser.h`, `parser.c`, `ast.h`, `ast.c`, `printer.h`, `printer.c`
**Input:** Token array
**Output:** `Iron_Node*` (AST root, typically an `Iron_Program`)

Recursive descent parser producing a complete abstract syntax tree. Every AST node carries a source span. The parser supports error recovery via `ErrorNode` insertion, allowing multiple syntax errors to be reported without cascading.

The AST preserves source structure exactly: if/elif/else chains, for-range loops, match arms, string interpolation, compound assignment operators, etc. No desugaring happens at this stage.

**Key types:**
- `Iron_NodeKind` enum with ~60 variants covering all language constructs
- `Iron_Node` tagged union with per-kind data structs
- `Iron_Program` top-level node containing type declarations, externs, and function definitions

---

## Stage 3: Semantic Analyzer

**Directory:** `src/analyzer/`
**Files:** `analyzer.h/.c`, `resolve.h/.c`, `typecheck.h/.c`, `scope.h/.c`, `escape.h/.c`, `concurrency.h/.c`, `types.h/.c`
**Input:** AST (`Iron_Program*`)
**Output:** Annotated AST (same tree, with resolved types, scopes, and analysis results)

Multi-pass analysis that annotates the AST in place:

1. **Name resolution** (`resolve.c`): Resolves all identifiers to their declarations. Builds scope tree with parent/child relationships. Detects undeclared variables, duplicate declarations.

2. **Type checking** (`typecheck.c`): Infers and validates types for every expression. Checks function call argument counts and types. Validates operator compatibility. Resolves generic type instantiation.

3. **Escape analysis** (`escape.c`): Determines which heap allocations escape their defining scope. Non-escaping allocations get automatic `free` insertion. Marks `auto_free` flags on allocations.

4. **Concurrency checking** (`concurrency.c`): Validates that `parallel for` bodies don't mutate outer non-mutex variables. Checks spawn/await patterns.

**Also includes:**
- `Iron_Type` system (`types.h/.c`): Int, Float, Bool, String, Array, Object, Nullable, RC, Function types. Reused by all downstream stages.
- Comptime evaluation (`src/comptime/comptime.c`): Evaluates `comptime` expressions at compile time (arithmetic, string operations, `read_file`).

---

## Stage 4: High IR (HIR) Lowering

**Directory:** `src/hir/`
**Files:** `hir.h`, `hir.c`, `hir_lower.h`, `hir_lower.c`, `hir_print.c`, `hir_verify.c`
**Input:** Annotated AST
**Output:** `IronHIR_Module` (structured, language-level IR)

The HIR is a cleaned-up version of the AST designed to be the input for LIR construction. It preserves Iron's high-level semantics while applying light desugaring.

### HIR Design

**Two node kinds:**
- `IronHIR_Stmt` (13 kinds): let, assign, if/else, for, while, match, return, defer, block, expr_stmt, spawn_stmt, break, continue
- `IronHIR_Expr` (28 kinds): binop, unop, call, method_call, field_access, index_access, literal (int/float/bool/string/null), ident, construct, array_lit, closure, cast, slice, spawn, parallel_for, await, is_null, is_not_null, func_ref, interp_string, range, and more

**Key properties:**
- **Structured control flow**: if/else, for, while, match are tree nodes (not CFG basic blocks)
- **Named variables**: `IronHIR_VarId` with a `name_table` mapping IDs to source names
- **Lexical scopes**: Each `IronHIR_Block` creates a new scope; let bindings are visible to subsequent statements in the same block and nested blocks
- **Defer preserved**: Defer remains a statement node (not inlined at exit points)
- **Functions only**: `IronHIR_Module` holds `IronHIR_Func` nodes. Type declarations and externs stay on the LIR module since they don't benefit from high-level representation.

### Desugaring (AST to HIR)

Four transformations happen during HIR lowering:

1. **String interpolation** to concat calls: `"hello {name}"` becomes a series of `to_string` + concatenation expressions
2. **For-range** to while: `for i in range(n)` becomes `{ var i = 0; while i < n { ...; i = i + 1 } }`
3. **Elif chains** to nested if-in-else: `if A {} elif B {} else {}` becomes `if A {} else { if B {} else {} }`
4. **Compound assignment** to binop+assign: `x += 1` becomes `x = x + 1`

### HIR Tools

- **Printer** (`hir_print.c`): Produces an indented tree dump showing the AST shape. Types shown on declarations only. 2-space indentation per nesting level.
- **Verifier** (`hir_verify.c`): Validates structural integrity, scope nesting (every variable use resolves to a visible let binding), and type consistency (binop operand compatibility, call argument count). Collects all errors without early exit.

---

## Stage 5: HIR-to-LIR Lowering

**Directory:** `src/hir/`
**Files:** `hir_to_lir.h`, `hir_to_lir.c`
**Input:** `IronHIR_Module`
**Output:** `IronLIR_Module` (SSA-form, CFG-based IR)

Transforms the structured HIR tree into the flat, SSA-form Lower IR. This is the most complex transformation in the compiler.

### Three-Pass Algorithm

**Pass 1: Flatten to pre-SSA CFG**
Walk the HIR tree and create LIR basic blocks. Structured control flow is flattened:
- `if/else` becomes: condition block, then block, else block, merge block with conditional branch
- `while` becomes: header block (with condition), body block, exit block, back-edge from body to header
- `match` becomes: switch instruction with case blocks and merge block
- Variables become `ALLOCA` + `STORE`/`LOAD` sequences (not yet SSA)
- Defer creates dedicated cleanup blocks (not inlined at every exit point)

**Pass 2: SSA Construction**
Classic dominance-frontier phi placement:
1. Rebuild CFG edges from terminators
2. Compute dominator tree (iterative Cooper-Harvey-Kennedy algorithm)
3. Compute dominance frontiers
4. Insert phi nodes at dominance frontiers for each variable
5. Rename variables to SSA form

**Pass 3: Type Registration**
Register type declarations (objects, enums, interfaces, externs) from the AST program onto the LIR module. These bypass the HIR since they're declarative, not behavioral.

---

## Stage 6: LIR (Lower IR)

**Directory:** `src/lir/`
**Files:** `lir.h`, `lir.c`, `lir_optimize.h`, `lir_optimize.c`, `print.h`, `print.c`, `verify.h`, `verify.c`
**Input:** `IronLIR_Module` from HIR-to-LIR lowering
**Output:** Optimized `IronLIR_Module`

The LIR is a mid-level SSA-form IR with basic blocks and a flat instruction set. It is the representation that optimization passes operate on.

### LIR Structure

- **Module** (`IronLIR_Module`): Type declarations (objects, enums, interfaces), extern declarations, functions, monomorphization registry
- **Function** (`IronLIR_Func`): Parameters with types, return type, array of basic blocks, value table (ValueId to instruction lookup)
- **Block** (`IronLIR_Block`): Array of instructions, CFG edges (predecessors/successors), Braun SSA state
- **Instruction** (`IronLIR_Instr`): Tagged union with 37 instruction kinds, each producing a `IronLIR_ValueId` (uint32_t, 0 = invalid)

### Instruction Set (37 kinds)

| Category | Instructions |
|----------|-------------|
| Constants | CONST_INT, CONST_FLOAT, CONST_BOOL, CONST_STRING, CONST_NULL |
| Arithmetic | ADD, SUB, MUL, DIV, MOD |
| Comparison | EQ, NEQ, LT, LTE, GT, GTE |
| Logical | AND, OR |
| Unary | NEG, NOT |
| Memory | ALLOCA, LOAD, STORE |
| Field/Index | GET_FIELD, SET_FIELD, GET_INDEX, SET_INDEX |
| Control Flow | JUMP, BRANCH, SWITCH, RETURN |
| High-Level | CALL, CAST, CONSTRUCT, ARRAY_LIT, SLICE, IS_NULL, IS_NOT_NULL |
| Memory Mgmt | HEAP_ALLOC, RC_ALLOC, FREE |
| Concurrency | MAKE_CLOSURE, FUNC_REF, SPAWN, PARALLEL_FOR, AWAIT |
| SSA | PHI |

### LIR Tools

- **Printer** (`print.c`): LLVM-style text format: `%0 = add %1, %2 : Int`. Named blocks (`entry`, `if.then`, `while.body`).
- **Verifier** (`verify.c`): Validates every block has a terminator, all branch targets are valid, all value references exist, no use-before-def. Collects all errors.

---

## Stage 7: LIR Optimization

**Directory:** `src/lir/`
**Files:** `lir_optimize.h`, `lir_optimize.c`
**Input:** `IronLIR_Module`
**Output:** Optimized `IronLIR_Module` + `IronLIR_OptimizeInfo` (metadata for emission)

Six optimization passes run in a fixpoint loop (iterate until no changes, max 32 iterations). The verifier runs between every pass to catch bugs at the exact pass that introduced them.

### Optimization Pipeline

```
phi_eliminate  ->  array_param_modes  ->  array_repr_optimize
       |
       v
  Fixpoint Loop:
    copy_propagation  ->  constant_folding  ->  DCE
         ->  store_load_elimination  ->  strength_reduction
       |
       v
  compute_use_counts  ->  compute_func_purity  ->  compute_inline_eligible
```

**Pre-optimization passes (run once):**
1. **Phi elimination**: Converts SSA phi nodes to alloca+store+load pattern for C emission
2. **Array parameter mode analysis**: Determines if array parameters can use pointer+length instead of Iron_List_T. Iterative convergence across the call graph.
3. **Array representation optimization**: Marks small arrays (<=256 elements) as stack-eligible. Escape analysis revokes stack eligibility if the array escapes.

**Fixpoint passes (iterate until stable):**
4. **Copy propagation**: Eliminates trivial LOAD copies by replacing all uses with the original value. Targets the alloca-with-single-store pattern from phi elimination.
5. **Constant folding**: Evaluates compile-time constant arithmetic (CONST_INT op CONST_INT) in place. Handles addition, subtraction, multiplication, division, modulo, and all comparisons.
6. **Dead code elimination**: Worklist-based liveness analysis. Seeds from side-effecting instructions (CALL, STORE, RETURN, SET_INDEX, etc.), propagates through operand chains, removes non-live pure instructions.
7. **Store/Load elimination**: Forward dataflow scan per basic block. Tracks last-stored value per alloca. Non-escaped allocas survive CALL instructions (escape analysis via address-taken tracking). SET_INDEX/SET_FIELD invalidate tracked entries.
8. **Strength reduction**: Detects affine `a*i + b` patterns in loop bodies (where `i` is an induction variable and `a`, `b` are loop-invariant). Replaces multiply with an induction variable that increments by `a` each iteration. Uses dominator tree and natural loop analysis infrastructure (nested loop tree with parent references).

**Post-optimization analysis (for C emission):**
9. **Use-count analysis**: Counts references to each ValueId. Used by expression inlining.
10. **Function purity analysis**: Transitively determines which functions are pure (no side effects). Enables call inlining in expressions.
11. **Inline eligibility**: Marks single-use pure values as inlineable at their use site during C emission.

### Supporting Infrastructure

- **Dominator tree**: Iterative Cooper-Harvey-Kennedy algorithm. Computed per function, recomputed each fixpoint iteration.
- **Natural loop analysis**: `IronLIR_LoopInfo` structs with header, latch, preheader, body blocks, induction variable, and parent (for nested loops).
- **Purity classification**: `iron_lir_instr_is_pure()` classifies all 37 instruction kinds. Pure instructions can be eliminated by DCE and inlined during emission.
- **CLI flags**: `--dump-ir-passes` prints IR after each pass. `--no-optimize` skips all optimization passes (for A/B benchmark comparison).

---

## Stage 8: C Emission

**Directory:** `src/lir/`
**Files:** `emit_c.h`, `emit_c.c`
**Input:** Optimized `IronLIR_Module` + `IronLIR_OptimizeInfo`
**Output:** C source string (arena-allocated)

Emits valid C11 code from the optimized LIR. This is a pure rendering pass with no transformations.

### Emission Phases

1. Include directives (runtime, stdint, stdlib, string, stdio, math, stdlib modules)
2. Forward declarations (`typedef struct Iron_Foo Iron_Foo;`)
3. Struct bodies (topologically sorted for inheritance), interface vtable structs
4. Enum definitions with explicit values
5. Function prototypes
6. Lifted function bodies (lambdas, spawn bodies, parallel-for chunk functions)
7. Function implementations
8. `main()` wrapper calling `iron_runtime_init()`, `Iron_main()`, `iron_runtime_shutdown()`

### Expression Inlining

During emission, single-use pure values are inlined at their use site via `emit_expr_to_buf()`. Instead of:
```c
int64_t _v10 = _v2;
int64_t _v13 = _v11 * _v12;
int64_t _v14 = _v10 + _v13;
```
The emitter produces:
```c
int64_t _v14 = (_v2 + (_v11 * _v12));
```

Inlining rules:
- Only single-use pure values are inlined
- No inlining across basic block boundaries
- Calls to pure functions (including known-pure builtins) can be inlined
- Always parenthesized for correctness
- Long expressions break at ~120 characters
- Clarifying comments added for casts, constructs, and deep expressions

### Type Mapping

| Iron Type | C Type |
|-----------|--------|
| Int | int64_t |
| Float | double |
| Bool | bool |
| String | Iron_String |
| Object | Iron_<Name> (struct) |
| Nullable | Iron_Optional_<Type> |
| RC | <Type>* |
| Array | Iron_List_<Type> or stack array (int64_t*) |
| Function | void* |

---

## Stage 9: C Compilation

The emitted C is written to a temporary file and compiled by clang (preferred) or gcc:

```
clang -std=c11 -Wall -Werror -O3 \
  -I<lib>/runtime -I<lib>/stdlib \
  -o <binary> <generated.c> \
  -L<lib>/runtime -liron_runtime -lpthread -lm
```

The temporary C file is deleted after compilation unless `--debug-build` is specified.

---

## Runtime Library

**Directory:** `src/runtime/`
**Files:** `iron_runtime.h`, `iron_string.c`, `iron_collections.c`, `iron_rc.c`, `iron_builtins.c`, `iron_threads.c`

Separate CMake target (`iron_runtime`) linked into every Iron binary. Provides:
- **Strings**: `Iron_String` with reference-counted backing, concatenation, substring, interpolation helpers
- **Collections**: `Iron_List_T` generic dynamic arrays, `Iron_Map`, `Iron_Set`
- **Reference counting**: `Iron_Rc` with increment/decrement/free
- **Builtins**: `println`, `print`, `range`, `len`, `abs`, `min`, `max`, type conversion functions
- **Threading**: Thread pool for `parallel for`, spawn/await via pthreads

---

## Standard Library

**Directory:** `src/stdlib/`
**Modules:** `math` (`iron_math.c`), `io` (`iron_io.c`), `time` (`iron_time.c`), `log` (`iron_log.c`)

Each module has a C implementation (linked with the runtime) and an optional `.iron` wrapper file for high-level APIs. Imported via `import math`, `import io`, etc.

---

## Diagnostics

**Directory:** `src/diagnostics/`
**Files:** `diagnostics.h`, `diagnostics.c`

Rust-style error reporting with source snippets, line/column arrows, and suggestions. `Iron_DiagList` collects diagnostics from all pipeline stages. `Iron_Span` on every AST/HIR/LIR node enables precise error locations.

Error codes are namespaced: `IRON_ERR_LEX_*` (100s), `IRON_ERR_PARSE_*` (200s), `IRON_ERR_SEM_*` (300s), `IRON_ERR_LIR_*` (400s), `IRON_ERR_HIR_*` (500s).

---

## CLI Toolchain

**Directory:** `src/cli/` (ironc), `src/pkg/` (iron)

Two binaries:
- **`ironc`** — Raw compiler. `ironc build`, `ironc run`, `ironc check`, `ironc fmt`, `ironc test`.
- **`iron`** — Package manager. `iron init`, `iron build`, `iron run`, `iron check`, `iron test`. Discovers `ironc` as a sibling binary. Manages `iron.toml` projects and GitHub-sourced dependencies.

---

## Memory Management

The compiler uses arena allocation throughout:
- **AST arena**: Allocated during parsing, freed after compilation
- **HIR arena**: Internal to `IronHIR_Module`, freed after HIR-to-LIR lowering
- **LIR arena**: Allocated for LIR construction, freed after C emission
- **stb_ds**: Used for dynamic arrays (`arrput`/`arrlen`) and hash maps (`hmput`/`hmget`) across all stages

Each pipeline stage owns its arena. Stages do not share arenas, preventing use-after-free across stage boundaries.

---

## File Size Summary

| Component | Lines |
|-----------|-------|
| HIR (src/hir/) | ~5,700 |
| LIR (src/lir/) | ~8,100 |
| Frontend (lexer + parser + analyzer + comptime + diagnostics) | ~9,800 |
| Runtime + Stdlib | ~2,000 |
| CLI (ironc + iron) | ~3,500 |
| **Total compiler** | **~29,100** |
