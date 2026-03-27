# Phase 6: Milestone Gap Closure - Research

**Researched:** 2026-03-26
**Domain:** Iron compiler internals — builtin registration, stdlib Iron wrappers, CLI check command
**Confidence:** HIGH

## Summary

Phase 6 closes three narrowly-scoped gaps that prevent 3 of the 52 v1.0 requirements from being
fully satisfied. Each gap has a well-understood root cause: (1) `range` was never registered in
`resolve.c` or implemented in `iron_builtins.c`; (2) `time.iron` is missing the Timer object and
its three methods; (3) `check.c` does not replicate the `strstr`-based stdlib import detection and
prepend logic that `build.c` already has.

Every fix is mechanical and self-contained — no new AST nodes, no new compiler passes, no new
architecture. The patterns for each fix already exist elsewhere in the codebase. The implementation
is copy-adapt from existing, proven code.

**Primary recommendation:** Follow existing patterns exactly. Add `range` exactly like `abs/clamp`
were added. Add Timer to `time.iron` exactly like `Math` methods live in `math.iron`. Copy the
`strstr`/prepend blocks from `build.c` into `check.c` verbatim.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| RT-07 | Built-in functions work: print, println, len, range, min, max, clamp, abs, assert | `range` needs registration in `resolve.c` (like `abs`) and C implementation in `iron_builtins.c` (like `Iron_abs`), plus codegen dispatch in `gen_exprs.c` (like `Iron_clamp`) |
| STD-03 | time module provides now, now_ms, sleep, since, Timer | `iron_time.c` already has `Iron_timer_create`, `Iron_timer_since`, `Iron_timer_reset` and `Iron_time_since` — only `time.iron` wrapper is missing (`Timer` object + 3 methods + `Time.since`) |
| CLI-03 | `iron check [file]` type-checks without compiling to binary | `check.c` already does lex+parse+analyze correctly; it just needs the same `strstr`/prepend block that `build.c` has at steps 1c–1g |
</phase_requirements>

## Standard Stack

### Core
| Component | Location | Purpose |
|-----------|----------|---------|
| `iron_builtins.c` | `src/runtime/iron_builtins.c` | C implementations of Iron built-in functions |
| `iron_runtime.h` | `src/runtime/iron_runtime.h` | Declarations for built-ins (must stay in sync) |
| `resolve.c` | `src/analyzer/resolve.c` | Registers built-in symbols in global scope before Pass 1a |
| `gen_exprs.c` | `src/codegen/gen_exprs.c` | Codegen dispatch: `strcmp(callee_id->name, "range")` pattern |
| `time.iron` | `src/stdlib/time.iron` | Iron-level wrapper for the time stdlib module |
| `iron_time.h` | `src/stdlib/iron_time.h` | C API for time (already complete) |
| `check.c` | `src/cli/check.c` | `iron check` implementation |
| `build.c` | `src/cli/build.c` | Reference for `make_src_path` + `strstr`/prepend pattern |

No new libraries. No new dependencies. All changes are in existing files.

## Architecture Patterns

### Pattern 1: Registering a new built-in function

Three places must change in lockstep: declaration header, C implementation, resolver registration,
and codegen dispatch.

**Step A — Declare in `iron_runtime.h`:**
```c
// Source: src/runtime/iron_runtime.h (existing declarations at line 112-119)
int64_t Iron_range(int64_t n);
```

**Step B — Implement in `iron_builtins.c`:**
```c
// Source: src/runtime/iron_builtins.c (follow pattern of Iron_abs at line 40)
int64_t Iron_range(int64_t n) {
    return n;  /* range(n) returns n; used as upper bound in for-loop iterable */
}
```

The codegen already emits `for (int64_t i = 0; i < <iterable>; i++)` for `for i in <expr>`.
Passing `Iron_range(10)` as the iterable is identical to passing `10` directly. The identity
implementation is correct and consistent with the integer-range semantics the compiler already
relies on.

**Step C — Register in `resolve.c`:**
```c
// Source: src/analyzer/resolve.c (follow pattern starting at line 658)
/* range(Int) -> Int */
{
    Iron_Type *params[1] = { int_t };
    Iron_Type *fn = iron_type_make_func(arena, params, 1, int_t);
    Iron_Symbol *sym = iron_symbol_create(arena, "range",
                                           IRON_SYM_FUNCTION, NULL, no_span);
    sym->type = fn;
    iron_scope_define(ctx.global_scope, arena, sym);
}
```

