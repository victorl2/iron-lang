# Phase 39: Module Completions (Math, IO, Time, Log) - Research

**Researched:** 2026-04-02
**Domain:** Iron stdlib C implementation — math.h wrappers, POSIX file/IO, accumulator Timer, log level gating
**Confidence:** HIGH

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Timer: Accumulator style `{ elapsed_ms: Int, duration_ms: Int }`, `update(dt)` takes Float seconds converted to ms internally, `time.Timer(2.0)` constructor, `done()` returns `elapsed_ms >= duration_ms`, `reset()` zeroes elapsed_ms returns void (not Timer)
- IO paths: Always forward slash separator on `io.join_path` — works on all platforms (Windows accepts `/`)
- IO binary: `read_bytes`/`write_bytes` DEFERRED — wait until UInt8 type is more fully supported
- IO stdin: `io.read_line()` returns empty string `""` at EOF — caller checks `len()`
- IO append: `io.append_file(path, content)` opens in append mode, writes content
- IO read_lines: `io.read_lines(path)` returns `List[String]` — can use existing `io.read_file()` + `s.split("\n")` or implement directly in C
- Log: Constants as object fields `Log.DEBUG = 0`, `Log.INFO = 1`, `Log.WARN = 2`, `Log.ERROR = 3`. `log.set_level(log.WARN)` — global persistent, applies for rest of program
- Math: All 10 new functions are `<math.h>` wrappers. `math.sign(x)` returns Int (-1, 0, 1). `math.seed(n)` seeds existing xorshift64 PRNG. `math.random_float(min, max)` uses `math.random() * (max - min) + min`

### Claude's Discretion
- Exact C implementation details for each function
- Whether `io.read_lines` uses split internally or reads line-by-line
- How Timer struct is represented in the C runtime (Iron_Timer typedef)
- Whether `time.since()` takes Float (seconds) or Timer argument

### Deferred Ideas (OUT OF SCOPE)
- `io.read_bytes()` / `io.write_bytes()` — deferred until UInt8/List[UInt8] is fully supported
- `io.watch_file()` — future phase (requires inotify/kqueue)
- `log.set_format()` — custom log format string, future enhancement
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| MATH-01 | `math.asin(x)` for inverse sine | `asin()` from `<math.h>` — already included in iron_math.h |
| MATH-02 | `math.acos(x)` for inverse cosine | `acos()` from `<math.h>` |
| MATH-03 | `math.atan2(y, x)` for two-argument arctangent | `atan2()` from `<math.h>` |
| MATH-04 | `math.sign(x)` returns -1, 0, or 1 as Int | Custom logic: `(x > 0) - (x < 0)` — not in `<math.h>` |
| MATH-05 | `math.seed(n)` seeds the RNG | Must set `s_global_rng.state` via new `Iron_math_seed` function; requires exposing state or adding setter |
| MATH-06 | `math.random_float(min, max)` float-range random | `Iron_math_random() * (max - min) + min` |
| MATH-07 | `math.log(x)` natural logarithm | `log()` from `<math.h>` — note: `log` conflicts with C name; wrapper `Iron_math_log` is fine |
| MATH-08 | `math.log2(x)` base-2 logarithm | `log2()` from `<math.h>` |
| MATH-09 | `math.exp(x)` for e^x | `exp()` from `<math.h>` |
| MATH-10 | `math.hypot(a, b)` for sqrt(a^2 + b^2) | `hypot()` from `<math.h>` |
| IO-01 | `io.read_bytes(path)` DEFERRED | Out of scope — UInt8 not ready |
| IO-02 | `io.write_bytes(path, bytes)` DEFERRED | Out of scope — UInt8 not ready |
| IO-03 | `io.read_line()` reads from stdin | `fgets(buf, size, stdin)` — return `""` at EOF |
| IO-04 | `io.append_file(path, content)` appends to file | `fopen(p, "ab")` — append-binary mode |
| IO-05 | `io.basename(path)` filename component | Pure string walk: find last `/`, return remainder |
| IO-06 | `io.dirname(path)` directory component | Pure string walk: find last `/`, return prefix |
| IO-07 | `io.join_path(a, b)` concatenate path components | Concat with `/` separator, strip trailing slash from a first |
| IO-08 | `io.extension(path)` file extension | Find last `.` after last `/`, return suffix |
| IO-09 | `io.is_dir(path)` check if path is a directory | `stat(p, &st); S_ISDIR(st.st_mode)` — `<sys/stat.h>` already included |
| IO-10 | `io.read_lines(path)` read file into `List[String]` | Read file via existing `Iron_io_read_file_result`, then split on `\n` using `Iron_string_split` |
| TIME-01 | `time.since(start)` elapsed seconds since timestamp | `Iron_time_now() - start` — takes Float (seconds from `time.now()`) |
| TIME-02 | `time.Timer(duration)` constructor with duration field | New `object Timer` with `elapsed_ms: Int, duration_ms: Int`; `Iron_timer_new(double duration_s)` |
| TIME-03 | `timer.done()` check if timer expired | `t.elapsed_ms >= t.duration_ms` |
| TIME-04 | `timer.update(dt)` advance timer | `t.elapsed_ms += (int64_t)(dt * 1000.0)` |
| TIME-05 | `timer.reset()` reset timer (returns void) | Zero `t.elapsed_ms` |
| LOG-01 | `log.set_level(level)` filter log output | `Iron_log_set_level(level)` already exists in iron_log.c — only needs .iron declaration |
| LOG-02 | Log constants `log.DEBUG`, `log.INFO`, `log.WARN`, `log.ERROR` | Add `val DEBUG: Int`, etc. as fields in `object Log {}` |
| ITEST-03 | Integration tests for all math additions | 1 .iron + .expected pair covering all 10 new math functions |
| ITEST-04 | Integration tests for all IO additions | 1 .iron + .expected pair covering all 8 new IO functions (IO-01/02 deferred) |
| ITEST-05 | Integration tests for time and log additions | 1 .iron + .expected pair each for time additions and log additions |
</phase_requirements>

