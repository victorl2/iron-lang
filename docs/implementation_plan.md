# Iron — Implementation Plan

Iron compiles `.iron` source files into **standalone executables**. The compilation pipeline is:

```
.iron source → Lexer → Parser → Semantic Analysis → C Code Generation → gcc/clang → standalone binary
```

The Iron runtime is statically linked into the final binary. The output is a native executable — no Iron installation or VM required to run it. External libraries (graphics, audio) are dynamically linked as standard for game binaries.

This plan is organized in phases. Each phase produces something runnable or testable.

---

## Phase 1: Lexer

Turn source text into a stream of tokens.

### Tokens to support

```
-- literals
INTEGER        123
FLOAT          3.14
STRING         "hello"
BOOL           true, false
NULL           null

-- identifiers & keywords
IDENTIFIER     myVar, Player, update
KEYWORD        val, var, func, object, enum, interface, import,
               if, elif, else, for, while, match, return,
               heap, free, leak, defer, rc, private,
               extends, implements, super, is,
               spawn, await, parallel, pool,
               not, and, or, self, comptime, in

-- operators
PLUS           +
MINUS          -
STAR           *
SLASH          /
PERCENT        %
ASSIGN         =
EQUALS         ==
NOT_EQUALS     !=
LESS           <
GREATER        >
LESS_EQ        <=
GREATER_EQ     >=
DOT            .
DOTDOT         ..
COMMA          ,
COLON          :
ARROW          ->
QUESTION       ?

-- delimiters
LPAREN         (
RPAREN         )
LBRACKET       [
RBRACKET       ]
LBRACE         {
RBRACE         }
SEMICOLON      ;

-- special
COMMENT        -- ...
NEWLINE
EOF
```

### Deliverable

- Lexer that takes a `.iron` source string and produces a `List[Token]`
- Each token has: type, value, line number, column number
- Error reporting for unterminated strings, invalid characters
- Test suite with coverage for all token types

---

## Phase 2: Parser

Turn token stream into an Abstract Syntax Tree (AST).

### AST Node Types

```
-- top-level
Program          list of top-level declarations
ImportDecl       import path, optional alias
ObjectDecl       name, fields, extends?, implements[]
InterfaceDecl    name, method signatures
EnumDecl         name, variants
FuncDecl         name, params, return type?, body
MethodDecl       type name, method name, params, return type?, body

-- statements
ValDecl          name, type?, initializer
VarDecl          name, type?, initializer
Assignment       target, value
ReturnStmt       value?
IfStmt           condition, body, elif branches, else body
WhileStmt        condition, body
ForStmt          variable, iterable, parallel pool?, body
MatchStmt        subject, cases, else case?
DeferStmt        expression
FreeStmt         expression
LeakStmt         expression
SpawnStmt        name, pool?, body, handle?
AwaitExpr        handle

-- expressions
IntLiteral       value
FloatLiteral     value
StringLiteral    value (with interpolation segments)
BoolLiteral      value
NullLiteral
Identifier       name
BinaryExpr       left, op, right
UnaryExpr        op, operand
CallExpr         target, arguments
MethodCallExpr   object, method, arguments
FieldAccess      object, field
IndexAccess      object, index
SliceExpr        object, start, end
LambdaExpr       params, return type?, body
HeapExpr         inner expression
RcExpr           inner expression
ComptimeExpr     inner expression
IsExpr           expression, type
ConstructExpr    type name, arguments
ArrayLiteral     type, size?, elements
CastExpr         target type, expression
```

### Grammar priorities

1. Expressions and operator precedence
2. Object and function declarations
3. Control flow (if/elif/else, while, for, match)
4. Memory keywords (heap, free, leak, defer, rc)
5. Lambdas
6. Generics
7. Concurrency (spawn, await, parallel)
8. String interpolation parsing
9. Comptime

### Deliverable

- Recursive descent parser producing an AST
- Error recovery: report multiple errors per file, don't stop at the first
- Pretty-printer that can dump the AST back to readable Iron (for debugging)
- Test suite: one test per grammar rule minimum