**Step D — Dispatch in `gen_exprs.c`:**
```c
// Source: src/codegen/gen_exprs.c (follow pattern at line 691 for abs)
if (strcmp(callee_id->name, "range") == 0 && call->arg_count == 1) {
    iron_strbuf_appendf(sb, "Iron_range(");
    emit_expr(sb, call->args[0], ctx);
    iron_strbuf_appendf(sb, ")");
    break;
}
```

### Pattern 2: Adding Timer wrapper to time.iron

The `iron_time.c` C implementation is already complete — `Iron_timer_create`, `Iron_timer_since`,
`Iron_timer_reset`, and `Iron_time_now_ms` all exist. The `time.iron` wrapper just needs to declare
the `Timer` object and its methods, plus a `Time.since` method.

**Reference — existing time.iron (3 lines):**
```
-- time.iron
object Time {}
func Time.now() -> Float {}
func Time.now_ms() -> Int {}
func Time.sleep(ms: Int) {}
```

**Reference — iron_time.h signatures:**
```c
// Source: src/stdlib/iron_time.h
typedef struct { int64_t start_ms; } Iron_Timer;
Iron_Timer Iron_timer_create(void);
int64_t    Iron_timer_since(const Iron_Timer *t);
void       Iron_timer_reset(Iron_Timer *t);
```

**Reference — math.iron for method dispatch pattern:**
```
-- math.iron (shows func Object.method() pattern used by codegen)
object Math {}
func Math.sin(x: Float) -> Float {}
```

The codegen auto-static dispatch (Phase 5 decision: `IRON_SYM_TYPE` check emits `Iron_math_sin`
pattern) will map `Timer.create()` to `Iron_timer_create`, `Timer.since(t)` to `Iron_timer_since`,
and `Timer.reset(t)` to `Iron_timer_reset`.

**Key question for planner:** What does `Time.since` accept? The audit says "Timer.create(),
Timer.since(), Timer.reset()" are the missing wrappers. The `iron_time.h` has
`Iron_timer_since(const Iron_Timer *t)` — this is a Timer method. STD-03 requirement text says
"since, Timer". The `iron_time.c` has no standalone `Iron_time_since`. The Time.since wrapper
should likely proxy `Iron_time_now_ms()` or be omitted in favor of Timer methods. The
implementation plan should add: `Timer` object + `Timer.create()`, `Timer.since(t: Timer) -> Int`,
`Timer.reset(t: Timer)`. Do not invent `Time.since` if there is no C backing for it — use only
what `iron_time.h` exposes.

### Pattern 3: Replicating stdlib prepend in check.c

`build.c` already has the complete `strstr`-based import detection and `read_file`+prepend logic
at steps 1c through 1g (lines 547–658). `check.c` needs the same steps applied to the raw source
text before passing it to the lexer.

**check.c current structure:**
```c
// Source: src/cli/check.c (lines 45-110)
int iron_check(const char *source_path, bool verbose) {
    char *source = check_read_file(source_path);  // line 47
    // ... immediately goes to lexer at line 55
    Iron_Lexer lexer = iron_lexer_create(source, ...);
```

**build.c reference pattern (simplified):**
```c
// Source: src/cli/build.c (lines 547-658)
#ifndef IRON_SOURCE_DIR
#define IRON_SOURCE_DIR "src"
#endif

static char *make_src_path(const char *rel) { ... }
static char *read_file(const char *path, long *out_size) { ... }

/* In iron_build(): after reading source, before lexing: */
if (strstr(source, "import math") != NULL) {
    char *math_path = make_src_path("stdlib/math.iron");
    // ... read, prepend, replace source pointer
}
// repeat for io, time, log, raylib
```

**Changes needed in check.c:**
1. Add `#ifndef IRON_SOURCE_DIR` / `#define IRON_SOURCE_DIR "src"` guard (already in build.c)
2. Add local `make_src_path()` static helper (or share via a header — but duplicating as static is
   the existing project pattern, see build.c which has its own static helpers)
