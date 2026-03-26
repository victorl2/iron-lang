# Phase 3: Runtime, Stdlib, and CLI - Research

**Researched:** 2026-03-25
**Domain:** C runtime library, standard library modules, CLI toolchain, CI setup
**Confidence:** HIGH

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**String and Collection Design**
- Full SSO + interning: small strings (<=23 bytes) stored inline, larger strings heap-allocated with global intern table for deduplication
- `weak T` keyword added to the language for rc cycle breaking
- Method syntax for collections: `list.push(item)`, `map.get(key)`, `set.contains(val)`
- Generic collections from the start: List[T], Map[K,V], Set[T] monomorphized per Phase 2 decisions

**Threading and Concurrency**
- C11 threads (`<threads.h>`) as the stated target — but Apple Clang 17 on macOS does NOT include `<threads.h>`. Use pthreads directly (`<pthread.h>`) which is available on all three targets. The `<threads.h>` intent from CONTEXT.md must be interpreted as "C11-era threading semantics, implemented via pthreads on macOS/Linux"
- Default pool size: CPU count - 1 (leave one core for main/render thread)
- Channels unbuffered by default; explicit buffering via `channel[T](size)`
- Named thread pools: `val compute = pool("compute", 4)`
- Mutex[T] + raw Lock/CondVar primitives
- Panic propagates on await: panic stored in Future, re-raised on parent await
- Parallel-for always auto-splits (range / pool_size)
- `await_all` helper: `await_all([task1, task2])`

**CLI Command Behavior**
- Build directory: `.iron-build/` kept when `--debug-build`, else temp dir cleaned up
- Timestamp-based build cache
- Test discovery: `test_` prefix convention
- `iron fmt` in-place by default
- `iron check` = parse + analyze only (no codegen, no clang)
- Binary name matches source: `hello.iron` → `hello`
- `iron run` passes args after `--`
- Basic `iron.toml` project file for multi-file projects
- Clang-only as target C compiler

**Standard Library**
- File I/O uses multiple return values: `func read_file(path: String) -> String, Error`
- Log format: `[2025-03-25 12:00:00] [INFO] message`
- Both global RNG functions and explicit RNG objects
- math module wraps `<math.h>` with Iron types
- io module: read_file, read_bytes, write_file, write_bytes, file_exists, list_files, create_dir, delete_file
- time module: now, now_ms, sleep, since, Timer object
- log module: info, warn, error, debug with set_level, output to stderr

**CI Setup**
- GitHub Actions in this phase
- CMake + full test suite + ASan/UBSan on macOS and Linux

### Claude's Discretion
- SSO threshold (23 bytes is suggested, exact value flexible)
- Intern table implementation details (hash map size, eviction policy)
- Thread pool work-stealing vs FIFO queue
- Build cache storage format
- iron.toml parser implementation (hand-written vs library)
- Exact CLI argument parsing approach
- Error type design details
- Multiple return value codegen implementation (anonymous struct naming)

### Deferred Ideas (OUT OF SCOPE)

None — discussion stayed within phase scope

</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| RT-01 | String type supports UTF-8 with interning and small string optimization | SSO layout, intern table via stb_ds hash map, UTF-8 byte/codepoint distinction |
| RT-02 | List, Map, Set collections work correctly | Monomorphization stubs from Phase 2 need real implementations; stb_ds is already vendored |
| RT-03 | Reference counting correctly manages shared ownership with retain/release | Atomic retain/release using `<stdatomic.h>`, weak ptr pattern, destructor callback |
| RT-04 | Thread pool with work queue, submit, and barrier | pthreads on macOS/Linux; work queue using mutex + condvar |
| RT-05 | Channel implementation (ring buffer + mutex + condvars) with send/recv/try_recv | Ring buffer pattern, blocking/non-blocking semantics |
| RT-06 | Mutex wraps a value with lock semantics | `pthread_mutex_t` wrapper around typed value |
| RT-07 | Built-in functions work: print, println, len, range, min, max, clamp, abs, assert | Replace printf stubs in gen_exprs.c with Iron_String-aware implementations |
| RT-08 | Runtime compiles and passes tests on macOS, Linux, and Windows | `<threads.h>` absent on Apple Clang 17; use pthreads; Windows deferred to Phase 4 per RT-08 scope |
| STD-01 | math module: trig, sqrt, pow, lerp, random, PI/TAU/E | Thin wrapper over `<math.h>`; RNG via xorshift64 or PCG32 |
| STD-02 | io module: file read/write, file_exists, list_files, create_dir | `<stdio.h>`, `<dirent.h>`, `<sys/stat.h>` on macOS/Linux |
| STD-03 | time module: now, now_ms, sleep, since, Timer | `clock_gettime(CLOCK_MONOTONIC, ...)` — verified available on macOS |
| STD-04 | log module: info/warn/error/debug with level filtering | stderr output, isatty for color, timestamp from time module |
| CLI-01 | `iron build [file]` compiles .iron to standalone binary | Full pipeline + clang invocation |
| CLI-02 | `iron run [file]` compiles and immediately executes | Build then exec |
| CLI-03 | `iron check [file]` type-checks without compiling to binary | Stop after iron_analyze(); no codegen |
| CLI-04 | `iron fmt [file]` formats Iron source code | Reuse iron_print_ast() from Phase 1 (printer.h already exists) |
| CLI-05 | `iron test [dir]` discovers and runs Iron tests | Discover `test_*.iron` files, compile+run each, collect exit codes |
| CLI-06 | Error messages show Rust-style diagnostics | Diagnostics system already exists; wire to CLI output |
| CLI-07 | Terminal output is colored | isatty() gating already established; extend ANSI codes to CLI |
| CLI-08 | `--verbose` flag shows generated C code | Print ctx's output buffer to stdout before clang invocation |
| TEST-04 | Memory safety validated with ASan/UBSan in CI | GitHub Actions workflow; CMake Debug build already has ASan/UBSan |

