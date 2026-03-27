# Phase 7: IR Foundation - Research

**Researched:** 2026-03-27
**Domain:** C IR data structures (SSA form), IR printing, IR verification
**Confidence:** HIGH

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**IR Text Format (printer output)**
- LLVM-style syntax: `%0 = add %1, %2 : Int`
- Full Iron type names in signatures and instructions (Int, Float, Bool, Player, List[Int], heap Player, rc Enemy, Player?) -- NOT C-mapped types
- No raw pointer syntax (`*T`) -- Iron doesn't have pointers; use `heap T`, `rc T`
- Named basic blocks derived from source: `entry`, `if.then`, `if.else`, `while.header`, `while.body`, `for_header`, `for_body`, `for_exit`, `base_case`, `recurse`
- Function signatures: `func @Iron_main(x: Int, y: Float) -> Bool { ... }`
- Module-level declarations section at top: types, externs, globals, then functions
- Annotations on by default (auto_free flags, escapes flags, [deferred] markers, source spans as trailing comments)
- Optional `--no-annotations` flag to hide all annotations for clean output

**Instruction Set Design**
- Single flat `IronIR_InstrKind` enum with ~40 variants, grouped by comment sections (constants, arithmetic, comparison, unary, memory, control flow, high-level)
- Per-kind structs inside a tagged union -- each instruction kind gets its own named struct in the union (not a shared ops[3] array)
- Common fields on every instruction: kind, id (IrValueId), type (Iron_Type*), span (Iron_Span)
- High-level constructs kept as single opcodes: IRON_IR_MAKE_CLOSURE, IRON_IR_SPAWN, IRON_IR_PARALLEL_FOR, IRON_IR_AWAIT, IRON_IR_INTERP_STRING, IRON_IR_HEAP_ALLOC, IRON_IR_RC_ALLOC, IRON_IR_CONSTRUCT -- C emitter handles expansion
- IRON_IR_SWITCH terminator for match/enum (enum variants mapped to integer discriminants)
- For loops lowered to CFG blocks (header/body/exit with counter via alloca), NOT a high-level for instruction
- Parallel-for stays as single IR_PARALLEL_FOR opcode (high-level, emitter expands)

**Naming Conventions**
- Type prefix: `IronIR_` -- IronIR_Module, IronIR_Func, IronIR_Block, IronIR_Instr, IronIR_ValueId, IronIR_BlockId
- Enum prefix: `IRON_IR_` -- IRON_IR_CONST_INT, IRON_IR_ADD, IRON_IR_BRANCH, IRON_IR_CALL
- Function prefix: `iron_ir_` -- iron_ir_module_create(), iron_ir_print(), iron_ir_verify(), iron_ir_lower()
- Matches existing codebase convention: Iron_ for types, IRON_ for enums, iron_ for functions

**File Organization**
- Split by concern with separate .h/.c pairs:
  - `src/ir/ir.h` + `ir.c` -- all data structures and constructors
  - `src/ir/lower.h` + `lower.c` -- AST to IR (Phase 8, empty stubs in Phase 7)
  - `src/ir/emit_c.h` + `emit_c.c` -- IR to C text (Phase 9, empty stubs in Phase 7)
  - `src/ir/print.h` + `print.c` -- human-readable IR printer
  - `src/ir/verify.h` + `verify.c` -- structural verifier
- Tests in `tests/ir/` subdirectory with separate files per concern

### Claude's Discretion
- Exact set of ~40 instruction kind variants (Claude determines the full enum from requirements)
- IrValueId and IrBlockId integer width (uint32_t vs uint16_t)
- Arena allocation strategy details (dedicated ir_arena sizing, growth policy)
- Verifier error message format and reporting mechanism
- Helper macros for common instruction creation patterns

### Deferred Ideas (OUT OF SCOPE)

None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| IRCORE-01 | IR data structures defined (IrValue, IrInstr, IrBasicBlock, IrFunction, IrModule) with SSA-form semantics | Per-kind union struct layout documented in Standard Stack; naming conventions from CONTEXT.md locked |
| IRCORE-02 | IR instructions use integer value IDs (not pointers) for operand references | IronIR_ValueId as uint32_t; per-function value_table array for O(1) lookup; pattern verified by Cranelift, Julia IR |
| IRCORE-03 | IR carries full Iron type system (Iron_Type*) without introducing IR-specific types | Iron_Type* reused directly from src/analyzer/types.h; no IrType layer needed |
| IRCORE-04 | Each IR instruction carries an Iron_Span for source location tracking | Iron_Span is a by-value struct (5 fields); carried as third field in every IronIR_Instr; span constructors already exist |
| TOOL-01 | IR printer producing human-readable text dump of IR modules | LLVM-style format locked; Iron_StrBuf already available; print.h/print.c in src/ir/ |
| TOOL-02 | IR verifier validating structural invariants (values defined before use, blocks have terminators, branch targets valid) | Iron_DiagList already available for error reporting; verify.h/verify.c in src/ir/ |
</phase_requirements>