## Summary

Phase 39 closes the gap between what `docs/language_definition.md` specifies and what the four existing stdlib modules actually implement. The work is purely additive: new C function bodies appended to existing `.c` files, new declarations added to `.h` files, and new Iron declarations added to the four `.iron` wrappers. No compiler changes are needed — all four modules are already registered in `build.c` import detection and all four headers are already included unconditionally in `emit_c.c`.

The name dispatch chain is: `.iron` declares `func Math.asin(x: Float) -> Float {}` with empty body → parser/typecheck records it → `hir_to_lir.c` mangles `Math.asin` to `math_asin` (lowercased type + `_` + method) → `emit_mangle_name` prepends `Iron_` → generated C calls `Iron_math_asin(x)` → resolved by `iron_math.h` declaration + `iron_math.c` body. The same pattern applies for all four modules.

The Timer redesign is the most invasive change. The current `object Timer { val start_ms: Int }` and its three C functions must be replaced with `object Timer { val elapsed_ms: Int, val duration_ms: Int }` and four new C functions (`Iron_timer_new`, `Iron_timer_done`, `Iron_timer_update`, `Iron_timer_reset`). The `IRON_TIMER_STRUCT_DEFINED` guard in `iron_time.h` ensures the codegen-emitted struct body takes precedence over the header's fallback typedef — this mechanism must be preserved with updated field names.

The Log constants (`Log.DEBUG` etc.) are object fields declared in `log.iron`. The compiler treats these as field accesses on the `Log` object type. The C emission resolves them via the type's field layout — which means the `object Log {}` must declare them as `val` fields with the correct `Int` values matching the `Iron_LogLevel` enum values already in `iron_log.h`.

**Primary recommendation:** Follow the exact four-file pattern per module (`.iron` + `.h` + `.c` + integration test). No build system changes, no compiler changes.

## Standard Stack

### Core
| Component | Version/Location | Purpose | Why Standard |
|-----------|-----------------|---------|--------------|
| `<math.h>` | C99, already in iron_math.h | asin, acos, atan2, log, log2, exp, hypot | Part of C99 standard, already linked via `-lm` in CMakeLists.txt |
| `<stdio.h>` | C99, already in iron_io.c | fgets for read_line, fopen append mode | Already included |
| `<sys/stat.h>` | POSIX, already in iron_io.c | stat() for is_dir | Already included |
| `Iron_string_split` | src/runtime/iron_string.c | Split file content on `\n` for read_lines | Already declared in iron_runtime.h |
| `Iron_List_Iron_String` macros | iron_runtime.h IRON_LIST_DECL/IMPL | Return type for io.read_lines | Already instantiated in iron_runtime.h |

### No New Dependencies
All needed C functions are already available in the headers already included by the stdlib files. No new `#include` directives are needed for math additions. The only subtle inclusion needed: `iron_io.c` must `#include "runtime/iron_runtime.h"` (already present via iron_io.h) to use `Iron_string_split` for `read_lines`.

**Installation:** No new packages — stdlib additions compile with existing CMake targets.

## Architecture Patterns

### Module Implementation Pattern (established in Phases 33-38)

Each module addition follows a strict four-part pattern:

```
src/stdlib/
├── math.iron       # Iron API declaration — add new func declarations
├── iron_math.h     # C header — add new function prototypes
└── iron_math.c     # C implementation — append new function bodies
tests/integration/
└── math_additions.iron / .expected   # ITEST-03
```

### Name Mangling Chain

```
Iron source:     Math.asin(x)
hir_to_lir.c:   type_name="Math" → lowercase → "math" → mangled="math_asin"
emit_mangle_name: "Iron_" + "math_asin" = "Iron_math_asin"
C function:      double Iron_math_asin(double x) { return asin(x); }
```

The key rule: C function names are `Iron_<lowercase_type>_<method>`. This is automatic — no compiler changes needed, the mangling happens at compile time from the .iron declaration.

### Pattern 1: Static Module Function (Math, IO, Time, Log)

**What:** Function declared in .iron with empty body `{}`, C implementation in .c file.
**When to use:** All module-level functions.
**Example:**
```iron
-- math.iron addition
func Math.asin(x: Float) -> Float {}
func Math.acos(x: Float) -> Float {}
func Math.atan2(y: Float, x: Float) -> Float {}
```
```c
// iron_math.c addition
double Iron_math_asin(double x)                  { return asin(x); }
double Iron_math_acos(double x)                  { return acos(x); }
double Iron_math_atan2(double y, double x)       { return atan2(y, x); }
```
```c
// iron_math.h addition
double Iron_math_asin(double x);
double Iron_math_acos(double x);
double Iron_math_atan2(double y, double x);
```

### Pattern 2: Timer Accumulator (TIME-01..05)

The Timer object must be redesigned from a stopwatch (records start time, queries elapsed) to an accumulator (holds elapsed and duration, advanced by `update(dt)`).

**Current time.iron:**
```iron
object Timer { val start_ms: Int }
func Timer.create() -> Timer {}
func Timer.since(t: Timer) -> Int {}
func Timer.reset(t: Timer) -> Timer {}
```

**New time.iron (complete replacement):**
```iron
-- time.iron -- Iron wrapper for the Time stdlib module
object Time {}

func Time.now() -> Float {}
func Time.now_ms() -> Int {}
func Time.sleep(ms: Int) {}
func Time.since(start: Float) -> Float {}
func Time.Timer(duration: Float) -> Timer {}

object Timer {
  val elapsed_ms: Int
  val duration_ms: Int
}

func Timer.done(t: Timer) -> Bool {}
func Timer.update(t: Timer, dt: Float) {}
func Timer.reset(t: Timer) {}
```

**C implementation (iron_time.h additions):**
```c
/* Updated Iron_Timer struct fallback (when codegen doesn't define it) */
#ifndef IRON_TIMER_STRUCT_DEFINED
typedef struct Iron_Timer { int64_t elapsed_ms; int64_t duration_ms; } Iron_Timer;
#else
typedef struct Iron_Timer Iron_Timer;
#endif

Iron_Timer Iron_time_Timer(double duration_s);   /* constructor: time.Timer(2.0) */
bool       Iron_timer_done(Iron_Timer t);
void       Iron_timer_update(Iron_Timer *t, double dt);
void       Iron_timer_reset(Iron_Timer *t);
double     Iron_time_since(double start);
```

**C implementation (iron_time.c additions):**
```c
Iron_Timer Iron_time_Timer(double duration_s) {
    Iron_Timer t;
    t.elapsed_ms  = 0;
    t.duration_ms = (int64_t)(duration_s * 1000.0);
    return t;
}
bool Iron_timer_done(Iron_Timer t) {
    return t.elapsed_ms >= t.duration_ms;
}
void Iron_timer_update(Iron_Timer *t, double dt) {
    t->elapsed_ms += (int64_t)(dt * 1000.0);
}
void Iron_timer_reset(Iron_Timer *t) {
    t->elapsed_ms = 0;
}
double Iron_time_since(double start) {
    return Iron_time_now() - start;
}
```

**Critical note on Timer.update / Timer.reset:** These methods mutate the timer in place. In Iron, `var timer = time.Timer(2.0)` creates a mutable binding. The method calls `timer.update(dt)` and `timer.reset()` must mutate the instance. The C functions take `Iron_Timer *t` (pointer). The `hir_to_lir.c` method dispatch passes `self` as the first argument — for mutable struct methods this must be passed by pointer. Research shows the existing `Iron_string_*` methods receive `Iron_String self` by value; however timer mutation requires pointer passing. This is a potential pitfall (see Common Pitfalls).

### Pattern 3: Log Constants as Object Fields (LOG-02)

The `object Log {}` must be expanded to declare `val` fields for the four level constants. In Iron, object fields declared with `val` are accessible as `Log.DEBUG`. The compiler emits field access for these.