</phase_requirements>

---

## Summary

Phase 3 builds three interconnected layers on top of the fully-working compiler from Phase 2: a C runtime library that implements the Iron type system at runtime (Iron_String with SSO+interning, generic collections, rc with weak references, thread pool, channels, mutex), four standard library modules (math, io, time, log), and the `iron` CLI that orchestrates the full pipeline from source to executable.

The codegen already emits stubs for most runtime APIs: `Iron_pool_submit(Iron_global_pool, ...)`, `Iron_pool_barrier(Iron_global_pool)`, `Iron_handle_wait(...)`, and `printf` stubs for print/println. Phase 3's job is to make those stubs real. The monomorphization registry already emits stub structs for `Iron_List_T`, `Iron_Map_K_V`, `Iron_Set_T` with `items/count/capacity` fields — the runtime just needs to provide the function implementations that match those struct layouts.

The critical discovery is that Apple Clang 17 on macOS does NOT provide `<threads.h>`. All threading must be implemented via `<pthread.h>` (verified available). The CONTEXT.md decision for "C11 threads" should be interpreted as C11-era semantics (atomic rc via `<stdatomic.h>`, mutex semantics, condvars) implemented via the POSIX pthread API which is available on all three target platforms.

**Primary recommendation:** Build in this order: (1) Iron_String + Iron_Rc + builtins, (2) collections, (3) thread pool + channel + mutex, (4) stdlib modules, (5) CLI driver, (6) GitHub Actions CI. Each layer is independently testable.

---

## Standard Stack

### Core Runtime Components

| Component | C Standard / API | Purpose | Notes |
|-----------|-----------------|---------|-------|
| Iron_String | Custom struct | SSO + interning + UTF-8 | 23-byte SSO threshold; codepoint count separate from byte length |
| Thread pool | `<pthread.h>` (pthreads) | Iron_Pool work queue | `<threads.h>` absent on Apple Clang 17 macOS |
| Atomic rc | `<stdatomic.h>` | Iron_Rc retain/release | C11 stdatomic available on Apple Clang 17 (verified) |
| Channel | ring buffer + `pthread_mutex_t` + `pthread_cond_t` | Typed async channel | Unbuffered default = capacity 0, send blocks until recv |
| Mutex wrapper | `pthread_mutex_t` | Iron_Mutex[T] | Wraps value + mutex in same struct |
| stb_ds | already vendored in src/vendor/stb_ds.h | Hash map for intern table, collections | reuse existing vendor file |

### Standard Library APIs

| Module | C API Used | Notes |
|--------|-----------|-------|
| iron_math | `<math.h>` + custom RNG | Link with `-lm` |
| iron_io | `<stdio.h>`, `<dirent.h>`, `<sys/stat.h>` | All three verified available on macOS |
| iron_time | `clock_gettime(CLOCK_MONOTONIC)`, `nanosleep` | Verified available on macOS (Apple clang 17) |
| iron_log | `<stdio.h>` + time module + isatty | Write to stderr |

### CLI Stack

| Component | Approach | Rationale |
|-----------|---------|-----------|
| Argument parsing | Hand-written (getopt_long or custom) | No external dep needed for small command set |
| iron.toml parser | Hand-written minimal parser | Only 4-5 key/value fields needed; avoid dep |
| Build cache | .iron-build/ dir + mtime comparison via `stat()` | `<sys/stat.h>` already available |
| Clang invocation | `execvp()` / `posix_spawn()` on Unix; `CreateProcess` on Windows | No shell = no injection risk |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| pthreads directly | `<threads.h>` (C11) | `<threads.h>` absent on Apple Clang 17 macOS — pthreads wins |
| stb_ds for intern table | Custom hash map | stb_ds already vendored; zero new dependencies |
| Hand-written TOML | tomlc99 or similar | External dep not worth it for minimal project file |
| xorshift64 RNG | PCG32 | Both are fine; xorshift64 is simpler to implement |

**Installation:** No new dependencies needed. Runtime is pure C using standard POSIX APIs.

---

## Architecture Patterns

### Recommended New Directory Structure