---

## Summary

Phase 7 builds the complete IR scaffold that phases 8 and 9 consume. The implementation is purely additive -- no existing files are modified except CMakeLists.txt. All design decisions that would be expensive to change later (struct field order, naming prefix, IrValueId width, Iron_Type* reuse, arena separation) are already locked by CONTEXT.md. The research task is to synthesize those decisions into exact struct layouts, a complete IrInstrKind enum, verifier invariant definitions, and printer format rules.

The primary technical concern in this phase is correctness of the data structure design, not algorithm complexity. The Braun SSA construction algorithm (Phase 8 concern) requires Braun state fields on IronIR_Block, so those fields must be reserved in the struct even though they are unused in Phase 7. The verifier needs Iron_DiagList access, which is already available. The printer needs Iron_StrBuf, also already available.

The biggest planning risk is the IronIR_Instr tagged union design. The CONTEXT locks "per-kind structs in the union," which is a slightly different shape than the shared-ops[3] approach in STACK.md. Per-kind structs mean each opcode group gets its own named member in the union with semantically named fields (not indexed operand slots). This is more verbose but matches the AST node pattern already established in ast.h and produces more self-documenting code.

**Primary recommendation:** Implement IronIR_Instr as a tagged union of named per-kind structs, with kind/id/type/span as the common prefix fields. Reserve Braun construction state (var_defs, incomplete_phis, is_sealed, is_filled) on IronIR_Block. All IR allocation from a dedicated ir_arena separate from ast_arena. Unit tests must construct modules by hand (no lowering needed) and assert on printer output and verifier pass/fail.

---

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| C17 | ISO/IEC 9899:2018 | Implementation language | Project-mandated; CMakeLists.txt sets CMAKE_C_STANDARD 17 |
| Iron_Arena | existing (src/util/arena.h) | Per-module bump allocator for all IR nodes | Same pattern as AST; iron_arena_create/alloc/free already available |
| stb_ds.h | vendored (src/vendor/stb_ds.h) | Dynamic arrays (arrput/arrlen/arrfree) and string hash maps (shput/shget) for per-block instruction lists and var_defs | Used throughout compiler; consistent pattern |
| Iron_StrBuf | existing (src/util/strbuf.h) | String builder for printer output | Same tool used by existing codegen and AST printer |
| Iron_DiagList | existing (src/diagnostics/diagnostics.h) | Verifier error collection | Same diagnostic infra used by lexer, parser, analyzer |
| Iron_Type* | existing (src/analyzer/types.h) | Type system reused directly | No IrType wrapper; iron_type_to_string() available for printer |
| Iron_Span | existing (src/diagnostics/diagnostics.h) | Source location on every IronIR_Instr | By-value struct; copied into IR fields; iron_span_make() available |
| Unity | v2.6.1 (fetched by CMake) | Unit test framework | Used for all existing compiler unit tests |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| ARENA_ALLOC macro | existing | Typed arena allocation: `ARENA_ALLOC(arena, T)` | Every IronIR_ struct creation |
| iron_type_to_string | existing | Convert Iron_Type* to human-readable string | Printer output for type annotations |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Per-kind union structs | Shared ops[3] array | ops[3] is denser but less readable; per-kind matches ast.h pattern and CONTEXT lock |
| uint32_t for IrValueId | uint16_t | uint16_t caps at 65535 instructions per function; uint32_t is safe for all real programs |
| Iron_Type* reuse | Separate IrType enum | IrType would require a translation layer and duplicate the semantic type system |

**Installation:** No new dependencies. Everything needed is already in the repo.

---

## Architecture Patterns

### Recommended Project Structure

```
src/ir/
├── ir.h         # IronIR_Module, IronIR_Func, IronIR_Block, IronIR_Instr,
│                #   IronIR_InstrKind enum, IronIR_ValueId, IronIR_BlockId
│                #   + all constructor / accessor declarations
├── ir.c         # Arena-backed constructors for all IR nodes
├── lower.h      # iron_ir_lower() public API stub (Phase 8 implementation)
├── lower.c      # Empty stub -- returns NULL with TODO comment
├── emit_c.h     # iron_ir_emit_c() public API stub (Phase 9 implementation)
├── emit_c.c     # Empty stub -- returns NULL with TODO comment
├── print.h      # iron_ir_print() declaration
├── print.c      # Human-readable IR text dump (LLVM-style)
├── verify.h     # iron_ir_verify() declaration
└── verify.c     # Structural verifier with Iron_DiagList error reporting

tests/ir/
├── test_ir_data.c     # Unit tests: construct IronIR_Module by hand, assert structure
├── test_ir_print.c    # Unit tests: assert printer output matches expected text
└── test_ir_verify.c   # Unit tests: well-formed IR passes; each invariant violation fails
```

### Pattern 1: Per-Kind Tagged Union for IronIR_Instr

**What:** IronIR_Instr has common prefix fields (kind, id, type, span), then a union of per-kind named structs. Each instruction opcode group gets its own named member with semantically named fields.

