# Requirements: Iron v0.2.0-alpha Standard Library Expansion

**Defined:** 2026-04-02
**Core Value:** Iron programs can do real work using the standard library alone, without dropping to C for common operations.

## v1 Requirements

Requirements for this milestone. Each maps to roadmap phases.

### Compiler Fixes

- [x] **COMP-01**: Compiler correctly dispatches method calls on `IRON_TYPE_STRING` receivers (typecheck.c + hir_to_lir.c)
- [x] **COMP-02**: Compiler correctly dispatches method calls on `IRON_TYPE_ARRAY` receivers for collection types (typecheck.c + hir_to_lir.c)
- [x] **COMP-03**: `build.c` argv_buf bumped to 128 with static assert to prevent overflow when adding new modules
- [x] **COMP-04**: `build.c` import detection replaced with token-level helper (no false positives for `import os` in comments/strings)
- [x] **COMP-05**: Windows portability documented in existing `iron_io.c` (dirent.h, mkdir) and `iron_log.c` (localtime_r, isatty)
- [x] **COMP-06**: `iron_math.c` `__thread` portability documented with WINDOWS-TODO comments
- [x] **COMP-07**: `emit_c.c` threads `argc`/`argv` through `iron_runtime_init(argc, argv)` for `os.args()` support

### String Methods

- [x] **STR-01**: User can call `s.upper()` to get an uppercase copy of a string
- [x] **STR-02**: User can call `s.lower()` to get a lowercase copy of a string
- [x] **STR-03**: User can call `s.trim()` to remove leading/trailing whitespace
- [x] **STR-04**: User can call `s.contains(sub)` to check if a string contains a substring
- [x] **STR-05**: User can call `s.starts_with(prefix)` to check string prefix
- [x] **STR-06**: User can call `s.ends_with(suffix)` to check string suffix
- [x] **STR-07**: User can call `s.split(sep)` to split a string into `List[String]`
- [x] **STR-08**: User can call `s.replace(old, new)` to substitute all occurrences
- [x] **STR-09**: User can call `s.substring(start, end)` to extract a slice
- [x] **STR-10**: User can call `s.index_of(sub)` to find first occurrence position (-1 if absent)
- [x] **STR-11**: User can call `s.char_at(i)` to get the character at index i as a String
- [x] **STR-12**: User can call `s.to_int()` to parse a string as an integer
- [x] **STR-13**: User can call `s.to_float()` to parse a string as a float
- [x] **STR-14**: User can call `sep.join(list)` to concatenate a list of strings with a separator
- [x] **STR-15**: User can call `s.len()` as a method to get character count
- [x] **STR-16**: User can call `s.repeat(n)` to repeat a string n times
- [x] **STR-17**: User can call `s.pad_left(width, char)` to left-pad a string
- [x] **STR-18**: User can call `s.pad_right(width, char)` to right-pad a string
- [x] **STR-19**: User can call `s.count(sub)` to count occurrences of a substring

### Collection Operations

- [ ] **COLL-01**: User can call `list.sort()` to sort a list in place
- [ ] **COLL-02**: User can call `list.filter(fn)` to create a filtered copy
- [ ] **COLL-03**: User can call `list.map(fn)` to transform all elements (same-type only)
- [ ] **COLL-04**: User can call `list.find(fn)` to get the first matching element
- [ ] **COLL-05**: User can call `list.reverse()` to reverse a list in place
- [ ] **COLL-06**: User can call `list.for_each(fn)` to iterate with a callback
- [ ] **COLL-07**: User can call `list.any(fn)` to check if any element matches a predicate
- [ ] **COLL-08**: User can call `list.all(fn)` to check if all elements match a predicate
- [ ] **COLL-09**: User can call `list.reduce(fn, init)` to fold/aggregate a list
- [ ] **COLL-10**: User can call `list.slice(start, end)` to extract a sub-list
- [ ] **COLL-11**: User can call `list.unique()` to remove duplicates
- [ ] **COLL-12**: User can call `map.keys()` to get all keys as a List
- [ ] **COLL-13**: User can call `map.values()` to get all values as a List
- [ ] **COLL-14**: User can call `map.for_each(fn)` to iterate key-value pairs
- [ ] **COLL-15**: User can call `map.merge(other)` to merge two maps
- [ ] **COLL-16**: User can call `set.union(other)` to get the union of two sets
- [ ] **COLL-17**: User can call `set.intersection(other)` to get the intersection
- [ ] **COLL-18**: User can call `set.difference(other)` to get elements in self not in other
- [ ] **COLL-19**: User can call `set.to_list()` to convert a set to a list