```
src/
├── runtime/
│   ├── iron_runtime.h       # Single public header for all runtime APIs
│   ├── iron_string.c        # Iron_String: SSO, interning, UTF-8 ops
│   ├── iron_collections.c   # Iron_List_*, Iron_Map_*, Iron_Set_* (generic impls)
│   ├── iron_rc.c            # Iron_Rc: atomic retain/release, weak references
│   ├── iron_threads.c       # Iron_Pool, Iron_Channel, Iron_Mutex, Iron_Handle
│   └── iron_builtins.c      # print, println, len, range, min, max, clamp, abs, assert
├── stdlib/
│   ├── iron_math.h / iron_math.c
│   ├── iron_io.h   / iron_io.c
│   ├── iron_time.h / iron_time.c
│   └── iron_log.h  / iron_log.c
└── cli/
    ├── main.c               # Entry point; dispatch to build/run/check/fmt/test
    ├── build.c              # iron build + iron run pipeline
    ├── check.c              # iron check (analyze only)
    ├── fmt.c                # iron fmt (reuse printer.h)
    └── test_runner.c        # iron test (discover + run test_ files)
```

### Pattern 1: Iron_String with SSO + Interning

**What:** Strings <= 23 bytes stored entirely inline (no heap). Larger strings heap-allocated with a global intern table deduplicated by content. The struct carries both byte_length and codepoint_count fields.

**Struct layout:**
```c
// Source: design based on common SSO implementations (llvm::SmallString, fbstring)
#define IRON_STRING_SSO_MAX 23

typedef struct {
    union {
        struct {
            char    *data;          /* heap pointer */
            uint32_t byte_length;
            uint32_t codepoint_count;
            uint8_t  _padding[IRON_STRING_SSO_MAX - 8];
            uint8_t  flags;         /* bit 0 = is_heap, bit 1 = is_interned */
        } heap;
        struct {
            char    data[IRON_STRING_SSO_MAX]; /* inline storage */
            uint8_t len;            /* byte length (codepoints computed on access) */
        } sso;
    };
} Iron_String;
// Total: 24 bytes — fits in two cache lines on ARM64
```

**Intern table:** Global hash map (stb_ds `shmap`) mapping `const char *` → `Iron_String`. Call `Iron_string_intern(s)` to deduplicate. Literal strings from codegen are pre-interned at program start via `iron_runtime_init()`.

**When to use:** All Iron `String` values go through this. The codegen currently emits `"hello"` as raw C string literals (stub) — Phase 3 replaces those with `Iron_string_from_literal("hello", 5)` calls that return interned Iron_String values.

### Pattern 2: Generic Collection via Macro Expansion

**What:** The codegen monomorphization registry emits stub structs like `Iron_List_int64_t { int64_t *items; int64_t count; int64_t capacity; }`. The runtime must provide the function implementations to match those struct layouts.

**Approach:** Use C macros to generate typed collection functions. One macro call per type instantiation:

```c
// Source: iron_collections.c macro pattern
#define IRON_LIST_IMPL(T, suffix)                                  \
    void Iron_List_##suffix##_push(Iron_List_##suffix *self, T item) { \
        if (self->count >= self->capacity) {                       \
            self->capacity = self->capacity ? self->capacity * 2 : 8; \
            self->items = realloc(self->items,                     \
                                  self->capacity * sizeof(T));     \
        }                                                          \
        self->items[self->count++] = item;                         \
    }                                                              \
    T Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t i) { \
        return self->items[i];                                     \
    }                                                              \
    int64_t Iron_List_##suffix##_len(const Iron_List_##suffix *self) { \
        return self->count;                                        \
    }
```

**Key constraint:** The codegen's `ensure_monomorphized_type()` in gen_types.c already emits the struct definition and prototypes like `void Iron_List_int64_t_push(...)`. The runtime macro implementations must use the exact same naming convention.

### Pattern 3: Thread Pool with Work Queue

**What:** Fixed-size worker thread pool with a FIFO work queue (mutex + condvar). `Iron_pool_submit` enqueues a work item. `Iron_pool_barrier` blocks until the queue drains.

**Why FIFO over work-stealing:** Work-stealing adds significant complexity. For v1 game dev use cases (physics, AI, particle updates), FIFO is sufficient. Work-stealing is a future enhancement.

```c
// Pattern based on standard POSIX thread pool design
typedef struct {
    void (*fn)(void *arg);
    void *arg;
} Iron_WorkItem;

typedef struct {
    pthread_t       *threads;
    int              thread_count;
    Iron_WorkItem   *queue;         /* circular buffer */
    int              queue_head;
    int              queue_tail;
    int              queue_capacity;
    pthread_mutex_t  lock;
    pthread_cond_t   work_ready;
    pthread_cond_t   work_done;
    int              pending;       /* items not yet completed */
    bool             shutdown;
    const char      *name;
} Iron_Pool;

extern Iron_Pool *Iron_global_pool; /* used by parallel-for codegen stubs */
```

The global pool `Iron_global_pool` is initialized in `iron_runtime_init()` with `(cpu_count - 1)` threads. The codegen already emits `Iron_global_pool` references; the runtime just needs to provide the global.

**Named pools:** `pool("compute", 4)` in Iron → `Iron_Pool *Iron_pool_create(const char *name, int n)` in C. Named pools are heap-allocated separately from the global pool.

### Pattern 4: Channel as Bounded Ring Buffer

**What:** Fixed-capacity circular buffer with blocking send/recv. Unbuffered default = capacity 1 (not 0, because a capacity-0 ring buffer requires special rendezvous logic — simpler to use capacity-1 with blocking semantics that mimic unbuffered behavior).

