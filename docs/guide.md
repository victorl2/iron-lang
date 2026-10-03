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
public surface. It never touches the network. `--release` builds with
optimization and `--verbose` prints the generated C and the compiler
command line; both pass through to `ironc`.

### iron run

```sh
$ iron run
   Compiling my-app v0.1.0
Built: /home/you/my-app/target/run/my-app
    Finished dev [unoptimized] in 2.02s
     Running /home/you/my-app/target/run/my-app
Hello, Iron!
```

Arguments after `--` go to the program: `iron run -- --port 8080`.

### iron check

```sh
$ iron check
    Checking my-app v0.1.0
    Finished check completed
```

Type-checks the project, vendored code included, without producing a
binary.

### iron test

```sh
$ iron test
     Testing my-app v0.1.0
[RUN ] test_math.iron
ok
[PASS] test_math.iron

Results: 1 passed, 0 failed, 1 total
    Finished all tests passed
```

Builds and runs every `.iron` file under `tests/`; a test passes when its
program exits with status 0.

### iron fmt

```sh
$ iron fmt src/main.iron
$ iron fmt --check src/main.iron
would reformat src/main.iron
```

Formats a file in place. `--check` rewrites nothing and exits 0 when the
file is already formatted, 1 when it would change, 2 on a syntax error.

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