### Math Completion

- [x] **MATH-01**: User can call `math.asin(x)` for inverse sine
- [x] **MATH-02**: User can call `math.acos(x)` for inverse cosine
- [x] **MATH-03**: User can call `math.atan2(y, x)` for two-argument arctangent
- [x] **MATH-04**: User can call `math.sign(x)` to get -1, 0, or 1
- [x] **MATH-05**: User can call `math.seed(n)` to seed the random number generator
- [x] **MATH-06**: User can call `math.random_float(min, max)` for a float-range random
- [x] **MATH-07**: User can call `math.log(x)` for natural logarithm
- [x] **MATH-08**: User can call `math.log2(x)` for base-2 logarithm
- [x] **MATH-09**: User can call `math.exp(x)` for e^x
- [x] **MATH-10**: User can call `math.hypot(a, b)` for sqrt(a^2 + b^2)

### IO Completion

- [ ] **IO-01**: User can call `io.read_bytes(path)` to read binary file contents
- [ ] **IO-02**: User can call `io.write_bytes(path, bytes)` to write binary data
- [ ] **IO-03**: User can call `io.read_line()` to read a single line from stdin
- [ ] **IO-04**: User can call `io.append_file(path, content)` to append to a file
- [ ] **IO-05**: User can call `io.basename(path)` to get the filename component
- [ ] **IO-06**: User can call `io.dirname(path)` to get the directory component
- [ ] **IO-07**: User can call `io.join_path(a, b)` to concatenate path components
- [ ] **IO-08**: User can call `io.extension(path)` to get the file extension
- [ ] **IO-09**: User can call `io.is_dir(path)` to check if a path is a directory
- [ ] **IO-10**: User can call `io.read_lines(path)` to read a file into `List[String]`

### Time Completion

- [ ] **TIME-01**: User can call `time.since(start)` to get elapsed seconds since a timestamp
- [ ] **TIME-02**: User can create a Timer with `time.Timer(duration)` with a duration field
- [ ] **TIME-03**: User can call `timer.done()` to check if the timer has expired
- [ ] **TIME-04**: User can call `timer.update(dt)` to advance the timer
- [ ] **TIME-05**: User can call `timer.reset()` to reset the timer (returns void, not Timer)

### Log Completion

- [ ] **LOG-01**: User can call `log.set_level(level)` to filter log output at runtime
- [ ] **LOG-02**: Log level constants `log.DEBUG`, `log.INFO`, `log.WARN`, `log.ERROR` are available

### OS Module

- [ ] **OS-01**: User can call `os.env(key)` to read an environment variable
- [ ] **OS-02**: User can call `os.set_env(key, value)` to set an environment variable
- [ ] **OS-03**: User can call `os.args()` to get command-line arguments as `List[String]`
- [ ] **OS-04**: User can call `os.exit(code)` to exit with a status code
- [ ] **OS-05**: User can call `os.cwd()` to get the current working directory
- [ ] **OS-06**: User can call `os.chdir(path)` to change the working directory
- [ ] **OS-07**: User can call `os.home_dir()` to get the user's home directory
- [ ] **OS-08**: User can call `os.temp_dir()` to get the system temp directory
- [ ] **OS-09**: User can call `os.getpid()` to get the current process ID
- [ ] **OS-10**: User can call `os.is_dir(path)` to check if a path is a directory
- [ ] **OS-11**: User can call `os.stat(path)` to get file metadata (size, mod time, type)
- [ ] **OS-12**: User can call `os.environ()` to get all environment variables as `Map[String, String]`