```c
typedef struct {
    void           **ring;          /* void* elements (type-erased) */
    int              capacity;
    int              head;
    int              tail;
    int              count;
    pthread_mutex_t  lock;
    pthread_cond_t   not_full;
    pthread_cond_t   not_empty;
    bool             closed;
} Iron_Channel;
```

**Codegen integration:** The generated code for `channel[T]()` emits a typed wrapper around `Iron_Channel`. The monomorphized `Iron_Channel_String_send` wraps `Iron_Channel_send` with type casting.

### Pattern 5: Reference Counting with Weak Pointers

**What:** `Iron_Rc` wraps a heap value with an atomic strong reference count and an atomic weak reference count. Retain/release use `atomic_fetch_add/sub`. When strong count hits 0, destructor fires. When both counts hit 0, the control block itself is freed.

```c
typedef struct Iron_RcControl {
    atomic_int       strong_count;
    atomic_int       weak_count;
    void           (*destructor)(void *value);
    /* value data follows immediately in memory (flexible array member) */
} Iron_RcControl;

typedef struct {
    Iron_RcControl *ctrl;   /* NULL for null rc */
} Iron_Rc;

typedef struct {
    Iron_RcControl *ctrl;   /* weak ref: doesn't keep value alive */
} Iron_Weak;
```

`weak T` in Iron emits `Iron_Weak` — locks to strong ref to access, NULL if value is dead.

### Pattern 6: Multiple Return Values via Anonymous Struct

**What:** `func read_file(path: String) -> String, Error` emits a C struct `Iron_Result_String_Error` with fields `v0` and `v1`.

**Codegen change needed:** The codegen currently handles single return types. Phase 3 needs to extend `emit_func_prototype` / `emit_func_impl` to detect multi-return functions and emit the anonymous struct + adjust the return type.

```c
// Generated by codegen for: func read_file(path: String) -> String, Error
typedef struct { Iron_String v0; Iron_Error v1; } Iron_Result_String_Error;
Iron_Result_String_Error Iron_io_read_file(Iron_String path);

// At call site: val data, val err = io.read_file("save.dat")
Iron_Result_String_Error _r0 = Iron_io_read_file(_path);
const Iron_String data = _r0.v0;
const Iron_Error err = _r0.v1;
```

**Naming convention:** `Iron_Result_<type1>_<type2>` for the anonymous struct. The codegen's `ensure_optional_type` pattern provides the model — use the same dedup-registry approach.

### Pattern 7: CLI Driver Architecture

**What:** `src/cli/main.c` dispatches to sub-commands. The build pipeline wraps the existing `iron_codegen()` and adds a clang invocation step.

```c
// Build pipeline in build.c
int iron_build(const char *source_path, const char *output_path, IronBuildOpts opts) {
    // 1. Read source file
    // 2. Run full pipeline: lex → parse → analyze → codegen
    // 3. Write generated C to .iron-build/ or temp dir
    // 4. Check build cache: skip if source mtime <= last build mtime
    // 5. Invoke clang: execvp("clang", ["-std=c11", "-O2", generated_c, runtime_srcs...])
    // 6. If --debug-build: keep .iron-build/; else: cleanup temp dir
}
```

**iron check:** Call `iron_analyze()` and stop. Return diagnostic output. No clang invocation.

**iron fmt:** Call `iron_parse()` then `iron_print_ast(root, &arena)` (already exists in printer.h). Write result back to file in-place.

**iron test:** Discover all `test_*.iron` files in the given directory. Compile each with `iron build`. Run each binary. Collect exit codes. Report pass/fail.

### Anti-Patterns to Avoid

- **Using `<threads.h>` directly:** Not available on Apple Clang 17 macOS. Use `<pthread.h>`.
- **String interning without thread safety:** The global intern table is written from multiple threads (any spawn body that creates strings). Protect with a reader-writer lock or make `Iron_string_intern()` thread-safe via a mutex.
- **Printing Iron_String as `%s` in printf:** Iron_String is a struct, not a `char*`. `print/println` stubs in gen_exprs.c use `printf("%s", arg)` which only works by accident for the SSO inline case. Runtime must provide `Iron_print(Iron_String s)` and codegen must be updated to call it.
- **Hardcoding 4 chunks in parallel-for:** gen_stmts.c currently hardcodes `(total + 3) / 4` for chunk splitting. The runtime must use `Iron_global_pool->thread_count` as the divisor.
- **Building `iron` CLI binary without separating it from `iron_compiler` library:** The compiler is already a static library (`libiron_compiler.a`). The CLI should link that library and add only the driver code. Do not merge compiler and CLI into one monolithic source.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Hash map for intern table / collections | Custom hash map | `stb_ds.h` (already vendored) | Already used throughout compiler; battle-tested |
| Atomic operations for rc | Custom memory barriers | `<stdatomic.h>` | C11, verified available on Apple Clang 17 |
| UTF-8 codepoint scanning | Custom bitwise decoder | Well-known 4-line UTF-8 advance loop | Standard algorithm; no library needed |
| File system directory listing | POSIX reimplementation | `<dirent.h>` + `opendir/readdir` | Available on macOS and Linux; verified |
| RNG | Custom PRNG | xorshift64 (4 lines of C) | Simple, fast, sufficient for game dev use |
| Timestamp | Custom timer | `clock_gettime(CLOCK_MONOTONIC, ...)` | Verified available macOS Clang 17 |
| Build cache invalidation | Content hashing | `stat().st_mtime` comparison | Simpler and fast enough; matches make/cmake |