**When to use:** Always -- this is the locked design decision from CONTEXT.md.

**Example:**
```c
// Source: CONTEXT.md design decisions + ast.h pattern
typedef uint32_t IronIR_ValueId;
typedef uint32_t IronIR_BlockId;

#define IRON_IR_VALUE_INVALID 0
#define IRON_IR_BLOCK_INVALID 0

typedef struct IronIR_Instr {
    IronIR_InstrKind   kind;
    IronIR_ValueId     id;       /* 0 = instruction produces no value (store, jump, etc.) */
    Iron_Type         *type;     /* result type; NULL for void-result instructions */
    Iron_Span          span;     /* source location -- carried from AST node */

    union {
        /* IRON_IR_CONST_INT */
        struct { int64_t value; } const_int;
        /* IRON_IR_CONST_FLOAT */
        struct { double value; } const_float;
        /* IRON_IR_CONST_BOOL */
        struct { bool value; } const_bool;
        /* IRON_IR_CONST_STRING */
        struct { const char *value; } const_str;
        /* IRON_IR_CONST_NULL */
        struct { int _pad; } const_null; /* no payload needed */

        /* IRON_IR_ADD, IRON_IR_SUB, IRON_IR_MUL, IRON_IR_DIV, IRON_IR_MOD */
        /* IRON_IR_EQ, IRON_IR_NEQ, IRON_IR_LT, IRON_IR_LTE, IRON_IR_GT, IRON_IR_GTE */
        /* IRON_IR_AND, IRON_IR_OR */
        struct { IronIR_ValueId left; IronIR_ValueId right; } binop;

        /* IRON_IR_NEG, IRON_IR_NOT */
        struct { IronIR_ValueId operand; } unop;

        /* IRON_IR_ALLOCA */
        struct {
            Iron_Type  *alloc_type;
            const char *name_hint;  /* source variable name for readable printer output */
        } alloca;

        /* IRON_IR_LOAD */
        struct { IronIR_ValueId ptr; } load;

        /* IRON_IR_STORE */
        struct { IronIR_ValueId ptr; IronIR_ValueId value; } store;

        /* IRON_IR_GET_FIELD, IRON_IR_SET_FIELD */
        struct {
            IronIR_ValueId object;
            const char    *field;   /* Iron field name, not mangled C name */
            IronIR_ValueId value;   /* SET_FIELD only; ignored for GET_FIELD */
        } field;

        /* IRON_IR_GET_INDEX, IRON_IR_SET_INDEX */
        struct {
            IronIR_ValueId array;
            IronIR_ValueId index;
            IronIR_ValueId value;   /* SET_INDEX only */
        } index;

        /* IRON_IR_CALL */
        struct {
            Iron_FuncDecl    *func_decl;   /* for direct calls; NULL for indirect */
            IronIR_ValueId    func_ptr;    /* for indirect calls (lambda/fn pointer) */
            IronIR_ValueId   *args;        /* stb_ds array */
            int               arg_count;
        } call;

        /* IRON_IR_JUMP */
        struct { IronIR_BlockId target; } jump;

        /* IRON_IR_BRANCH */
        struct {
            IronIR_ValueId cond;
            IronIR_BlockId then_block;
            IronIR_BlockId else_block;
        } branch;

        /* IRON_IR_SWITCH */
        struct {
            IronIR_ValueId  subject;
            IronIR_BlockId  default_block;
            int            *case_values;   /* stb_ds array of discriminant ints */
            IronIR_BlockId *case_blocks;   /* stb_ds array, parallel to case_values */
            int             case_count;
        } sw;

        /* IRON_IR_RETURN */
        struct { IronIR_ValueId value; bool is_void; } ret;

        /* IRON_IR_CAST */
        struct { IronIR_ValueId value; Iron_Type *target_type; } cast;

        /* IRON_IR_HEAP_ALLOC */
        struct {
            IronIR_ValueId inner_val;
            bool           auto_free;
            bool           escapes;
        } heap_alloc;

        /* IRON_IR_RC_ALLOC */
        struct { IronIR_ValueId inner_val; } rc_alloc;

        /* IRON_IR_FREE */
        struct { IronIR_ValueId value; } free;

        /* IRON_IR_CONSTRUCT */
        struct {
            Iron_Type       *type;         /* resolved object type */
            IronIR_ValueId  *field_vals;   /* stb_ds array, in declaration order */
            int              field_count;
        } construct;

        /* IRON_IR_ARRAY_LIT */
        struct {
            Iron_Type       *elem_type;
            IronIR_ValueId  *elements;    /* stb_ds array */
            int              element_count;
        } array_lit;

        /* IRON_IR_SLICE */
        struct {
            IronIR_ValueId array;
            IronIR_ValueId start;  /* IRON_IR_VALUE_INVALID if omitted */
            IronIR_ValueId end;    /* IRON_IR_VALUE_INVALID if omitted */
        } slice;

        /* IRON_IR_IS_NULL, IRON_IR_IS_NOT_NULL */
        struct { IronIR_ValueId value; } null_check;

        /* IRON_IR_INTERP_STRING */
        struct {
            IronIR_ValueId *parts;   /* stb_ds array; mix of string consts and values */
            int             part_count;
        } interp_string;

        /* IRON_IR_MAKE_CLOSURE */
        struct {
            const char      *lifted_func_name;
            IronIR_ValueId  *captures;    /* stb_ds array */
            int              capture_count;
        } make_closure;

        /* IRON_IR_FUNC_REF (no-capture lambda) */
        struct { const char *func_name; } func_ref;

        /* IRON_IR_SPAWN */
        struct {
            const char    *lifted_func_name;
            IronIR_ValueId pool_val;        /* IRON_IR_VALUE_INVALID if default */
            const char    *handle_name;     /* NULL if no handle */
        } spawn;

        /* IRON_IR_PARALLEL_FOR */
        struct {
            const char    *loop_var_name;
            IronIR_ValueId range_val;
            const char    *chunk_func_name;
            IronIR_ValueId pool_val;        /* IRON_IR_VALUE_INVALID if default */
            IronIR_ValueId *captures;       /* stb_ds array */
            int             capture_count;
        } parallel_for;

        /* IRON_IR_AWAIT */
        struct { IronIR_ValueId handle; } await;

        /* IRON_IR_PHI (Braun SSA construction; may be present in Phase 7 structs) */
        struct {
            IronIR_ValueId  *values;       /* stb_ds array, parallel with pred_blocks */
            IronIR_BlockId  *pred_blocks;  /* stb_ds array */
            int              count;
        } phi;
    };
} IronIR_Instr;
```