### Testing Module

- [ ] **TEST-01**: User can call `test.assert_eq(a, b)` to assert equality (Int, Float, String, Bool)
- [ ] **TEST-02**: User can call `test.assert_true(cond)` to assert a boolean condition
- [ ] **TEST-03**: User can call `test.assert_false(cond)` to assert false
- [ ] **TEST-04**: User can call `test.assert_ne(a, b)` to assert not-equal
- [ ] **TEST-05**: User can call `test.fail(msg)` to fail unconditionally with a message
- [ ] **TEST-06**: Test runner discovers `test_*` functions automatically
- [ ] **TEST-07**: Test runner executes all tests and reports pass/fail count with colored output
- [ ] **TEST-08**: A failing test does not abort the entire suite (test isolation)
- [ ] **TEST-09**: User can define `test_setup()` and `test_teardown()` for per-test hooks
- [ ] **TEST-10**: User can call `test.skip(reason)` to skip a test with a message
- [ ] **TEST-11**: Test output includes timing per test

### Integration Tests

- [x] **ITEST-01**: Integration tests exist for all string methods
- [ ] **ITEST-02**: Integration tests exist for all collection operations
- [ ] **ITEST-03**: Integration tests exist for all math additions
- [ ] **ITEST-04**: Integration tests exist for all IO additions
- [ ] **ITEST-05**: Integration tests exist for time and log additions
- [ ] **ITEST-06**: Integration tests exist for all OS module functions
- [ ] **ITEST-07**: Integration tests exist for the testing module itself

## v2 Requirements

Deferred to future release. Tracked but not in current roadmap.

### OS Module Advanced

- **OS-ADV-01**: `os.spawn(cmd, args)` — run subprocess asynchronously with pipe capture
- **OS-ADV-02**: `os.exec(cmd, args)` — run subprocess synchronously, return stdout
- **OS-ADV-03**: `os.which(name)` — find executable in PATH
- **OS-ADV-04**: `os.chmod(path, mode)` — set file permissions
- **OS-ADV-05**: Signal handling (`os.on_signal()`)
- **OS-ADV-06**: Symlink support

### Collection Advanced

- **COLL-ADV-01**: `list.flatten()` — flatten List[List[T]] (requires generics)
- **COLL-ADV-02**: `list.zip(other)` — pair elements (requires tuples)
- **COLL-ADV-03**: Cross-type `list.map(fn)` returning different element type (requires generics)

### String Advanced

- **STR-ADV-01**: Unicode-aware case folding for `upper()`/`lower()`
- **STR-ADV-02**: Regex / pattern matching module

### Testing Advanced

- **TEST-ADV-01**: Property-based testing (requires generics)
- **TEST-ADV-02**: Test mocking framework (requires reflection)
- **TEST-ADV-03**: Benchmarking with `bench_*` function convention

## Out of Scope

| Feature | Reason |
|---------|--------|
| Networking / HTTP module | Separate future milestone, different scope |
| Database drivers | Application territory, use C FFI |
| Async / non-blocking IO | Requires event loop (libuv-scale complexity) |
| String formatting / printf | Iron already has string interpolation |
| File system watchers | Requires inotify/kqueue/ReadDirectoryChangesW |
| Package manager | Separate project |
| LSP implementation | This project prepares stdlib content, not LSP infrastructure |

**Coverage:**
- v1 requirements: 88 total
- Mapped to phases: 88
- Unmapped: 0

---
*Requirements defined: 2026-04-02*
*Last updated: 2026-04-02 after initial definition*