**Key insight:** All required functionality is in POSIX and C11 standard headers. The runtime is a pure-C library with zero external dependencies beyond what's already vendored (stb_ds).

---

## Common Pitfalls

### Pitfall 1: C11 `<threads.h>` Not Available on Apple Clang 17

**What goes wrong:** CONTEXT.md says "C11 threads only (`<threads.h>`)". Apple Clang 17 (the current macOS toolchain) does not ship `<threads.h>`. A `#include <threads.h>` fails to compile immediately.

**Why it happens:** C11 added `<threads.h>` to the standard but Apple did not implement it in their libc. It is absent from all Apple SDKs verified on this machine.

**How to avoid:** Use `#include <pthread.h>` directly. The pthread API is fully available and achieves all the same semantics. Wrap it in an internal `iron_thread_platform.h` abstraction if future portability to `<threads.h>` is desired, but the wrapper is not required for v1.

**Warning signs:** `fatal error: 'threads.h' file not found` on first compile attempt.

### Pitfall 2: Iron_String Is a Struct, Not a char*

**What goes wrong:** The existing print/println codegen stubs emit `printf("%s", arg)`. Once Iron_String is a 24-byte struct with SSO union, passing it to `%s` is undefined behavior and will print garbage or crash.

**How to avoid:** The runtime must provide `void Iron_print(Iron_String s)` and `void Iron_println(Iron_String s)`. The codegen in gen_exprs.c must be updated in the same plan/wave that introduces Iron_String — change the print/println cases from `printf("%s", ...)` to `Iron_print(...)` and `Iron_println(...)`.

**Ordering constraint:** Iron_String runtime implementation and the codegen print stub replacement must be in the same plan unit.

### Pitfall 3: Monomorphized Collection Struct Names Must Match Codegen Exactly

**What goes wrong:** The codegen emits `Iron_List_int64_t` (from gen_types.c `mangle_generic()`). If the runtime macro expands to `Iron_List_Int` or any other variant, the linker fails with "undefined reference to `Iron_List_int64_t_push`".

**How to avoid:** Read `mangle_generic()` in gen_types.c before writing any collection macros. The naming pattern is `Iron_<base>_<c_type_name>` where the C type name is `int64_t`, `double`, `bool`, `Iron_String`, or `Iron_<TypeName>`. The macro suffix parameter must match these exact strings.

**Key naming rules from gen_types.c:**
- `Int` → `int64_t`
- `Float` → `double`
- `Bool` → `bool`
- `String` → `Iron_String`
- `Enemy` (user type) → `Iron_Enemy`

### Pitfall 4: parallel-for Chunk Size Hardcoded to 4

**What goes wrong:** gen_stmts.c currently hardcodes chunk splitting as `(_total + 3) / 4` — always 4 chunks. This ignores the actual pool thread count. For a 2-thread pool, 4 chunks is extra overhead. For a 16-thread pool, 4 chunks underutilizes cores.

**How to avoid:** The parallel-for codegen stub should call `Iron_pool_thread_count(Iron_global_pool)` at runtime to determine the number of chunks. This requires adding `int Iron_pool_thread_count(Iron_Pool *pool)` to the runtime API, and updating gen_stmts.c in the same plan that implements the pool.

### Pitfall 5: String Intern Table Not Thread-Safe

**What goes wrong:** Multiple `spawn` bodies run concurrently and each may construct Iron_String values. All construction paths call `Iron_string_intern()` which writes to the global intern table. Concurrent writes to stb_ds hash maps are not safe.

**How to avoid:** Protect the intern table with a `pthread_mutex_t` (a single global `Iron_intern_lock`). Lock on write (new interned string) and read (lookup). Given that most strings in a typical program are created at startup and the game loop mostly reuses them, this lock is rarely contended in practice.

### Pitfall 6: io Module Error Returns Don't Match Codegen Multi-Return Emit

**What goes wrong:** The io module returns `String, Error` (multiple returns). The codegen changes required for multi-return (anonymous struct `Iron_Result_String_Error`) must be implemented before the stdlib io functions can be called from Iron source. If the codegen change is deferred, all io calls produce incorrect C.

**How to avoid:** Multi-return codegen support (anonymous result struct emission) must be in the same wave as the io stdlib module. The two are tightly coupled — neither is testable without the other.

### Pitfall 7: iron fmt Overwrites File Before Successful Parse

**What goes wrong:** `iron fmt --in-place file.iron` reads the file, parses it, writes formatted output back. If the parse fails (syntax error in source), the file is partially written with empty/garbage content and the original is lost.

**How to avoid:** Always write formatted output to a temp file first. Verify the parse succeeded. Only then atomically rename the temp file to the original path. If parsing fails, report the error and leave the original file untouched.

### Pitfall 8: Build Cache mtime Comparison Races on Fast Machines

**What goes wrong:** Timestamp-based cache compares source file `st_mtime` to the last build output's `st_mtime`. On modern NVMe drives, a re-save and re-compile within the same second produces equal mtimes and the cache wrongly skips recompilation.