### Pattern 2: IronIR_Block with Braun Construction State

**What:** IronIR_Block carries both the permanent IR data (instrs, preds, succs, label) and the ephemeral Braun SSA construction state (var_defs, incomplete_phis, is_sealed, is_filled). Construction state fields are reserved in Phase 7 even though Phase 7 never calls the SSA algorithm.

**When to use:** Every basic block allocation.

**Example:**
```c
// Source: STACK.md Braun construction design + CONTEXT.md naming
typedef struct IronIR_Block {
    IronIR_BlockId   id;
    const char      *label;        /* e.g. "entry", "if.then", "while.body" */

    /* Instruction list -- stb_ds array of IronIR_Instr* (arena-allocated) */
    IronIR_Instr   **instrs;       /* stb_ds array */
    int              instr_count;  /* mirrors arrlen(instrs) */

    /* CFG edges */
    IronIR_BlockId  *preds;        /* stb_ds array */
    IronIR_BlockId  *succs;        /* stb_ds array */

    /* Braun SSA construction state (used in Phase 8, reserved here) */
    bool             is_sealed;    /* all predecessors are known */
    bool             is_filled;    /* terminator has been appended */
    /* Per-variable current definition map: var_name -> IronIR_ValueId */
    struct { char *key; IronIR_ValueId value; } *var_defs;    /* stb_ds string map */
    /* Incomplete phis for unsealed blocks: var_name -> IronIR_Instr* */
    struct { char *key; IronIR_Instr *value; } *incomplete_phis; /* stb_ds string map */
} IronIR_Block;
```

### Pattern 3: IronIR_Func with Per-Function Value Counter

**What:** Per-function value numbering (IDs reset to 0 per function). IronIR_Func owns a value_table stb_ds array indexed by IronIR_ValueId for O(1) lookup.

**Example:**
```c
// Source: STACK.md per-function numbering design
typedef struct IronIR_Func {
    const char       *name;           /* mangled name, e.g. "Iron_main" */
    Iron_Type        *return_type;    /* NULL for void */
    IronIR_Param     *params;         /* fixed array */
    int               param_count;
    bool              is_extern;
    const char       *extern_c_name;  /* NULL for non-extern */

    IronIR_Block    **blocks;         /* stb_ds array; blocks[0] is entry */
    int               block_count;

    /* Per-function ID counters */
    IronIR_ValueId    next_value_id;  /* monotonically increasing, starts at 1 */
    IronIR_BlockId    next_block_id;  /* monotonically increasing, starts at 1 */

    /* Value table: id -> defining IronIR_Instr* (O(1) lookup during emission) */
    IronIR_Instr    **value_table;    /* stb_ds array indexed by value id */

    Iron_Arena       *arena;          /* owning arena for this function's IR nodes */
} IronIR_Func;
```

### Pattern 4: IronIR_Module Top-Level Structure

**What:** Module owns the ir_arena, func array, type declaration list, extern declarations, and monomorphization registry.

