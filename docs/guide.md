# Projects

Create, build, run and test Iron projects, and bring in third-party code
by vendoring it. This guide covers the `iron` project commands; the
language itself is in the [reference manual](language_definition.md). It is
published at [ironlang.dev/guide](https://ironlang.dev/guide/), generated
from this file.

## Two binaries

Iron ships two binaries, in the spirit of Cargo and rustc:

| Binary | Role |
|---|---|
| `iron` | The project tool: scaffolding, builds, runs, checks, tests, formatting |
| `ironc` | The compiler: compiles one `.iron` file to a native binary |

For most work you only use `iron`. It reads the project manifest
(`iron.toml`), gathers the sources (including anything vendored under
`vendor/`) and invokes `ironc`. Given a `.iron` file argument
(`iron build hello.iron`, `iron run script.iron`) it forwards to `ironc`
for a single-file build, so one command covers both.

Iron has no package manager: no registry, no download step, no lockfile.
See [Third-party code](#third-party-code).

Both binaries are installed side by side in `~/.iron/bin/` and `iron`
finds `ironc` next to itself. Programs are compiled with Iron's own pinned
C toolchain, downloaded on first use; `iron --version` prints which one
(see the manual's [backend section](language_definition.md#106-the-backend)).

## Creating a project

```sh
$ iron init
     Created binary `my-app` package
```

creates, in the current directory:

```text
my-app/
    iron.toml          # the manifest
    .gitignore         # ignores target/
    src/
        main.iron      # the entry point
```

and runs `git init` when the directory is not already in a repository.
`iron init --lib` creates a library package with `src/lib.iron` instead.
`iron init` works in a non-empty directory: it creates the files that do
not exist and leaves the others alone.

A grown project looks like this:

```text
my-app/
    iron.toml
    .gitignore
    src/
        main.iron      # bin entry point (lib.iron for a library)
    vendor/            # third-party source, committed (optional)
    tests/
        test_math.iron # found by iron test
    target/
        my-app         # the built binary
        combined.iron  # the sources as one program (vendored code, several files)
```

The entry point is a convention: `src/main.iron` for a binary,
`src/lib.iron` for a library; there is no `entry` field.

## The manifest

```toml
[package]
name        = "my-app"
version     = "0.1.0"
type        = "bin"             # "bin" (default) or "lib"
description = "A cool project"  # optional
iron        = ">= 4.6.0-alpha"  # optional compiler version constraint
```

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | The package name; also the binary's file name |
| `version` | yes | A semantic version |
| `type` | no | `"bin"` (default) or `"lib"` |
| `description` | no | A short description |
| `iron` | no | A Cargo-style semver constraint on the compiler, checked by `iron build` and `iron run` |

An optional `[fmt]` table configures the formatter (`indent_width`,
default 4, `line_width`, `use_tabs`), and a `[web]` table configures the
web target
(see the manual's [web target section](language_definition.md#105-the-web-target)).
There is no dependency table.

## Commands

### iron build

```sh
$ iron build
   Compiling my-app v0.1.0
Built: /home/you/my-app/target/my-app
    Finished dev [unoptimized] in 1.99s
```

Reads `iron.toml`, gathers `vendor/` and `src/`, compiles them as one
program and writes the binary to `target/`; a `type = "lib"` package
produces `target/lib<name>.a` and a `<name>.iron-stub` describing its
public surface. `--release` builds with optimization and `--verbose`
prints the generated C and the compiler command line; both pass through
to `ironc`.

`iron build --target=linux-x86_64` (or `linux-arm64`) cross compiles a
binary package into a static Linux executable under `target/<target>/`,
`--target=macos-arm64` (or `macos-x86_64`) into a macOS executable and
`--target=windows-x86_64` into a Windows `.exe`, from any host and
without an SDK: the program is linked against a
precompiled runtime bundle that `ironc` downloads once per compiler
version into `~/.iron/rt/` (see the manual's
[backend section](language_definition.md#106-the-backend)). Apart from
that first download `iron build` never touches the network.

### Debugging

`iron build --debug` (or `ironc build file.iron --debug`, and the same for
`run`) builds a binary whose debug information points at the `.iron`
source: a breakpoint on `main.iron:12` stops on that line, stepping moves
from Iron line to Iron line and the call stack lists Iron functions. The
build skips optimization and function inlining so every function keeps
its frame. Any C debugger reads it: gdb and lldb on Linux and macOS, the
Visual Studio debugger (a PDB is written next to the `.exe`) on Windows.

```sh
$ iron debug                       # in a package; or: iron debug main.iron
(gdb) break main.iron:12
(gdb) run
```

A panic (an index out of bounds, a failed `assert`, a missing map key) stops
the debugger on the Iron line that failed: `iron debug` and `iron dap`
put a breakpoint on the C library's `abort`, where every panic ends, and
select the Iron frame that panicked (`Iron panic at main.iron:6`), so
`print` and the locals show that function's values. Through `iron dap`
the stop is reported as an exception with the panic's message and the
call stack starts at the Iron frame; `"stopOnPanic": false` in the launch
configuration turns this off. In LLDB started by hand, run
`iron-panic-stop` after loading the formatters; in gdb, `break abort`.

`iron debug` builds with `--debug` and starts the debugger on the
program with the value formatters below already loaded: LLDB on macOS,
gdb elsewhere (`--gdb` / `--lldb` choose). Arguments after `--` go to
the program. Building with `iron build --debug` and starting a debugger
yourself works the same way.

`iron dap` is the debug adapter for editors: a Debug Adapter Protocol
server on standard input and output that VS Code, Neovim (nvim-dap) and
Zed can run. Its `launch` request takes `program` (a `.iron` file, a
package directory, or a binary that is already built), `args`, `cwd`,
`env` and `stopOnEntry`. It builds the program with `--debug`, runs
`lldb-dap` (from LLVM or Xcode) or, without it, gdb 14 or later in its
DAP mode, loads the value formatters below, lists locals under their
Iron names without the compiler's temporaries, and shows Iron function
names in the call stack. `editors/neovim` (nvim-dap) and `editors/zed`
(Zed's debugger) configure it; their READMEs have the details.
`--adapter <path>` (or `IRON_DAP_ADAPTER`)
picks the debugger. It needs Python 3; the pinned toolchain does not
include `lldb-dap`.

In VS Code the Iron extension lets you set breakpoints in `.iron` files;
pair it with a C debugger extension (C/C++ from Microsoft, or CodeLLDB)
and a build task:

```jsonc
// .vscode/tasks.json
{ "version": "2.0.0",
  "tasks": [{ "label": "iron: build --debug", "type": "shell",
              "command": "iron build --debug", "problemMatcher": [] }] }

// .vscode/launch.json
{ "version": "0.2.0",
  "configurations": [{
    "name": "Iron: debug",
    "type": "cppdbg",            // "cppvsdbg" on Windows, "lldb" with CodeLLDB
    "request": "launch",
    "program": "${workspaceFolder}/target/my-app",
    "cwd": "${workspaceFolder}",
    "MIMode": "gdb",             // "lldb" on macOS
    "preLaunchTask": "iron: build --debug"
  }]
}
```

Parameters and local bindings show under their Iron names (`w`, `total`);
a name used twice in one function, or one that is also a C keyword,
carries a suffix (`total_14`). The compiler's own temporaries appear as
`_v12`.

`lib/debug/` in the Iron installation holds formatters that show values
as Iron values: a `String` as its text, a list or set as its elements,
a map as its entries and a `T?` as its value or `null`. Load
`iron_gdb.py` in gdb and `iron_lldb.py` in LLDB:

```sh
(gdb) source ~/.iron/lib/debug/iron_gdb.py
(lldb) command script import ~/.iron/lib/debug/iron_lldb.py
```

In the VS Code configuration above, add
`"setupCommands": [{ "text": "source ~/.iron/lib/debug/iron_gdb.py" }]`
(cppdbg with gdb) or
`"initCommands": ["command script import ~/.iron/lib/debug/iron_lldb.py"]`
(CodeLLDB). Adjust the path to where Iron is installed.

### iron run

```sh
$ iron run
   Compiling my-app v0.1.0
Built: /home/you/my-app/target/run/my-app
    Finished dev [unoptimized] in 2.02s
     Running /home/you/my-app/target/run/my-app
Hello, Iron!
```

Arguments after `--` go to the program: `iron run -- --port 8080`. The
program receives them as its `main` parameter, `func main(args: [String])`,
and reads environment variables with `OS.env(name)` (`import os`).

### iron check

```sh
$ iron check
    Checking my-app v0.1.0
    Finished check completed
```

Type-checks the project, vendored code included, without producing a
binary.

### iron test

A test is a `test "name" { ... }` block, in any file of `src/` (next to the
code it tests, with access to its private functions) or `tests/` (with the
project's `pub` declarations):

<!-- doctest-skip: a test block needs `iron test` to run -->
```iron
func double(x: Int) -> Int {
    return x * 2
}

test "double doubles" {
    assert_eq(double(21), 42)
}

test "double of a negative" {
    assert_eq(double(-3), -6)
    assert_ne(double(1), 1)
}
```

```sh
$ iron test
     Testing my-app v0.1.0
test double doubles ... ok
test double of a negative ... ok

2 passed, 0 failed
    Finished all tests passed
```

`iron test` compiles `vendor/`, `src/` and `tests/` into one test program
and runs each test in its own process, several at a time: a failed
`assert`, `assert_eq` or a panic fails that test, with its output and the
source line, and the others still run. `iron test <filter>` runs the tests
whose name contains `<filter>`. The exit status is 1 when a test fails.
`iron build` and `iron run` type-check test blocks but leave them out of the
program, and the program's `main` is not run by the tests.

A `tests/test_*.iron` file with its own `func main` is a standalone test
program, run as a whole: it passes when it exits with status 0.

`ironc test file.iron [filter]` runs the test blocks of a single file, and
`ironc build --test` builds the test program itself (`--iron-list` prints
the test names, `--iron-test <n>` runs one).

### iron fmt

```sh
$ iron fmt src/main.iron
$ iron fmt --check src/main.iron
would reformat src/main.iron
```

Formats a file in place: it re-indents each line from the nesting of the
brackets around it (four spaces per level, or `[fmt] indent_width` and
`use_tabs` in `iron.toml`), removes trailing whitespace and runs of blank
lines, and changes nothing else, so comments and the line breaks you chose
stay as written. `--check` rewrites nothing and exits 0 when the file is
already formatted, 1 when it would change, 2 on a syntax error.

## Third-party code

Iron deliberately has no package manager: no registry, no `iron add`, no
dependency resolver, no lockfile. The standard library is meant to cover
the common ground (collections, strings, math, I/O, time, logging,
networking, HTTP) and raylib ships with the compiler.

When you need someone else's code you take a copy of it, as Odin projects
do: the source goes into your repository, you read it, and you own it.
Builds are reproducible because everything they compile is committed, and
they never reach out to the network.

### Vendoring

Put third-party code under `vendor/`, one subdirectory per library.
Nothing is declared in `iron.toml`:

```text
my-game/
    iron.toml
    src/
        main.iron
    vendor/
        greeter/           # an Iron library project, copied as is
            iron.toml
            LICENSE
            src/
                lib.iron
        noise/             # or loose .iron files
            perlin.iron
```

Take the copy however suits you:

```sh
# a release archive
mkdir -p vendor/greeter
curl -sSL https://example.com/greeter-0.3.0.tar.gz | tar -xz --strip-components=1 -C vendor/greeter

# or track an upstream repository with git subtree
git subtree add --prefix vendor/greeter https://example.com/greeter.git v0.3.0 --squash
```

Vendored code is compiled into the same program as your own, so its `pub`
functions and types are available directly. A library's `src/lib.iron`:

```iron
pub func greet(name: String) -> String {
    return "Hello, {name}!"
}
```

and the project's `src/main.iron`, where `import greeter` is optional and
documents where the names come from:

<!-- doctest-skip: needs the vendored greeter library -->
```iron
import greeter

func main() {
    println(greet("Iron"))
}
```

Vendored code shares the project's namespace. If two libraries declare
the same name the build fails with a duplicate declaration error; rename
one of them in your copy. Aliased imports (`import greeter as g`) are not
supported for vendored or project-local modules yet.

### What gets compiled

`iron build`, `iron run` and `iron check` collect every `.iron` file under
`vendor/`, then the project's own `src/*.iron`, into
`target/combined.iron` and compile that as one program:

| Under `vendor/` | Compiled |
|---|---|
| A directory with its own `iron.toml` and `src/` | only its `src/`, so a library project can be dropped in unchanged |
| any other directory | every `.iron` file, recursively |
| `tests/`, `examples/`, `target/` | no |
| hidden directories (`.git`, ...) | no |
| other files (`LICENSE`, `README.md`, ...) | no; keep the license next to the code |

Files are compiled in sorted path order. An error in vendored code is
reported against `target/combined.iron`; the `-- vendor:` comment above
each file says where it came from.

### Updating vendored code

Updating a dependency is an ordinary change to your repository: replace
the directory with a newer copy (or `git subtree pull`), rebuild, and
review the diff before committing. Local patches are edits; keep a note in
the vendored directory if they have to be reapplied after the next update.

### Sharing a library

Publish its source (a repository or an archive) with an `iron.toml` and a
`src/` directory, as `iron init --lib` creates them. Users copy it into
their `vendor/`.

### C libraries

Not yet: `extern func` can call C functions the compiler already declares
(the C library, raylib), but a project cannot yet declare and link an
arbitrary vendored C library.

## Environment

| Variable | Meaning |
|---|---|
| `NO_COLOR` | Disable colored output when set to any value |
| `FORCE_COLOR` | Force colored output when not writing to a terminal |
| `IRON_TOOLCHAIN` | Use the C toolchain bundle at this path (developer override) |
| `IRON_LIB_DIR` | Use the standard library at this path (developer override) |

## Errors

| Message | Cause | Fix |
|---|---|---|
| `iron.toml declares dependency '...'` | the manifest still has a `[dependencies]` table | vendor the code and delete the table |
| `duplicate declaration` | two vendored libraries, or a library and your code, declare the same name | rename one of them in your copy |
| `no iron.toml found` | not in a project directory | `iron init`, or `cd` into the project |
| `the Iron toolchain ... is not installed` | the pinned C toolchain could not be downloaded | `iron toolchain install`, see INSTALL.md |