---

## Phase 3: Semantic Analysis

Validate the AST and enrich it with type information.

### 3a: Name Resolution

- Build scope tree (global → module → function → block)
- Resolve all identifiers to their declarations
- Resolve imports (locate `.iron` files by path)
- Resolve `self` inside methods
- Resolve `super` calls to parent methods
- Error on undefined variables, duplicate declarations, private access violations

### 3b: Type Checking

- Infer types for `val`/`var` with no explicit type
- Check all assignments: lhs type matches rhs type
- Check function call arguments: count and types match signature
- Check return types match function declaration
- Enforce `val` immutability (no reassignment)
- Enforce `var` requirement for mutable object parameters
- Enforce nullable rules: `T` cannot be `null`, `T?` must be checked before use
- Narrow nullable types after `!= null` checks
- Validate `is` expressions against type hierarchy
- Check interface implementation completeness
- Check method override signatures match parent
- Validate generic type parameters
- Enforce comptime restrictions (no heap, no rc, no runtime APIs)

### 3c: Escape Analysis

- Track heap allocations
- Determine if heap values escape their block (returned, stored in outer scope)
- Mark non-escaping heap values for auto-free
- Emit warnings for escaping values without `free` or `leak`
- Error on `free`/`leak` of non-heap values

### 3d: Concurrency Checks

- Enforce parallel for body cannot mutate outer non-mutex variables
- Validate spawn block captures

### Deliverable

- Annotated AST with resolved types on every expression
- Symbol table for each scope
- Diagnostic list: errors and warnings with line/column info
- Test suite for type errors, scope errors, nullable violations, escape analysis

---

## Phase 4: C Code Generation

Walk the annotated AST and emit C code.

### 4a: Type Mapping

```
Iron             C
------           ------
Int              int64_t
Int8             int8_t
Int16            int16_t
Int32            int32_t
Int64            int64_t
UInt             uint64_t
UInt8            uint8_t
Float            double
Float32          float
Float64          double
Bool             bool
String           Iron_String (struct)
object           typedef struct
enum             typedef enum
[T; N]           T name[N]
List[T]          Iron_List_T (generated struct)
Map[K,V]         Iron_Map_K_V (generated struct)
Set[T]           Iron_Set_T (generated struct)
T?               Iron_Optional_T (struct with value + has_value flag)
rc T             Iron_Rc_T (struct with value + refcount)
func(A) -> R     R (*name)(A)  (function pointer)
```

### 4b: Code Emission Order

1. `#include` directives (runtime headers, standard C headers)
2. Forward declarations of all structs
3. Struct definitions (topologically sorted by dependency)
4. Function prototypes
5. Function implementations
6. `main()` entry point wrapping `Iron_main()`

### 4c: Language Feature → C Translation

| Iron | C |
|---|---|
| `val x = 10` | `const int64_t x = 10;` |
| `var x = 10` | `int64_t x = 10;` |
| `object Player { ... }` | `typedef struct { ... } Player;` |
| `func foo() { }` | `void foo() { }` |
| `func Player.update(dt) { }` | `void Player_update(Player* self, double dt) { }` |
| `player.update(dt)` | `Player_update(&player, dt);` |
| `Player(a, b)` | `(Player){ .field1 = a, .field2 = b }` |
| `heap T(...)` | `malloc + init` |
| `free x` | `free(x)` |
| `defer f()` | Collect and emit at scope exit (reverse order) |
| `rc expr` | `Iron_Rc_T_create(expr)` |
| `if x != null { x.foo() }` | `if (x.has_value) { x.value.foo(); }` |
| `spawn("n") { }` | Extract to function, `pool_submit(...)` |
| `await handle` | `Iron_handle_wait(&handle)` |
| `channel[T](n)` | `Iron_Channel_T_create(n)` |
| `mutex(val)` | `Iron_Mutex_T_create(val)` |
| `for ... parallel(p) { }` | Split range, submit chunks, barrier |
| `comptime expr` | Evaluate at compile time, emit result as literal |
| String interpolation | `snprintf` or string builder calls |