**Example:**
```c
// Source: CONTEXT.md + codegen.h mono_registry pattern
typedef struct IronIR_Module {
    const char         *name;
    IronIR_TypeDecl   **type_decls;   /* stb_ds array: objects, enums, interfaces */
    int                 type_decl_count;
    IronIR_ExternDecl **extern_decls; /* stb_ds array */
    int                 extern_decl_count;
    IronIR_Func       **funcs;        /* stb_ds array; emission order = declaration order */
    int                 func_count;

    /* Monomorphization registry (matches Iron_Codegen.mono_registry pattern) */
    struct { char *key; bool value; } *mono_registry; /* stb_ds string hash map */

    /* Counters for unique naming of lifted functions (lambda, spawn, parallel) */
    int  lambda_counter;
    int  spawn_counter;
    int  parallel_counter;

    Iron_Arena  *arena;   /* dedicated IR arena; separate from ast_arena */
} IronIR_Module;
```

### Pattern 5: LLVM-Style Printer Format

**What:** Human-readable text dump with LLVM-inspired syntax. `%N = opcode %M, %K : TypeName`.

**Format rules (locked in CONTEXT.md):**
```
; Module header
module @filename

; Type declarations section
type @Iron_Player = { x: Int, y: Float }
type @Iron_Direction = enum { North = 0, South = 1, East = 2, West = 3 }
interface @Iron_Drawable = { draw: func(@Iron_Drawable_ref) -> Void }

; Extern declarations
extern @InitWindow(width: Int, height: Int, title: String) -> Void

; Function bodies
func @Iron_main(argc: Int) -> Int {
entry:                                          ; source: main.iron:1:1
  %0 = alloca Int                               ; name_hint: "x"
  %1 = const_int 42 : Int
  store %0, %1                                  ; [deferred: ...] (annotation)
  %2 = load %0 : Int
  %3 = add %2, %1 : Int
  ret %3
}

func @Iron_Player_update(self: heap Player, dt: Float) -> Void {
entry:                                          ; source: player.iron:10:1
  %0 = get_field self.x : Float
  ...
if.then:
  ...
if.else:
  ...
merge:
  jump merge
}
```

**Annotation mode:** When annotations are on (default), trailing comments show:
- `; auto_free` on IRON_IR_HEAP_ALLOC instructions
- `; escapes` on heap allocs that escape
- `; [deferred: call_expr]` on instructions that will be deferred
- `; source: filename:line:col` as trailing span comments

**`--no-annotations` flag:** Suppresses all trailing `;` annotation comments for clean output.

### Pattern 6: Verifier Invariants

**What:** iron_ir_verify() walks the module and checks structural invariants, reporting each violation via iron_diag_emit() into a passed Iron_DiagList.

**Invariants to check (all required by TOOL-02):**

1. **Every block has exactly one terminator** -- the last instruction must be JUMP/BRANCH/SWITCH/RETURN; no instruction before it may be a terminator
2. **Every terminator's branch target is a valid block ID** -- IDs must exist in the parent function's blocks array
3. **Every value used is defined before use** (use-before-def) -- for each operand IronIR_ValueId, must appear in value_table at an instruction that precedes the use in dominator order; for Phase 7 basic dominance check: ID < current instruction's ID within a block suffices as a simplification
4. **No instruction after a terminator** -- once a terminator appears, no more instructions in that block
5. **Entry block exists** -- every function has at least one block (blocks[0])
6. **Return type matches** -- RETURN instructions carry the correct type (or void) matching function return_type

**Verifier error codes:** Define new IRON_ERR codes in diagnostics.h for IR errors, e.g.:
```c
#define IRON_ERR_IR_MISSING_TERMINATOR    300
#define IRON_ERR_IR_INVALID_BRANCH_TARGET 301
#define IRON_ERR_IR_USE_BEFORE_DEF        302
#define IRON_ERR_IR_INSTR_AFTER_TERMINATOR 303
#define IRON_ERR_IR_NO_ENTRY_BLOCK        304
#define IRON_ERR_IR_RETURN_TYPE_MISMATCH  305
```

### Anti-Patterns to Avoid

- **Storing C type strings in IrInstr fields:** All types use Iron_Type*, never `"int64_t"` strings -- this violates Pitfall 3 from project research and makes a future LLVM backend impossible
- **Using the AST arena for IR nodes:** IR must allocate from its own `ir_arena`; mixing arenas creates lifetime confusion when the AST arena is freed (Pitfall 12)
- **Global value numbering:** Value IDs reset to 1 per function -- global IDs bloat arrays and make debug output unreadable (Pitfall 6)
- **ops[3] shared operand array:** CONTEXT.md locks per-kind structs; the ops[3] design from STACK.md research is NOT what was decided
- **Carrying callee by C name string:** IronIR_Instr.call must carry `Iron_FuncDecl*` (or equivalent), not a mangled name string -- the emitter derives the C name from the decl; storing raw C names defeats semantic context (Pitfall 13)

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Dynamic instruction lists per block | Custom growable array | stb_ds arrput/arrlen | Already vendored; used throughout compiler; realloc-based growth |
| Variable name to value ID map (Braun var_defs) | Custom hash map | stb_ds string hash map (shput/shget) | Already vendored; handles `char*` keys natively |
| String building for printer | Custom strcat loops | Iron_StrBuf (strbuf_appendf) | Already available; same tool used by all existing output paths |
| Diagnostic error collection for verifier | Custom error list | Iron_DiagList + iron_diag_emit() | Already available; consistent with analyzer/lexer/parser error patterns |
| Type-to-string for printer | Custom type serializer | iron_type_to_string(type, arena) | Already implemented in types.c; handles all 20+ type kinds |
| Arena-allocated struct creation | malloc/free individual nodes | ARENA_ALLOC(arena, T) macro | Already defined in arena.h; zero-overhead typed allocation |