**How to avoid:** Store the last-known mtime in a `.iron-build/cache.json` (or similar) rather than comparing output file mtime to source mtime. Use nanosecond precision (`st_mtimespec.tv_nsec` on macOS, `st_mtim.tv_nsec` on Linux) to distinguish same-second saves.

---

## Code Examples

### Iron_String SSO Implementation

```c
// Source: standard SSO pattern (llvm SmallString / fbstring variation)
// Thread-safe interning via pthread_mutex_t

static pthread_mutex_t s_intern_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char *key; Iron_String value; } *s_intern_table = NULL;

Iron_String iron_string_from_cstr(const char *cstr, size_t byte_len) {
    Iron_String s;
    if (byte_len <= IRON_STRING_SSO_MAX) {
        memcpy(s.sso.data, cstr, byte_len);
        s.sso.data[byte_len] = '\0';
        s.sso.len = (uint8_t)byte_len;
        s.sso.flags = 0;  /* SSO, not interned */
    } else {
        char *data = malloc(byte_len + 1);
        memcpy(data, cstr, byte_len);
        data[byte_len] = '\0';
        s.heap.data = data;
        s.heap.byte_length = (uint32_t)byte_len;
        s.heap.codepoint_count = iron_utf8_codepoint_count(cstr, byte_len);
        s.heap.flags = 0x01;  /* is_heap */
    }
    return s;
}

/* UTF-8 codepoint advance — standard bit pattern check */
static size_t iron_utf8_advance(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}
```

### Thread Pool with FIFO Queue

```c
// Source: standard POSIX thread pool pattern
// iron_threads.c

static void *worker_thread(void *arg) {
    Iron_Pool *pool = (Iron_Pool *)arg;
    while (true) {
        pthread_mutex_lock(&pool->lock);
        while (pool->queue_head == pool->queue_tail && !pool->shutdown) {
            pthread_cond_wait(&pool->work_ready, &pool->lock);
        }
        if (pool->shutdown && pool->queue_head == pool->queue_tail) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        Iron_WorkItem item = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pthread_mutex_unlock(&pool->lock);

        item.fn(item.arg);

        pthread_mutex_lock(&pool->lock);
        pool->pending--;
        if (pool->pending == 0) pthread_cond_broadcast(&pool->work_done);
        pthread_mutex_unlock(&pool->lock);
    }
}

void Iron_pool_barrier(Iron_Pool *pool) {
    pthread_mutex_lock(&pool->lock);
    while (pool->pending > 0) {
        pthread_cond_wait(&pool->work_done, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}
```

### CLI Build Pipeline

```c
// src/cli/build.c
int iron_cmd_build(const char *source_path, BuildOpts opts) {
    // 1. Read source
    char *source = read_file_to_string(source_path);  // from io utils

    // 2. Run compiler pipeline
    Iron_Arena arena = iron_arena_create(4 * 1024 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    // ... lex, parse, analyze, codegen ...
    const char *c_code = iron_codegen(program, scope, &arena, &diags);
    if (diags.error_count > 0) {
        iron_print_diagnostics(&diags, source, source_path, isatty(STDERR_FILENO));
        return 1;
    }

    // 3. Write C to .iron-build/
    char build_dir[PATH_MAX];
    iron_build_dir(source_path, build_dir, sizeof(build_dir));
    mkdir(build_dir, 0755);
    char c_path[PATH_MAX];
    snprintf(c_path, sizeof(c_path), "%s/generated.c", build_dir);
    write_string_to_file(c_path, c_code);

    // 4. Invoke clang
    char out_path[PATH_MAX];
    iron_output_name(source_path, out_path, sizeof(out_path));
    const char *clang_args[] = {
        "clang", "-std=c11", "-O2",
        c_path,
        IRON_RUNTIME_SRC,   /* path to iron_runtime.c baked in at compile time */
        "-o", out_path,
        NULL
    };
    execvp("clang", (char *const *)clang_args);  /* replaces process */
}
```

### GitHub Actions CI Workflow

```yaml
# .github/workflows/ci.yml
name: CI
on: [push, pull_request]
jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - name: Configure (Debug with ASan/UBSan)
        run: cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
      - name: Build
        run: cmake --build build
      - name: Test
        run: ctest --test-dir build --output-on-failure
```

Note: ASan/UBSan is already wired in CMakeLists.txt for Debug builds. The CI workflow just needs to set `CMAKE_BUILD_TYPE=Debug`.

---

## State of the Art

| Old Approach (Phase 2 stubs) | Phase 3 Real Implementation | Impact |
|------------------------------|----------------------------|--------|
| `printf("%s", arg)` for print/println | `Iron_print(Iron_String s)` using SSO-aware extraction | Iron programs can print non-ASCII strings correctly |
| `rc expr` emits inner expression directly (gen_types.c rc stub) | `Iron_Rc_create(...)`, `Iron_Rc_retain(...)`, `Iron_Rc_release(...)` with atomic counts | Real shared ownership, no dangling pointers |
| Monomorphized collection stubs (Iron_List_T struct + empty push/len prototypes) | Full realloc-based dynamic arrays with push/get/len/clear | Iron programs can use List[T], Map[K,V], Set[T] |
| `Iron_pool_submit(Iron_global_pool, ...)` emitted but not linked | Real thread pool linked via iron_runtime.c | spawn/parallel-for actually run concurrently |
| `Iron_handle_wait(handle)` emitted but no Iron_Handle type | Iron_Handle with condvar + result storage | await actually blocks until spawn completes |

