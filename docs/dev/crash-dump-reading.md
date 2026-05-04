# Reading Iron LSP Crash Dumps

Phase 7 Plan 07-01 (HARD-14, D-02) introduced a crash-dump pipeline that
writes a plain-text `<iso8601>-<pid>.dmp` file to
`$XDG_STATE_HOME/iron-lsp/crashes/` (fallback:
`$HOME/.local/state/iron-lsp/crashes/`) whenever the server receives
`SIGSEGV` or `SIGBUS`. `SIGABRT` continues to flow through the Phase 2
sigsetjmp per-document quarantine path and does NOT write a crash dump
by default — we have per-document diagnostics for that case already.

## Format overview

Each dump is a plain-text file with 3 `===`-delimited sections:

```
Iron LSP crash dump
signal=11 pid=12345

=== BACKTRACE ===
./ironls(+0x12345) [0x7f5a...]
./ironls(+0x23456) [0x7f5a...]
/lib/x86_64-linux-gnu/libc.so.6(+0x...) [0x7f5a...]
...

=== IN-FLIGHT REQUESTS ===
id=42 method=textDocument/hover
id=41 method=textDocument/didChange
...

=== DOCUMENT STATE ===
IRON_VERSION_FULL=1.2.0-alpha
OS=Linux x86_64
WORKSPACE=/home/user/myproj
```

## Resolving BACKTRACE frames with addr2line

The BACKTRACE section contains **raw addresses** — not human-readable
function names. This is a deliberate discipline decision: the glibc
`backtrace_symbols()` routine allocates memory, which is not allowed
from a signal handler. We use `backtrace_symbols_fd()` instead, which
writes verbatim addresses directly via `write(2)` (async-signal-safe).

To resolve a frame's address to a function name + source line:

```bash
addr2line -e ./ironls -f -C -p -i 0x<address>
```

Flags:

- `-f`: print the function name
- `-C`: demangle (harmless for pure-C Iron, useful if we ever vendor a
  C++ dep)
- `-p`: pretty-print (function at file:line)
- `-i`: expand inlined frames

Example session:

```bash
$ addr2line -e build/ironls -f -C -p -i 0x4f2a3
ilsp_facade_compile at src/lsp/facade/compile.c:137
```

If the frame comes from glibc, use the distro's debug symbols:

```bash
addr2line -e /usr/lib/debug/lib/x86_64-linux-gnu/libc.so.6 -f -p 0x...
```

## The IN-FLIGHT REQUESTS section

The dispatcher pushes `(request_id, method)` into a 16-slot lock-free
ring buffer at handler entry and pops at handler return. The signal
handler reads the 16 most-recent slots (torn-read-tolerant; see
`T-07-01-02` in the plan's threat model). A crash inside
`textDocument/hover` will show:

- the hover call itself at the top
- the most-recent requests that preceded it (debounced
  `didChange`, background `diagnostic` pulls, etc.)

This is the single most useful diagnostic signal for correlating the
backtrace with client activity.

## DOCUMENT STATE

Three facts are captured at install time (not at crash time, so they
never block the handler):

- `IRON_VERSION_FULL`: compiled-in version string. Use this to verify
  the dump came from the build you think it did.
- `OS`: `uname` sysname + machine arch.
- `WORKSPACE`: the last value passed to `ilsp_crash_set_workspace_root()`
  (populated by `handlers_lifecycle.c` when `initialize` resolves a
  workspace). If `(unset)` appears, the crash happened before a
  workspace was opened — for example, during `initialize` itself, or
  in an editor that never sends `workspaceFolders`.

## Known limitations

- **musl libc (Alpine, etc.):** musl ships a `backtrace(3)` stub that
  always returns 0 frames. The BACKTRACE section will be empty on
  musl-based distros. v1 targets glibc (Linux) + Apple libc (macOS);
  musl support is a v1.x tracking item.
- **Stripped builds:** `addr2line` requires DWARF debug info. Release
  builds normally carry `-g -O2` so this works, but fully-stripped
  binaries will show frame addresses without source attribution. Keep
  an un-stripped build of the same commit around for triage.
- **Signal-in-signal recursion:** SIGSEGV's handler uses `SA_RESETHAND`
  so a second SIGSEGV during dump writing kills the process with the
  default disposition rather than looping. The consequence is: a
  crashy dump path (disk full, permission denied) will yield a
  partial or missing dump — NOT a hang.

## Privacy warning

**Crash dumps include the workspace root path (which may be a full
home-directory path) and the methods of in-flight requests**. They do
NOT include the contents of open documents or any user code — but the
workspace path and the sequence of LSP methods can leak information
about what you were working on.

Do NOT post crash dumps to public issue trackers without redacting:

- The `WORKSPACE=` line (replace with `/redacted/`).
- Any path fragments inside the BACKTRACE (unlikely on modern Linux
  with PIE builds, but possible on macOS stack frames that carry
  full `/Users/<name>/...` paths).

A future Phase 7+ iteration (OPS-01) will add an `ironls
dump-redact <file>` helper that does this automatically.

## Testing the pipeline manually

```bash
# Start the server with a temp state dir
mkdir -p /tmp/ironls-test
XDG_STATE_HOME=/tmp/ironls-test ./build/ironls --log-level=DEBUG &
PID=$!

# Hurt it
kill -SEGV $PID

# Inspect the dump
ls /tmp/ironls-test/iron-lsp/crashes/
cat /tmp/ironls-test/iron-lsp/crashes/*.dmp
```

You should see a 3-section file. The ring buffer will be empty
(`(no requests in flight)`) because no handlers were dispatched —
that's correct: the dispatcher ring only fills when the server is
actively handling requests.