---

## Common Pitfalls

### Pitfall 1: Forgetting Iron_Span on Every IronIR_Instr
**What goes wrong:** If span is omitted from the struct or from constructor functions, retrofitting it later requires updating every callsite in phase 7, 8, and 9.
**Why it happens:** Span feels like infrastructure, not correctness. Easy to defer.
**How to avoid:** `span` is the 4th field in IronIR_Instr (after kind, id, type). Every `iron_ir_instr_*_create()` function takes an `Iron_Span span` parameter. Every test passes a valid span (can use `iron_span_make("test.iron", 1, 1, 1, 10)`).
**Warning signs:** Any constructor function that does not take a span parameter.

### Pitfall 2: Using AST Arena Instead of Dedicated ir_arena
**What goes wrong:** If IR nodes are allocated from the same arena as the AST, freeing the AST arena (a future optimization) produces dangling pointers in the IR.
**How to avoid:** `iron_ir_module_create()` takes an `Iron_Arena *ir_arena` parameter (not `*arena`). Name the parameter distinctively. Document: "This arena must outlive the module. Do NOT pass the ast_arena."
**Warning signs:** `iron_ir_module_create(arena, ...)` -- the parameter named just "arena" without distinction.

### Pitfall 3: Storing C-Specific Data in IronIR_Instr Fields
**What goes wrong:** If call.func_name is a mangled C string, the IR is tied to C emission and cannot serve a future LLVM backend.
**How to avoid:** `IronIR_Instr.call.func_decl` carries `Iron_FuncDecl*` for direct calls. The emitter calls `iron_mangle_name()` at emission time. `IronIR_Instr.alloca.name_hint` is the Iron source name (not mangled).
**Warning signs:** Any `const char *c_name` field in IronIR_Instr that contains a mangled identifier.

### Pitfall 4: Forgetting stb_ds Array Fields in Structs Need Separate Free
**What goes wrong:** When the module is destroyed, stb_ds arrays inside IronIR_Instr (call.args, switch.case_values, etc.) must be arrfree'd before the arena is freed, or the stb_ds metadata block leaks.
**How to avoid:** `iron_ir_module_destroy()` walks all instructions with stb_ds array fields and calls arrfree on each. Document the two-phase teardown: (1) free stb_ds arrays, (2) iron_arena_free(ir_arena).
**Warning signs:** `iron_ir_module_destroy()` that only calls `iron_arena_free()` without a prior walk.

### Pitfall 5: Missing Printer Coverage for High-Level Opcodes
**What goes wrong:** If IRON_IR_MAKE_CLOSURE, IRON_IR_SPAWN, IRON_IR_PARALLEL_FOR print as `<unknown>`, debugging phase 8 lowering of concurrency features is very difficult.
**How to avoid:** The printer must handle every IronIR_InstrKind variant. Use a `default: assert(false && "unhandled IronIR_InstrKind in printer")` in the switch to catch gaps at test time.
**Warning signs:** Printer switch statement without a `default` assertion; any opcode printing as a generic fallback.

### Pitfall 6: Verifier Checking Use-Before-Def with Wrong Scope
**What goes wrong:** A naive check that operand ID < current instruction ID works within a single block but fails across blocks (an instruction in block B can validly use a value defined in block A that dominates B).
**How to avoid:** For Phase 7, limit the use-before-def check to within a single block (sequential ordering) and cross-block checks only verify that a used value ID exists somewhere in the function's value_table. Full dominance checking is Phase 8 concern (once the CFG is constructed from real programs).
**Warning signs:** Verifier rejecting valid cross-block value uses in hand-built test modules.

---

## Code Examples

Verified patterns from the existing codebase:

### Arena Allocation Pattern (from arena.h)
```c
// Source: src/util/arena.h
IronIR_Module *mod = ARENA_ALLOC(ir_arena, IronIR_Module);
IronIR_Instr  *instr = ARENA_ALLOC(ir_arena, IronIR_Instr);
IronIR_Block  *block = ARENA_ALLOC(ir_arena, IronIR_Block);
```

### stb_ds Array Pattern (from existing codegen.h usage)
```c
// Source: codegen.h + stb_ds.h patterns
// Append instruction to block
arrput(block->instrs, instr);
block->instr_count = arrlen(block->instrs);

// String hash map for var_defs
shput(block->var_defs, "x", val_id);
IronIR_ValueId v = shget(block->var_defs, "x");

// Free before arena teardown
arrfree(block->instrs);
shfree(block->var_defs);
shfree(block->incomplete_phis);
iron_arena_free(ir_arena);
```