### 4d: Defer Implementation

```
// Iron
func foo() {
  val a = open("x")
  defer close(a)
  val b = open("y")
  defer close(b)
  do_work()
}

// Generated C: defers run in reverse order at every exit point
void foo() {
  File a = open("x");
  File b = open("y");
  do_work();
  close(b);  // defer 2
  close(a);  // defer 1
}
```

Handle early returns: defers must run before every `return` statement.

### 4e: Inheritance

```
// Iron
object Entity { var pos: Vec2; var hp: Int }
object Player extends Entity { val name: String }

// Generated C: embed parent struct
typedef struct { Vec2 pos; int64_t hp; } Entity;
typedef struct { Vec2 pos; int64_t hp; Iron_String name; } Player;

// Casting: Player* can be cast to Entity* because fields align
```

### Deliverable

- C code generator that produces compilable `.c` files
- Generated code compiles with `gcc -std=c11 -Wall -Werror`
- Test suite: compile and run generated C for each language feature
- Simple programs (hello world, fibonacci, basic game loop) work end to end

---

## Phase 5: Runtime Library

A small C library (~500-1000 lines) that ships with the compiler.

### Components

```
iron_runtime.h / iron_runtime.c

-- String
Iron_String struct (data, length, byte_length, flags)
Iron_String_create, Iron_String_concat, Iron_String_equals
Iron_String_substring, Iron_String_split, Iron_String_format
String interning table
Small string optimization

-- Collections
Iron_List (dynamic array with grow/shrink)
Iron_Map (hash map)
Iron_Set (hash set)

-- Reference Counting
Iron_Rc_create, Iron_Rc_retain, Iron_Rc_release

-- Optional/Nullable
Iron_Optional struct (value, has_value)

-- Threading
Iron_Pool (thread pool with work queue)
Iron_Channel (ring buffer + mutex + condvars)
Iron_Mutex (pthread_mutex wrapping a value)
Iron_Handle (spawn result with done flag + condvar)
pool_submit, pool_barrier
channel_send, channel_recv, channel_try_recv
mutex_lock

-- Built-in functions
Iron_print, Iron_println
Iron_len, Iron_range
Iron_min, Iron_max, Iron_clamp, Iron_abs
Iron_assert
```

### Deliverable

- `iron_runtime.c` / `iron_runtime.h` that compiles on Linux and macOS
- Unit tests for each runtime component
- Thread safety tests for concurrency primitives

---

## Phase 6: Standard Library Modules

Implement the importable standard library modules.

### `math`

```
sin, cos, tan, asin, acos, atan2
floor, ceil, round, sqrt, pow, lerp, sign
random, random_int, random_float, seed
PI, TAU, E
```

Maps mostly to `<math.h>` and a simple RNG.

### `io`

```
read_file, read_bytes, write_file, write_bytes
file_exists, list_files, create_dir, delete_file
```

Maps to `<stdio.h>`, `<dirent.h>`, `<sys/stat.h>`.

### `time`

```
now, now_ms, sleep, since
Timer object
```

Maps to `<time.h>`, `clock_gettime`, `nanosleep`.

### `log`

```
info, warn, error, debug
set_level
```

Formatted output to stderr with level filtering.

### Deliverable

- Each module implemented as `.c` / `.h` files
- Linked into the final binary by the compiler
- Test suite per module

---

## Phase 7: CLI Toolchain

The `iron` command-line tool.

### Commands

```
iron build [file]      Compile .iron files to binary
iron run [file]        Compile and run
iron check [file]      Type-check without compiling
iron fmt [file]        Format source code
iron test [dir]        Run tests
```

### Build Pipeline