3. Update `check_read_file` to accept `out_size` OR just use `strlen(source)` after reading
4. After reading source, add the same `strstr`/prepend blocks for math, io, time, log (raylib is
   not needed for check — build.c's raylib block adds a clang compilation flag, irrelevant here;
   but raylib.iron prepend is still needed for symbol resolution correctness)

### Pattern 4: check.c `make_src_path` scope

`make_src_path` is currently a `static` function inside `build.c`. It is NOT declared in `build.h`.
The simplest approach for `check.c` is to duplicate it as a second `static` function — this matches
the existing project pattern where both files are self-contained CLI command implementations.
Alternatively, extract to a shared `cli/common.c` — but that adds a new file and CMakeLists change.
**Recommended:** duplicate the static helper in check.c to avoid new files.

### Anti-Patterns to Avoid

- **Do not create a new `IRON_NODE_RANGE` AST node** — `range(n)` is just a builtin function call
  that returns `int64_t`. The for-loop codegen already treats the iterable as an integer bound.
- **Do not modify the parser for range** — it parses as a normal `IRON_NODE_CALL`.
- **Do not add a new shared header for `make_src_path`** — duplicate the static helper in check.c
  to stay consistent with the project's self-contained CLI file pattern.
- **Do not add `Time.since` without a C backing** — check iron_time.h before writing the wrapper;
  only expose what is implemented in C.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Stdlib file discovery | Custom path resolver | `make_src_path(IRON_SOURCE_DIR)` | Already in build.c, CMake bakes the path at configure time |
| String prepend logic | New concatenation utility | Direct malloc+memcpy pattern from build.c | Proven, tested, simple |
| Timer object layout | New struct in Iron runtime | `Iron_Timer` in `iron_time.h` | Already exists with full C implementation |

## Common Pitfalls

### Pitfall 1: Missing `IRON_SOURCE_DIR` in check.c
**What goes wrong:** `make_src_path` builds from `IRON_SOURCE_DIR`. If check.c doesn't have the
`#ifndef IRON_SOURCE_DIR / #define IRON_SOURCE_DIR "src"` guard, the path will be a relative "src"
which only works when `iron` is run from the project root.
**Why it happens:** build.c has the guard; check.c currently has no path-building logic at all.
**How to avoid:** Copy the exact guard from build.c line 22-24.
**Warning signs:** `iron check` works from project root but fails from other directories.

### Pitfall 2: Source pointer leak after prepend
**What goes wrong:** After prepending a stdlib .iron file, the original `source` pointer is freed
and replaced with `combined`. If an early-return path between prepend steps doesn't free all
allocated buffers, there will be memory leaks.
**Why it happens:** The prepend loop in build.c frees each `source` only after successful combined
allocation. Check.c needs the same careful sequencing.
**How to avoid:** Follow build.c's pattern exactly — free the old `source` inside the `if (combined)`
block, just like build.c does.

### Pitfall 3: range() type mismatch in for-loop
**What goes wrong:** If `range` is registered with wrong return type (e.g., `List[Int]` instead of
`Int`), the type-checker will emit a type mismatch error when `range(10)` is used as a for-loop
iterable, because the for-loop codegen expects an integer bound.
**Why it happens:** Some languages (Python) return iterables from range; Iron's codegen uses an
integer bound directly.
**How to avoid:** Register `range(Int) -> Int` — the identity function. The for-loop codegen emits
`for (int64_t i = 0; i < Iron_range(10); i++)` which is correct.

### Pitfall 4: Timer method dispatch — pointer vs value
**What goes wrong:** `Iron_timer_since` takes `const Iron_Timer *t` (pointer) but Iron objects are
passed by value. The codegen may emit a value call where a pointer is needed.
**Why it happens:** The Time methods `now`, `now_ms`, `sleep` are stateless and take no Timer arg.
Timer methods do take a Timer argument.
**How to avoid:** Check how Phase 5 codegen handles method dispatch for the math module — math
methods take scalar Float arguments, not pointers. Timer is a struct, so the codegen may need a
`&` address-of to pass it. Review `gen_exprs.c` method dispatch code before writing the wrapper.
This is the one area of non-trivial uncertainty in the phase.