### Iron_DiagList Error Reporting Pattern (from diagnostics.h)
```c
// Source: src/diagnostics/diagnostics.h
// Used in verifier to report structural violations
iron_diag_emit(
    diags, arena,
    IRON_DIAG_ERROR,
    IRON_ERR_IR_MISSING_TERMINATOR,
    block->instrs[0]->span,   /* span of first instruction as approximation */
    "basic block has no terminator instruction",
    "add a return, jump, or branch as the last instruction"
);
```

### Iron_Type* Reuse Pattern (from types.h)
```c
// Source: src/analyzer/types.h
// No new type creation needed -- reuse existing singletons
Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);

// Printer uses iron_type_to_string
const char *type_str = iron_type_to_string(instr->type, arena);
// Produces: "Int", "Bool", "Int?", "rc Player", "[Int]", etc.
```

### Unity Test Pattern (from existing tests)
```c
// Source: tests/test_codegen.c pattern
#include "unity.h"
#include "ir/ir.h"
#include "ir/print.h"
#include "ir/verify.h"

void setUp(void) {}
void tearDown(void) {}

void test_const_int_instr_prints_correctly(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    IronIR_Func   *fn  = iron_ir_func_create(mod, "Iron_main", NULL, 0,
                                              iron_type_make_primitive(IRON_TYPE_INT));
    IronIR_Block  *entry = iron_ir_block_create(fn, "entry");

    Iron_Span sp = iron_span_make("test.iron", 1, 1, 1, 10);
    IronIR_Instr *c = iron_ir_const_int(fn, entry, 42,
                                         iron_type_make_primitive(IRON_TYPE_INT), sp);

    TEST_ASSERT_EQUAL(IRON_IR_CONST_INT, c->kind);
    TEST_ASSERT_EQUAL(42, c->const_int.value);
    TEST_ASSERT_NOT_NULL(c->type);

    iron_arena_free(&ir_arena);
}
```

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| AST -> C direct (gen_stmts.c, gen_exprs.c) | AST -> IR -> C (this phase starts that transition) | v1.1 milestone | Full codegen replacement in phase 9; IR enables future LLVM backend |
| Phi-placement via dominance frontiers | Braun et al. 2013 incremental SSA | Design decision for v1.1 | Single-pass construction; no pre-built CFG needed; ~200 lines vs ~500+ |
| Instruction fields as shared ops[3] array | Per-kind named structs in union | CONTEXT.md decision | More verbose but self-documenting; matches ast.h pattern |

**Deprecated/outdated:**
- `ops[3]` shared operand array: STACK.md proposed this but CONTEXT.md locked the per-kind struct design -- do not implement the ops[3] pattern
- IR-specific type system (IrTypeKind enum): CONTEXT.md locks Iron_Type* reuse -- do not create a separate IrType

---

## Open Questions

1. **IrValueId width: uint32_t vs uint16_t**
   - What we know: uint16_t caps at 65535; a function with heavy string interpolation or complex generics could produce thousands of instructions
   - What's unclear: whether any real Iron program will exceed 65535 instructions per function
   - Recommendation: Use `uint32_t` (4 bytes). The 2x size cost vs uint16_t is negligible; the overflow risk is not worth taking. Document `typedef uint32_t IronIR_ValueId` in ir.h.

2. **ir_arena initial size**
   - What we know: An average Iron function produces perhaps 50-200 IR instructions; a complex program may have 50-100 functions; each IronIR_Instr is ~80-120 bytes
   - What's unclear: What "average" looks like for the raylib integration tests
   - Recommendation: `iron_arena_create(64 * 1024)` (64KB) for the module-level ir_arena. The arena doubles on overflow so this is a floor, not a cap. Document the choice with a comment.

3. **Verifier span for block-level errors (missing terminator)**
   - What we know: The verifier must report a span; a missing terminator has no instruction to point to
   - What's unclear: Best span to report for structural absences
   - Recommendation: Use the span of the last instruction in the block (if any), or the function declaration span if the block is empty. Document this as a known approximation.