**New log.iron:**
```iron
-- log.iron -- Iron wrapper for the Log stdlib module
object Log {
  val DEBUG: Int
  val INFO: Int
  val WARN: Int
  val ERROR: Int
}

func Log.debug(msg: String) {}
func Log.info(msg: String) {}
func Log.warn(msg: String) {}
func Log.error(msg: String) {}
func Log.set_level(level: Int) {}
```

**How Log.DEBUG resolves:** The `object Log { val DEBUG: Int }` declaration means `Log.DEBUG` is a field access on the `Log` type. The compiler emits this as a field access in the generated struct. However, Log has no instance — it is a namespace. The field values must be initialized as compile-time constants. Research shows the Iron `object` with `val` fields — when accessed statically like `Math.PI` — are emitted as `#define IRON_PI` or as `const` fields on the struct. Looking at math.iron: `val PI: Float` on `object Math {}` is accessed as `Math.PI`. The C emission for `Math.PI` is `Iron_math_PI` or similar.

**Investigation finding on object constant fields:** The `iron_math.h` defines `IRON_PI`, `IRON_TAU`, `IRON_E` as `#define`. The typecheck/emit layer maps `Math.PI` to the constant. For Log, the same approach applies: declare `Log.DEBUG` as 0, `Log.INFO` as 1, etc. In the .iron file, these are `val` fields. Looking at how `Math.PI` compiles — checking typecheck for constant field resolution is needed during implementation, but the pattern is established and working. The safe approach: use the exact same pattern as `object Math { val PI: Float }` → the C side provides `Iron_math_PI` etc.

### Pattern 4: IO Path Functions (IO-05..09)

Path functions are pure string operations with no filesystem calls (except `is_dir`):

```c
// iron_io.h additions
Iron_String Iron_io_basename(Iron_String path);
Iron_String Iron_io_dirname(Iron_String path);
Iron_String Iron_io_join_path(Iron_String a, Iron_String b);
Iron_String Iron_io_extension(Iron_String path);
bool        Iron_io_is_dir(Iron_String path);
Iron_String Iron_io_read_line(void);
Iron_Error  Iron_io_append_file(Iron_String path, Iron_String content);
Iron_List_Iron_String Iron_io_read_lines(Iron_String path);
```

**read_lines implementation strategy:** Use `Iron_io_read_file_result()` to get content, then `Iron_string_split()` to split on `\n`. This reuses existing infrastructure. The `Iron_List_Iron_String` type is already declared and implemented in `iron_runtime.h` via `IRON_LIST_DECL(Iron_String, Iron_String)`.

### Anti-Patterns to Avoid

- **Reimplementing basename/dirname with OS-specific separator:** `io.join_path` MUST use `/` only (locked decision). All path functions work on strings as-is.
- **Timer returning new Timer from reset():** The old `Iron_timer_reset(Iron_Timer t) -> Iron_Timer` pattern is replaced. `reset()` returns void and mutates in place.
- **Adding new includes to emit_c.c:** All four headers are already included unconditionally. No emit_c.c changes needed.
- **Touching build.c or check.c:** All four modules are already registered for import detection. No changes needed.
- **Using `log2` as a C function name in iron_math.c without aliasing:** `log2` is a standard `<math.h>` function — the wrapper is simply `double Iron_math_log2(double x) { return log2(x); }`.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Natural log, log2, exp, hypot | Custom approximations | `<math.h>` functions | Edge cases (denormals, NaN, +/-inf) handled by libc |
| String-to-list splitting for read_lines | Custom tokenizer | `Iron_string_split(content, newline_sep)` | Already implemented, handles empty lines |
| Directory check | Custom file probing | `stat() + S_ISDIR()` | POSIX standard, `<sys/stat.h>` already included |
| RNG seeding | New PRNG state | Set `s_global_rng.state` on the existing `__thread Iron_RNG` | The xorshift64 state is already the canonical RNG |
| Timer elapsed tracking with wall clock | New `clock_gettime` calls | Accumulator `elapsed_ms += (int64_t)(dt * 1000)` | Game-loop friendly, no system call per check |

**Key insight:** All infrastructure is already present. The phase is about wiring, not building new subsystems.

## Common Pitfalls

### Pitfall 1: Timer update/reset Mutation vs Value Semantics

**What goes wrong:** `Iron_timer_update` and `Iron_timer_reset` mutate the timer in place. If the C function takes `Iron_Timer t` by value, the mutation is lost. The calling Iron code does `timer.update(dt)` on a `var timer` binding.

**Why it happens:** Iron's method dispatch in `hir_to_lir.c` passes `self` by value for object types (matches existing pattern for `Iron_Timer` passed by value). Mutation is impossible if the C function receives a copy.