### Pitfall 5: Duplicate symbol when both time.iron defines Timer and user code defines Timer
**What goes wrong:** If the user file also contains a `Timer` type, prepending time.iron causes a
duplicate declaration error.
**Why it happens:** build.c has the same potential issue — it's accepted as a v1 limitation.
**How to avoid:** No action needed; document as known behavior.

## Code Examples

### range builtin — resolver registration
```c
// src/analyzer/resolve.c — insert after abs block (line ~718), before read_file block
/* range(Int) -> Int — identity function; for-loop iterable bound */
{
    Iron_Type *params[1] = { int_t };
    Iron_Type *fn = iron_type_make_func(arena, params, 1, int_t);
    Iron_Symbol *sym = iron_symbol_create(arena, "range",
                                           IRON_SYM_FUNCTION, NULL, no_span);
    sym->type = fn;
    iron_scope_define(ctx.global_scope, arena, sym);
}
```

### range builtin — C implementation
```c
// src/runtime/iron_builtins.c — add after Iron_assert
int64_t Iron_range(int64_t n) {
    return n;
}
```

### range builtin — header declaration
```c
// src/runtime/iron_runtime.h — add after Iron_assert declaration (line ~119)
int64_t Iron_range(int64_t n);
```

### range builtin — codegen dispatch
```c
// src/codegen/gen_exprs.c — add after abs block (line ~695), before assert block
if (strcmp(callee_id->name, "range") == 0 && call->arg_count == 1) {
    iron_strbuf_appendf(sb, "Iron_range(");
    emit_expr(sb, call->args[0], ctx);
    iron_strbuf_appendf(sb, ")");
    break;
}
```

### time.iron additions
```
-- src/stdlib/time.iron — add after existing 3 lines
object Timer { val start_ms: Int }

func Timer.create() -> Timer {}
func Timer.since(t: Timer) -> Int {}
func Timer.reset(t: Timer) {}
```

### check.c stdlib prepend additions
```c
// src/cli/check.c — add before #include "lexer/lexer.h"
#ifndef IRON_SOURCE_DIR
#define IRON_SOURCE_DIR "src"
#endif

// Add static helper:
static char *check_make_src_path(const char *rel) {
    size_t base_len = strlen(IRON_SOURCE_DIR);
    size_t rel_len  = strlen(rel);
    char *out = (char *)malloc(base_len + 1 + rel_len + 1);
    if (!out) return NULL;
    memcpy(out, IRON_SOURCE_DIR, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, rel, rel_len + 1);
    return out;
}

// In iron_check(), after reading source and before lexing,
// add the same strstr/prepend blocks from build.c (1d through 1g):
if (strstr(source, "import math") != NULL) { /* ... prepend math.iron ... */ }
if (strstr(source, "import io")   != NULL) { /* ... prepend io.iron ...   */ }
if (strstr(source, "import time") != NULL) { /* ... prepend time.iron ... */ }
if (strstr(source, "import log")  != NULL) { /* ... prepend log.iron ...  */ }
```

### Integration test for range (new .iron file)
```iron
-- tests/integration/test_range.iron
func main() {
  val sum = 0
  for i in range(5) {
    -- sum = sum + i  -- note: Iron v1 may not have += yet; use var
  }
  println("range works")
}
```

### Integration test for timer (extend test_time.iron)
```iron
-- tests/integration/test_time.iron (updated)
import time

func main() {
  val ms = Time.now_ms()
  val t = Timer.create()
  val elapsed = Timer.since(t)
  println("time works")
}
```