4. **Whether lower.c and emit_c.c stubs should compile cleanly**
   - What we know: They must not cause CMake build failures in Phase 7
   - Recommendation: Stubs should `#include` their own headers and define the function body as `{ (void)module; return NULL; }` with a `/* TODO: Phase 8 */` comment. All parameters must be named or cast to void to satisfy `-Wunused-parameter -Werror`.

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Unity v2.6.1 (fetched by CMake FetchContent) |
| Config file | CMakeLists.txt -- each test_*.c becomes a separate add_executable target |
| Quick run command | `ctest -R test_ir --output-on-failure` (from build directory) |
| Full suite command | `ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| IRCORE-01 | IronIR_Module/Func/Block/Instr structs compile and are arena-allocatable | unit | `ctest -R test_ir_data` | Wave 0 |
| IRCORE-01 | IronIR_InstrKind enum has all required variants | unit | `ctest -R test_ir_data` | Wave 0 |
| IRCORE-02 | IronIR_ValueId is uint32_t; operands reference by ID not pointer | unit | `ctest -R test_ir_data` | Wave 0 |
| IRCORE-03 | IronIR_Instr.type is Iron_Type*; iron_type_make_primitive() types appear in printed IR | unit | `ctest -R test_ir_print` | Wave 0 |
| IRCORE-04 | Iron_Span field present on IronIR_Instr; span value survives round-trip | unit | `ctest -R test_ir_data` | Wave 0 |
| TOOL-01 | Printer output matches expected LLVM-style text for module with all instr kinds | unit | `ctest -R test_ir_print` | Wave 0 |
| TOOL-02 | Well-formed module passes verify; use-before-def triggers IRON_ERR_IR_USE_BEFORE_DEF | unit | `ctest -R test_ir_verify` | Wave 0 |
| TOOL-02 | Missing terminator triggers IRON_ERR_IR_MISSING_TERMINATOR | unit | `ctest -R test_ir_verify` | Wave 0 |
| TOOL-02 | Invalid branch target triggers IRON_ERR_IR_INVALID_BRANCH_TARGET | unit | `ctest -R test_ir_verify` | Wave 0 |

### Sampling Rate
- **Per task commit:** `ctest -R test_ir --output-on-failure` (all IR tests, < 5 seconds)
- **Per wave merge:** `ctest --output-on-failure` (full suite including existing tests)
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/ir/test_ir_data.c` -- covers IRCORE-01, IRCORE-02, IRCORE-04
- [ ] `tests/ir/test_ir_print.c` -- covers TOOL-01, IRCORE-03
- [ ] `tests/ir/test_ir_verify.c` -- covers TOOL-02
- [ ] `CMakeLists.txt` additions: `src/ir/*.c` to iron_compiler target; `tests/ir/test_ir_*.c` add_executable entries
- [ ] `tests/ir/` directory does not exist yet -- must be created

---

## Sources

### Primary (HIGH confidence)
- `src/parser/ast.h` -- Ground truth for all Iron_NodeKind variants and per-kind struct naming pattern; inspected directly
- `src/codegen/codegen.h` -- Iron_Codegen struct showing mono_registry, lambda_counter, defer_stacks patterns to carry forward to IronIR_Module
- `src/analyzer/types.h` -- Full Iron_Type* system (20+ type kinds) that IR reuses directly; iron_type_to_string() available
- `src/diagnostics/diagnostics.h` -- Iron_Span struct layout (5 fields, by-value); Iron_DiagList and iron_diag_emit() API
- `src/util/arena.h` -- ARENA_ALLOC macro, iron_arena_create/alloc/free API
- `.planning/phases/07-ir-foundation/07-CONTEXT.md` -- All locked design decisions
- `.planning/research/STACK.md` -- IrValueId integer scheme, IrBlock Braun state fields, IrFunc value_table design
- `.planning/research/FEATURES.md` -- Complete instruction set mapping; all AST node kinds to IR opcodes
- `.planning/research/ARCHITECTURE.md` -- Pipeline structure, component responsibilities, file layout
- `.planning/research/PITFALLS.md` -- 13 documented pitfalls with phase assignments; Pitfalls 1, 3, 4, 6, 7, 12, 13 most relevant to Phase 7
- `.planning/research/SUMMARY.md` -- Synthesized findings and phase ordering rationale
- `CMakeLists.txt` -- Exact add_executable / add_library / add_test patterns for new test targets

### Secondary (MEDIUM confidence)
- Braun et al. "Simple and Efficient Construction of Static Single-Assignment Form" (CC 2013): https://c9x.me/compile/bib/braun13cc.pdf -- SSA construction algorithm; IrBlock seal/fill lifecycle
- Cranelift CLIF IR reference: https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/docs/ir.md -- per-function value numbering, value_table pattern, block parameter design
- Julia SSA-form IR: https://docs.julialang.org/en/v1/devdocs/ssair/ -- flat array of instructions per block
- LLVM Kaleidoscope tutorial (mutable variables via alloca): https://releases.llvm.org/2.6/docs/tutorial/OCamlLangImpl7.html -- alloca+load+store for var bindings

### Tertiary (LOW confidence)
None -- all critical claims verified against official sources or the existing codebase directly.

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all tools already in repo; no new dependencies
- Architecture: HIGH -- direct inspection of existing patterns (ast.h, codegen.h) combined with locked CONTEXT.md decisions
- Pitfalls: HIGH -- derived from project research PITFALLS.md (13 documented pitfalls with root cause analysis) + direct codebase inspection
- Instruction set: HIGH -- FEATURES.md provides complete AST node -> IR opcode mapping; ~40 variants confirmed from all Iron_NodeKind variants

**Research date:** 2026-03-27
**Valid until:** 2026-04-27 (stable design; no external dependencies changing)