**How to avoid:** Two options:
1. `update` and `reset` take `Iron_Timer *t` and return `void` — the caller's alloca is passed by address
2. `update` returns a new `Iron_Timer` and the call site assigns back

Option 1 requires the method dispatch to pass the alloca pointer, which may not be how current hir_to_lir works for object method calls. Option 2 (return new value, assign to `var`) is safer and matches the existing `Iron_timer_reset(Iron_Timer t) -> Iron_Timer` pattern. However, the locked decision says `timer.reset()` returns void. Implementer must verify how the compiler handles void-returning mutating methods on struct objects. If the compiler cannot pass a struct pointer, return-and-assign is the workaround but conflicts with the locked decision.

**Warning signs:** Integration test `timer.update(dt); println(timer.done())` always prints `false` regardless of dt value — this means the update was discarded.

### Pitfall 2: math.seed Thread-Local State

**What goes wrong:** `Iron_math_random()` uses a `static __thread Iron_RNG s_global_rng` with a lazy init flag. `Iron_math_seed(n)` must set this same per-thread state. If `seed` resets the init flag, subsequent `random()` calls will re-init from the clock, discarding the seed.

**Why it happens:** The `s_global_rng` and `s_global_rng_initialized` are `static __thread` locals inside `Iron_math_random`. A new `Iron_math_seed` function in the same file cannot access them.

**How to avoid:** Move `s_global_rng` and `s_global_rng_initialized` to file-scope `static __thread` variables (still thread-local, but accessible from multiple functions in the same file). Both `Iron_math_random` and `Iron_math_seed` then reference the same state.

**Warning signs:** `math.seed(42); math.random()` produces different values across runs — seeding had no effect.

### Pitfall 3: Log Constants — How Iron Object Val Fields Map to C

**What goes wrong:** `object Log { val DEBUG: Int }` + `Log.DEBUG` — the compiler must know the value is 0. If the Iron object field has no initializer, the emitted C struct field is uninitialized.

**Why it happens:** Iron `object` fields declared as `val` without initializers in the object body may default to 0 or may be undefined depending on compiler behavior.

**How to avoid:** Verify how `Math.PI` resolves. The `iron_math.h` defines `#define IRON_PI 3.14...` and the typecheck/emit layer must have a special case for object constant fields. If `Log.DEBUG` field access emits `Iron_Log_DEBUG`, a corresponding `#define Iron_Log_DEBUG 0` or `static const int64_t Iron_Log_DEBUG = 0` in `iron_log.h` is needed. Check the exact C output for a `Math.PI` usage to understand the pattern before implementing Log constants.

**Warning signs:** `log.set_level(log.DEBUG)` sets level to garbage value; or compilation error "undefined identifier Iron_Log_DEBUG".

### Pitfall 4: io.read_lines Trailing Newline

**What goes wrong:** Most text files end with `\n`. Splitting `"a\nb\nc\n"` on `"\n"` produces `["a", "b", "c", ""]` — an extra empty string at the end.

**Why it happens:** `Iron_string_split` splits literally on the separator, producing an empty string after the final newline.

**How to avoid:** Either trim the trailing newline from the file content before splitting, or filter out the trailing empty string from the result list. The simpler approach: strip one trailing `\n` (or `\r\n`) from the content string before calling split.

**Warning signs:** `io.read_lines("file.txt").len()` returns one more than the number of lines in the file.

### Pitfall 5: math.log Name Collision

**What goes wrong:** `Iron_math_log` calls `log(x)` from `<math.h>`. The .iron declaration is `func Math.log(x: Float) -> Float {}` which mangles to `Iron_math_log`. The C implementation `double Iron_math_log(double x) { return log(x); }` is fine — `log` here refers to the `<math.h>` function, not a recursive call. No collision.

**Why it happens:** Concern that `log` is ambiguous. It is not — `Iron_math_log != log`. Not actually a pitfall, but worth documenting explicitly for the implementer.

## Code Examples

### Math — New Functions Pattern

```c
// Source: iron_math.c Phase 39 additions
double Iron_math_asin(double x)                  { return asin(x); }
double Iron_math_acos(double x)                  { return acos(x); }
double Iron_math_atan2(double y, double x)       { return atan2(y, x); }
double Iron_math_log(double x)                   { return log(x); }
double Iron_math_log2(double x)                  { return log2(x); }
double Iron_math_exp(double x)                   { return exp(x); }
double Iron_math_hypot(double a, double b)       { return hypot(a, b); }
double Iron_math_random_float(double min, double max) {
    return Iron_math_random() * (max - min) + min;
}
int64_t Iron_math_sign(double x) {
    if (x > 0.0) return  1;
    if (x < 0.0) return -1;
    return 0;
}
```