### Integration test for iron check with import
```bash
# Verify iron check succeeds on a file that imports math
echo 'import math\nfunc main() { val x = Math.sin(0.0) }' > /tmp/test_check_math.iron
iron check /tmp/test_check_math.iron
# Must exit 0
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| check.c with no stdlib support | check.c with same strstr/prepend as build.c | Phase 6 | iron check works on any Iron file that iron build works on |
| time.iron has 3 methods | time.iron has 6+ methods (Timer object + 3 methods) | Phase 6 | Timer usable from Iron source |
| range missing from resolver | range registered as builtin | Phase 6 | RT-07 fully satisfied |

## Open Questions

1. **Timer method pointer vs value dispatch**
   - What we know: `Iron_timer_since` takes `const Iron_Timer *t`; Iron object args are typically
     passed by value in the codegen
   - What's unclear: Does the existing method dispatch in `gen_exprs.c` automatically take address
     of struct arguments, or does the planner need to use `var` (mutable) Timer and check how
     pointer args are emitted?
   - Recommendation: The planner should read `gen_exprs.c` method call section (around the
     `IRON_NODE_CALL` case) and check if there is existing logic for struct-by-pointer. If not,
     the simplest fix is to implement `Iron_timer_since_val(Iron_Timer t)` and
     `Iron_timer_reset_val(Iron_Timer *t)` value-based wrappers in iron_time.c, callable without
     address-of. This stays consistent with how math functions take `double` not `double *`.

2. **`Time.since` — does it exist at the C level?**
   - What we know: `iron_time.h` has no `Iron_time_since` function. The audit gap says
     "Timer/since" together. The audit evidence says "Timer.create(), Timer.since(),
     Timer.reset() — no Iron wrapper". There is no `Iron_time_since` in iron_time.c.
   - What's unclear: Is `Time.since` a distinct function or is it the same as `Timer.since`?
   - Recommendation: Do not add `Time.since` unless a C backing is created. Implement
     `Timer.create`, `Timer.since`, `Timer.reset`. Add `Iron_time_since(int64_t start_ms) -> int64_t`
     to iron_time.c if needed for a standalone Time.since.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Unity (C unit tests) + bash integration runner |
| Config file | CMakeLists.txt |
| Quick run command | `cd build && ctest -R "test_stdlib\|test_codegen\|test_resolver\|test_typecheck" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements → Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| RT-07 | `range(10)` resolves in analyzer | unit | `ctest -R test_resolver -x` | ✅ Wave 0: add test case |
| RT-07 | `range(10)` emits `Iron_range(10)` in codegen | unit | `ctest -R test_codegen -x` | ✅ Wave 0: add test case |
| RT-07 | `for i in range(5) {}` compiles and runs | integration | `tests/integration/run_integration.sh` | ❌ Wave 0: test_range.iron |
| STD-03 | `Timer.create()`, `Timer.since(t)`, `Timer.reset(t)` callable | integration | `tests/integration/run_integration.sh` | ❌ Wave 0: update test_time.iron |
| CLI-03 | `iron check file_with_import_math.iron` exits 0 | integration | `iron check /tmp/test_check_math.iron` | ❌ Wave 0: ad-hoc check test |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R "test_resolver|test_codegen|test_stdlib|test_typecheck" --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/integration/test_range.iron` + `test_range.expected` — covers RT-07 integration
- [ ] Updated `tests/integration/test_time.iron` — covers STD-03 (Timer methods)
- [ ] Inline check test in verification script or CLI test — covers CLI-03

## Sources

### Primary (HIGH confidence)
- `src/analyzer/resolve.c` lines 630-728 — existing builtin registration pattern, verified by direct read
- `src/codegen/gen_exprs.c` lines 632-710 — existing codegen dispatch pattern, verified by direct read
- `src/runtime/iron_builtins.c` — existing C builtin implementations, verified by direct read
- `src/runtime/iron_runtime.h` lines 112-119 — builtin declarations, verified by direct read
- `src/stdlib/time.iron` — current state (3 methods only), verified by direct read
- `src/stdlib/iron_time.h` — C API (all Timer functions present), verified by direct read
- `src/cli/build.c` lines 254-265, 547-658 — `make_src_path` and prepend pattern, verified by direct read
- `src/cli/check.c` — current state (no stdlib prepend), verified by direct read
- `.planning/v1.0-MILESTONE-AUDIT.md` — audit gap evidence, verified by direct read

### Secondary (MEDIUM confidence)
- Phase 5 decision log in STATE.md: "auto-static dispatch by IRON_SYM_TYPE check emits Iron_math_sin
  pattern" — confirms how object method dispatch works in codegen

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all relevant files read directly
- Architecture: HIGH — exact code patterns confirmed from source
- Pitfalls: HIGH — based on direct analysis of existing code structure
- Timer dispatch uncertainty: MEDIUM — pointer-vs-value question requires planner to check gen_exprs.c

**Research date:** 2026-03-26
**Valid until:** 2026-04-25 (stable domain; no external dependencies)