```
1. Discover .iron files from entry point (main.iron)
2. Lex each file
3. Parse each file into AST
4. Resolve imports, build module graph
5. Semantic analysis (all modules)
6. Generate C code (single .c file with all modules inlined)
7. Invoke gcc/clang to compile C + runtime into a standalone binary
   gcc -std=c11 -O2 -o output generated.c iron_runtime.c iron_stdlib.c -lpthread
8. Output a single standalone executable (no runtime dependencies)
```

The Iron runtime and stdlib are statically compiled into the executable. The binary requires no Iron installation to run. System libraries (graphics, audio, windowing) are dynamically linked as is standard for game binaries on all platforms.

### Deliverable

- `iron` CLI that takes `.iron` files and produces standalone executables
- Error messages with file, line, column, and context
- Colored terminal output
- `--verbose` flag to show generated C code
- `iron build` produces a single binary ready to distribute

---

## Phase 8: Compile-Time Evaluation

### Approach

- After semantic analysis, identify `comptime` call sites
- Run a mini interpreter on the annotated AST for those expressions
- Replace the `comptime` expression with the computed literal in the AST
- Then proceed to C code generation as normal

### Restrictions enforced

- No heap, free, rc
- No runtime API calls
- No I/O except `read_file` (which reads at compile time)
- Must terminate (optionally: add a step limit to prevent infinite loops)

### Deliverable

- Comptime interpreter that handles: math, loops, conditionals, function calls, arrays, strings
- `comptime read_file(...)` embeds file contents as string/byte array literals
- Test suite for comptime evaluation edge cases

---

## Implementation Language

The compiler itself will be written in **C**. This keeps the toolchain self-contained and avoids bootstrap complexity. The phases can also be implemented in another language (Rust, Go, Python) for prototyping, then rewritten in Iron once the language is self-hosting.

### Suggested approach

1. **Prototype in Python** — fastest way to iterate on lexer, parser, and codegen
2. **Rewrite in C** — for performance and self-containment
3. **Bootstrap in Iron** — rewrite the compiler in Iron once it's capable enough

---

## Milestones

| Milestone | What works | Target |
|---|---|---|
| M1: Hello World | Lexer + parser + codegen for `print("hello")` | Week 2 |
| M2: Functions | Functions, variables, arithmetic, control flow | Week 4 |
| M3: Objects | Object declarations, methods, construction | Week 6 |
| M4: Memory | heap, free, defer, auto-free, leak, rc | Week 8 |
| M5: Types | Generics, nullable, type inference, interfaces | Week 10 |
| M6: Concurrency | spawn, await, channels, mutex, parallel for | Week 12 |
| M7: Stdlib | Collections, math, io, time, log | Week 14 |
| M8: Comptime | Compile-time evaluation | Week 16 |
| M9: Self-hosting | Compiler rewritten in Iron | Stretch goal |

---

## Testing Strategy

### Unit tests

- Lexer: token output for each token type, error cases
- Parser: AST structure for each grammar rule
- Type checker: valid programs pass, invalid programs produce specific errors
- Codegen: generated C compiles and produces expected output

### Integration tests

- End-to-end: `.iron` file → compile → run → check stdout
- One test per language feature
- Error message tests: specific diagnostics for specific mistakes

### Test format

```
tests/
  lexer/
    test_keywords.iron
    test_strings.iron
    test_numbers.iron
  parser/
    test_objects.iron
    test_functions.iron
    test_control_flow.iron
  codegen/
    test_hello.iron          -- expected output: "hello"
    test_fibonacci.iron      -- expected output: "55"
    test_memory.iron         -- expected: no leaks (valgrind)
  errors/
    test_type_mismatch.iron  -- expected error: "type mismatch on line 3"
    test_null_access.iron    -- expected error: "cannot access nullable"
```

---

## File Structure

```
iron-lang/
  docs/
    language_definition.md
    implementation_plan.md
  src/
    lexer/
    parser/
    analyzer/
    codegen/
    runtime/
    stdlib/
    cli/
  tests/
    lexer/
    parser/
    codegen/
    errors/
    integration/
  examples/
    hello.iron
    game.iron
    concurrency.iron
  LICENSE
  README.md
```