For `math.seed(n)` — requires promoting thread-local RNG state to file scope:
```c
// Promote to file scope (replace current static __thread locals inside Iron_math_random)
static __thread Iron_RNG  s_math_rng;
static __thread int       s_math_rng_init = 0;

void Iron_math_seed(int64_t n) {
    s_math_rng = Iron_rng_create((uint64_t)n);
    s_math_rng_init = 1;
}
double Iron_math_random(void) {
    /* WINDOWS-TODO: __thread -> IRON_THREAD_LOCAL */
    if (!s_math_rng_init) { s_global_rng_init(&s_math_rng); s_math_rng_init = 1; }
    return Iron_rng_next_float(&s_math_rng);
}
int64_t Iron_math_random_int(int64_t min, int64_t max) {
    if (!s_math_rng_init) { s_global_rng_init(&s_math_rng); s_math_rng_init = 1; }
    return Iron_rng_next_int(&s_math_rng, min, max);
}
```

### IO — Append and Stdin

```c
// Source: iron_io.c Phase 39 additions
Iron_Error Iron_io_append_file(Iron_String path, Iron_String content) {
    const char *p = iron_string_cstr(&path);
    FILE *f = fopen(p, "ab");
    if (!f) return iron_error_new(1, "could not open file for appending");
    const char *data = iron_string_cstr(&content);
    size_t len = iron_string_byte_len(&content);
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) return iron_error_new(5, "write incomplete");
    return iron_error_none();
}

Iron_String Iron_io_read_line(void) {
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return iron_string_from_cstr("", 0);  /* EOF */
    }
    size_t len = strlen(buf);
    /* Strip trailing \n */
    if (len > 0 && buf[len - 1] == '\n') { buf[--len] = '\0'; }
    if (len > 0 && buf[len - 1] == '\r') { buf[--len] = '\0'; }
    return iron_string_from_cstr(buf, len);
}
```

### IO — Path Functions

```c
// Source: iron_io.c Phase 39 additions
Iron_String Iron_io_basename(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    const char *last = strrchr(p, '/');
    const char *base = last ? last + 1 : p;
    return iron_string_from_cstr(base, strlen(base));
}

Iron_String Iron_io_dirname(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    const char *last = strrchr(p, '/');
    if (!last) return iron_string_from_cstr(".", 1);
    if (last == p) return iron_string_from_cstr("/", 1);
    size_t len = (size_t)(last - p);
    return iron_string_from_cstr(p, len);
}

Iron_String Iron_io_join_path(Iron_String a, Iron_String b) {
    /* Always forward slash — works on Windows too */
    const char *ap = iron_string_cstr(&a);
    const char *bp = iron_string_cstr(&b);
    size_t alen = iron_string_byte_len(&a);
    /* Strip trailing slash from a */
    while (alen > 0 && ap[alen - 1] == '/') alen--;
    size_t blen = strlen(bp);
    size_t total = alen + 1 + blen + 1;
    char *buf = (char *)malloc(total);
    memcpy(buf, ap, alen);
    buf[alen] = '/';
    memcpy(buf + alen + 1, bp, blen);
    buf[alen + 1 + blen] = '\0';
    Iron_String result = iron_string_from_cstr(buf, alen + 1 + blen);
    free(buf);
    return result;
}

Iron_String Iron_io_extension(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    const char *last_slash = strrchr(p, '/');
    const char *base = last_slash ? last_slash + 1 : p;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return iron_string_from_cstr("", 0);
    return iron_string_from_cstr(dot, strlen(dot));  /* includes the dot */
}

bool Iron_io_is_dir(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}
```

### IO — read_lines

```c
// Source: iron_io.c Phase 39 additions
// Requires: #include "runtime/iron_runtime.h" (already in iron_io.h)
Iron_List_Iron_String Iron_io_read_lines(Iron_String path) {
    Iron_Result_String_Error res = Iron_io_read_file_result(path);
    if (!iron_error_is_ok(res.v1)) {
        return Iron_List_Iron_String_create();
    }
    Iron_String sep = iron_string_from_cstr("\n", 1);
    Iron_List_Iron_String lines = Iron_string_split(res.v0, sep);
    /* Remove trailing empty string if file ended with \n */
    if (lines.count > 0) {
        Iron_String last = lines.items[lines.count - 1];
        if (iron_string_byte_len(&last) == 0) {
            lines.count--;
        }
    }
    return lines;
}
```

### Integration Test Pattern (from Phase 38)