**Deprecated/outdated in Phase 3:**
- The `printf` stubs in gen_exprs.c CASE IRON_NODE_CALL for print/println: replaced by `Iron_print` / `Iron_println`
- The chunk-size hardcode `(total + 3) / 4` in gen_stmts.c: replaced by pool thread count
- The `rc` type mapping to bare pointer (`%s*`) in gen_types.c: replaced by `Iron_Rc` struct

---

## Open Questions

1. **Windows support for RT-08**
   - What we know: RT-08 requires "macOS, Linux, and Windows". CONTEXT.md locks clang-only. Windows with clang requires either `pthreads-win32` (MinGW) or Win32 threading APIs.
   - What's unclear: Is Windows required in Phase 3 or can it ship in Phase 4 (cross-platform phase)?
   - Recommendation: Implement macOS + Linux in Phase 3 with `#ifdef _WIN32` stubs. Full Windows parity is Phase 4 (GAME-04 covers cross-platform binaries). Phase 3 runtime should compile on macOS and Linux and pass CI on those two platforms.

2. **`weak T` keyword codegen**
   - What we know: `weak T` is added to the language for rc cycle breaking. The runtime needs `Iron_Weak` type.
   - What's unclear: The codegen from Phase 2 currently handles `rc` as a bare pointer stub. Phase 3 needs both `Iron_Rc` and `Iron_Weak` codegen changes.
   - Recommendation: Add `IRON_TYPE_WEAK` handling to gen_types.c alongside the `IRON_TYPE_RC` changes. Both must be done in the same codegen plan.

3. **iron.toml multi-file project compilation order**
   - What we know: `iron.toml` specifies an entry point. The compiler needs to follow imports transitively to discover all files.
   - What's unclear: The module graph builder (SEM-11 from Phase 2) handles import resolution. Does the CLI need additional logic to handle multi-file compilation?
   - Recommendation: For Phase 3, `iron build file.iron` compiles a single file entry point and follows imports from there using the existing SEM-11 module resolution. The iron.toml file is parsed to determine entry point and dependency flags (like `raylib = true`), but the compiler already handles multi-file via imports.

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Unity v2.6.1 (via CMake FetchContent) |
| Config file | CMakeLists.txt (existing) |
| Quick run command | `cmake --build build && ctest --test-dir build -R test_runtime --output-on-failure` |
| Full suite command | `cmake --build build && ctest --test-dir build --output-on-failure` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| RT-01 | Iron_String SSO for short strings, heap for long, interning deduplicates | unit | `ctest --test-dir build -R test_string` | ❌ Wave 0 |
| RT-01 | len("日本語") == 3 (codepoints not bytes) | unit | `ctest --test-dir build -R test_string` | ❌ Wave 0 |
| RT-02 | List[Int] push/get/len works | unit | `ctest --test-dir build -R test_collections` | ❌ Wave 0 |
| RT-02 | Map[String,Int] set/get/has works | unit | `ctest --test-dir build -R test_collections` | ❌ Wave 0 |
| RT-03 | Rc retain/release: count correct, destructor fires at 0 | unit | `ctest --test-dir build -R test_rc` | ❌ Wave 0 |
| RT-03 | Weak pointer returns NULL after strong refs drop | unit | `ctest --test-dir build -R test_rc` | ❌ Wave 0 |
| RT-04 | Thread pool: submit N tasks, barrier waits for all | unit | `ctest --test-dir build -R test_threads` | ❌ Wave 0 |
| RT-05 | Channel: send/recv round-trip; try_recv returns null when empty | unit | `ctest --test-dir build -R test_threads` | ❌ Wave 0 |
| RT-06 | Mutex lock/unlock with concurrent access | unit | `ctest --test-dir build -R test_threads` | ❌ Wave 0 |
| RT-07 | Iron_print handles SSO and heap strings | unit | `ctest --test-dir build -R test_builtins` | ❌ Wave 0 |
| RT-07 | len() correct for arrays, strings, collections | unit | `ctest --test-dir build -R test_builtins` | ❌ Wave 0 |
| RT-08 | Full test suite passes under ASan/UBSan on macOS and Linux | integration | `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build` | ❌ Wave 0 (CI) |
| STD-01 | math.sin/cos/sqrt/lerp produce correct values | unit | `ctest --test-dir build -R test_math` | ❌ Wave 0 |
| STD-01 | math.random() in [0,1]; math.random_int(min,max) in range | unit | `ctest --test-dir build -R test_math` | ❌ Wave 0 |
| STD-02 | io.write_file + io.read_file round-trip | unit | `ctest --test-dir build -R test_io` | ❌ Wave 0 |
| STD-02 | io.file_exists returns correct bool | unit | `ctest --test-dir build -R test_io` | ❌ Wave 0 |
| STD-03 | time.now_ms increases monotonically | unit | `ctest --test-dir build -R test_time` | ❌ Wave 0 |
| STD-03 | time.sleep(0.1) sleeps at least 90ms | unit | `ctest --test-dir build -R test_time` | ❌ Wave 0 |
| STD-04 | log.info writes to stderr with timestamp and level | unit | `ctest --test-dir build -R test_log` | ❌ Wave 0 |
| STD-04 | log.set_level(WARN) suppresses INFO and DEBUG | unit | `ctest --test-dir build -R test_log` | ❌ Wave 0 |
| CLI-01 | `iron build hello.iron` produces `hello` binary that runs | integration | `iron build tests/fixtures/hello.iron && ./hello` | ❌ Wave 0 |
| CLI-02 | `iron run hello.iron` prints expected output | integration | `iron run tests/fixtures/hello.iron` | ❌ Wave 0 |
| CLI-03 | `iron check bad.iron` reports type error, no binary produced | integration | `iron check tests/fixtures/type_error.iron; test $? -ne 0` | ❌ Wave 0 |
| CLI-04 | `iron fmt` produces idempotent output (fmt twice = same result) | integration | run fmt twice, diff output | ❌ Wave 0 |
| CLI-05 | `iron test` discovers test_*.iron, reports pass/fail | integration | `iron test tests/iron_tests/` | ❌ Wave 0 |
| CLI-06 | Error messages show source snippet with arrow | integration | verify error output format on bad.iron | existing diag system |
| CLI-07 | Color codes present when stdout is tty | unit | isatty mock test | existing isatty pattern |
| CLI-08 | `--verbose` prints generated C to stdout | integration | `iron build --verbose hello.iron | grep '#include'` | ❌ Wave 0 |
| TEST-04 | ASan/UBSan CI passes | CI | `.github/workflows/ci.yml` run | ❌ Wave 0 (CI file) |