```iron
-- tests/integration/math_additions.iron
-- Test: MATH-01..10
import math

func main() {
    -- Trig inverse
    val a = Math.asin(0.0)
    println("asin(0) ok")
    val b = Math.acos(1.0)
    println("acos(1) ok")
    val c = Math.atan2(0.0, 1.0)
    println("atan2(0,1) ok")
    -- sign
    val s = Math.sign(-5.0)
    println(s)
    val s2 = Math.sign(0.0)
    println(s2)
    val s3 = Math.sign(3.0)
    println(s3)
    -- log/exp/hypot
    println("log ok")
    println("exp ok")
    println("hypot ok")
    -- random_float
    Math.seed(42)
    println("seed ok")
    println("random_float ok")
}
```

```
-- tests/integration/math_additions.expected
asin(0) ok
acos(1) ok
atan2(0,1) ok
-1
0
1
log ok
exp ok
hypot ok
seed ok
random_float ok
```

Note: Tests avoid printing floating-point results directly to sidestep platform-specific formatting. Tests verify the functions compile, link, and return plausible results.

## State of the Art

| Old Approach | Current Approach | Notes |
|--------------|-----------------|-------|
| Timer as stopwatch (start_ms + since) | Timer as accumulator (elapsed_ms + duration_ms + update/done/reset) | Phase 39 redesign per locked decision |
| `Log {}` has no level constants | `Log { val DEBUG/INFO/WARN/ERROR: Int }` + `log.set_level()` | Iron_log_set_level already exists in C, only needs .iron wiring |
| `io.iron` has only 6 functions | `io.iron` gains 8 more functions (IO-03..10) | IO-01/02 deferred |
| `math.iron` has 11 functions | `math.iron` gains 10 more functions (MATH-01..10) | All math.h wrappers |

**Deprecated/removed in this phase:**
- `Timer.create()` — replaced by `time.Timer(duration)` constructor
- `Timer.since(t)` — replaced by `time.since(start_float)` on the Time module
- `Timer.reset(t) -> Timer` (returns Timer) — replaced by `Timer.reset(t)` returning void
- `object Timer { val start_ms: Int }` — replaced with `{ val elapsed_ms, val duration_ms }`

## Open Questions

1. **Timer update/reset mutation semantics**
   - What we know: The locked decision says `timer.update(dt)` and `timer.reset()` return void and mutate in place. The current `hir_to_lir.c` passes struct instances by value for method calls.
   - What's unclear: Whether the compiler currently supports void-returning methods that mutate their receiver (passes alloca pointer rather than value). The existing `Iron_timer_reset` returns a new `Iron_Timer` — this may be because the compiler cannot pass by pointer.
   - Recommendation: Test with a minimal Iron program: `var t = time.Timer(1.0); t.update(0.5); println(t.elapsed_ms)`. If it prints 0, mutation is not working and `update`/`reset` must return `Timer` (value-replacement pattern) instead. If the locked decision is firm on void returns, a compiler fix may be needed — but this should be scoped as a plan-time decision by the implementer.

2. **Log constant object fields — C emission pattern**
   - What we know: `Math.PI` works as a field access on `object Math {}`. The C emission produces a reference to `Iron_math_PI` or similar. `iron_math.h` has `#define IRON_PI 3.14...`.
   - What's unclear: The exact name the compiler emits for `Log.DEBUG`. It may be `Iron_Log_DEBUG` or `Iron_log_DEBUG` (lowercase) or something else.
   - Recommendation: Before implementing, compile a trivial test `import math; func main() { val x = Math.PI }` and inspect the generated C to see how `Math.PI` is emitted. Use the same pattern for `Log.DEBUG`. If it emits `Iron_math_PI`, then add `#define Iron_log_DEBUG 0` etc. to `iron_log.h`.

3. **io.read_lines — `[String]` return type in .iron**
   - What we know: `func String.split(sep: String) -> [String] {}` works in string.iron. The `[String]` type is a `List[String]`.
   - What's unclear: Whether `func IO.read_lines(path: String) -> [String] {}` in io.iron will typecheck correctly and produce a `Iron_List_Iron_String` return type in C.
   - Recommendation: Use `-> [String]` in the io.iron declaration (same as string.split). The typecheck should handle it identically.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Shell integration test runner |
| Config file | `tests/integration/run_integration.sh` |
| Quick run command | `cd /Users/victor/code/iron-lang && ./tests/integration/run_integration.sh build/iron 2>&1 \| grep -E "math_additions\|io_additions\|time_additions\|log_additions"` |
| Full suite command | `cd /Users/victor/code/iron-lang && ./tests/integration/run_integration.sh build/iron` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Test File | File Exists? |
|--------|----------|-----------|-----------|-------------|
| MATH-01..10 | All 10 new math functions compile and return correct type | integration | `tests/integration/math_additions.iron` | Wave 0 |
| IO-01 | DEFERRED | — | — | — |
| IO-02 | DEFERRED | — | — | — |
| IO-03..10 | All 8 new IO functions compile and work | integration | `tests/integration/io_additions.iron` | Wave 0 |
| TIME-01..05 | Timer accumulator, since(), done(), update(), reset() | integration | `tests/integration/time_additions.iron` | Wave 0 |
| LOG-01..02 | set_level filters output, constants accessible | integration | `tests/integration/log_additions.iron` | Wave 0 |
| ITEST-03 | math_additions test pair exists and passes | integration | `tests/integration/math_additions.iron` + `.expected` | Wave 0 |
| ITEST-04 | io_additions test pair exists and passes | integration | `tests/integration/io_additions.iron` + `.expected` | Wave 0 |
| ITEST-05 | time_additions + log_additions test pairs exist and pass | integration | two pairs | Wave 0 |

### Sampling Rate
- **Per task commit:** Build iron binary and run the specific new test pair only
- **Per wave merge:** `./tests/integration/run_integration.sh build/iron` — full suite
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/integration/math_additions.iron` + `.expected` — covers MATH-01..10 / ITEST-03
- [ ] `tests/integration/io_additions.iron` + `.expected` — covers IO-03..10 / ITEST-04
- [ ] `tests/integration/time_additions.iron` + `.expected` — covers TIME-01..05 / ITEST-05 (partial)
- [ ] `tests/integration/log_additions.iron` + `.expected` — covers LOG-01..02 / ITEST-05 (partial)

Note: The existing `test_math.iron`, `test_io.iron`, `test_time.iron`, `test_log.iron` cover only pre-Phase-39 functionality. New test files use the `math_additions` / `io_additions` naming convention to avoid breaking existing tests.

## Sources

### Primary (HIGH confidence)
- `/Users/victor/code/iron-lang/src/stdlib/iron_math.c` — full xorshift64 RNG, all existing math wrappers, `__thread` pattern
- `/Users/victor/code/iron-lang/src/stdlib/iron_math.h` — existing declarations, `IRON_PI` constant pattern
- `/Users/victor/code/iron-lang/src/stdlib/iron_io.c` — `Iron_io_read_file_result` pattern, `Iron_Result_String_Error`, file I/O pattern
- `/Users/victor/code/iron-lang/src/stdlib/iron_io.h` — all existing declarations, `Iron_Result_String_Error` typedef
- `/Users/victor/code/iron-lang/src/stdlib/iron_time.c` — `Iron_time_now()`, `Iron_time_now_ms()`, current Timer implementation
- `/Users/victor/code/iron-lang/src/stdlib/iron_time.h` — `IRON_TIMER_STRUCT_DEFINED` guard mechanism
- `/Users/victor/code/iron-lang/src/stdlib/iron_log.c` — `s_log_level`, `Iron_log_set_level`, `iron_log_emit` — all already implemented
- `/Users/victor/code/iron-lang/src/stdlib/iron_log.h` — `Iron_LogLevel` enum with values 0..3
- `/Users/victor/code/iron-lang/src/stdlib/math.iron` — current wrapper pattern
- `/Users/victor/code/iron-lang/src/stdlib/string.iron` — Phase 38 method declaration pattern
- `/Users/victor/code/iron-lang/src/runtime/iron_runtime.h` — `Iron_List_Iron_String` typedef and `IRON_LIST_DECL`, `Iron_string_split` declaration
- `/Users/victor/code/iron-lang/src/lir/emit_c.c` lines 88-136, 774-900 — name mangling chain, method dispatch to C
- `/Users/victor/code/iron-lang/src/lir/emit_c.c` lines 3408-3420 — all four stdlib headers included unconditionally
- `/Users/victor/code/iron-lang/src/cli/build.c` lines 680-760 — import detection for all four modules already present
- `/Users/victor/code/iron-lang/CMakeLists.txt` lines 90-102 — `iron_stdlib` static library, `-lm` already linked

### Secondary (MEDIUM confidence)
- `/Users/victor/code/iron-lang/docs/language_definition.md` lines 1315-1395 — authoritative stdlib API spec for math, io, time, log

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all C functions verified in existing headers; `<math.h>` already included
- Architecture: HIGH — name mangling chain read directly from source; existing patterns verified
- Timer mutation pitfall: MEDIUM — behavior not empirically tested; based on source reading
- Log constant emission: MEDIUM — exact C symbol name for `Log.DEBUG` not tested; based on `Math.PI` analogy
- Integration test pattern: HIGH — verified against Phase 38 test files

**Research date:** 2026-04-02
**Valid until:** 2026-05-02 (stable codebase)