### Sampling Rate
- **Per task commit:** `cmake --build build && ctest --test-dir build -R "test_runtime|test_string|test_collections|test_rc|test_threads|test_builtins" --output-on-failure`
- **Per wave merge:** `cmake -DCMAKE_BUILD_TYPE=Debug -B build-asan && cmake --build build-asan && ctest --test-dir build-asan --output-on-failure`
- **Phase gate:** Full suite green + ASan/UBSan clean before `/gsd:verify-work`

### Wave 0 Gaps

- [ ] `tests/test_string.c` — covers RT-01 (SSO, heap, interning, UTF-8 len)
- [ ] `tests/test_collections.c` — covers RT-02 (List, Map, Set)
- [ ] `tests/test_rc.c` — covers RT-03 (retain/release, weak, destructor)
- [ ] `tests/test_threads.c` — covers RT-04/RT-05/RT-06 (pool, channel, mutex)
- [ ] `tests/test_builtins.c` — covers RT-07 (print, len, range, assert)
- [ ] `tests/test_math.c` — covers STD-01
- [ ] `tests/test_io.c` — covers STD-02
- [ ] `tests/test_time.c` — covers STD-03
- [ ] `tests/test_log.c` — covers STD-04
- [ ] `.github/workflows/ci.yml` — covers TEST-04 (ASan/UBSan on CI)
- [ ] CMakeLists.txt additions: `add_executable(test_string ...)` etc.
- [ ] `src/runtime/iron_runtime.h` — public runtime API header
- [ ] `src/runtime/iron_string.c`, `iron_collections.c`, `iron_rc.c`, `iron_threads.c`, `iron_builtins.c`
- [ ] `src/stdlib/iron_math.h/c`, `iron_io.h/c`, `iron_time.h/c`, `iron_log.h/c`
- [ ] `src/cli/main.c` and subcommand files
- [ ] Integration test fixtures in `tests/fixtures/` or `tests/iron_tests/`

---

## Sources

### Primary (HIGH confidence)
- `src/codegen/gen_stmts.c` — verified `Iron_global_pool`, `Iron_pool_submit`, `Iron_pool_barrier` symbols emitted
- `src/codegen/gen_exprs.c` — verified `Iron_handle_wait`, printf stubs for print/println, rc stub as bare pointer
- `src/codegen/gen_types.c` — verified mangle_generic naming convention (`Iron_<base>_<c_type>`)
- `src/codegen/codegen.h` — verified Iron_Codegen structure, spawn/parallel counters, lambda lifting
- `CMakeLists.txt` — verified ASan/UBSan in Debug build, Unity test framework, libiron_compiler.a
- Apple Clang 17 runtime check — verified `<threads.h>` NOT available, `<pthread.h>` available, `<stdatomic.h>` available, `clock_gettime` available

### Secondary (MEDIUM confidence)
- ARCHITECTURE.md research (2026-03-25) — confirmed runtime statically linked, `iron_runtime.h` always included
- PITFALLS.md research (2026-03-25) — RC cycle pitfall, string UTF-8 pitfall, Windows pthread pitfall (now resolved as clang-only + pthreads)
- CONTEXT.md (03-CONTEXT.md) — all locked decisions and discretion areas

### Tertiary (LOW confidence)
- SSO threshold at 23 bytes — conventional choice matching many string implementations; actual optimal value should be benchmarked

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all APIs verified against Apple Clang 17 on this machine
- Architecture: HIGH — derived directly from existing codegen stubs that must be implemented
- Pitfalls: HIGH — several confirmed by direct code inspection (printf stub, hardcoded chunk size, threads.h absence)

**Research date:** 2026-03-25
**Valid until:** 2026-06-25 (90 days; stable POSIX APIs, runtime design is locked)
