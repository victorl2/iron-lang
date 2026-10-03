# Installing from Source
This guide builds Iron, a general-purpose native programming language, from source.
For pre-built binaries, use the [installation guide](https://ironlang.dev/install/).

**Tracks:** Iron v4.0.0-alpha and newer (main branch). Older v1.2.x source tarballs follow their own per-release INSTALL.md.

Iron compiles to C and produces native binaries for tools, services, simulations,
games, and other software. You need a C compiler and CMake to build the Iron
compiler itself. The Iron runtime is linked into compiled programs; applications
may still depend on system or external libraries they use.

After a successful build, `./build/iron --version` and `./build/ironc --version` will both print `4.0.0-alpha (<git-sha>, <utc-date>)`. If the version line does not start with `4.0`, your checkout is out of date or on a stale branch — `git pull` and rebuild.

## Requirements

| Tool | Version | Notes |
|------|---------|-------|
| CMake | 3.25+ | Build system |
| C compiler | C17 support | clang or gcc, to build the compiler itself |
| Ninja | any | Recommended (faster builds) |

The compiler you build with is not the one Iron programs are compiled
with: `ironc` uses its own pinned LLVM toolchain, downloaded on first use
(see "How Iron Compilation Works" below).

On Linux, building/running the raylib manual test suite additionally
requires the X11/GL development headers:

```bash
sudo apt-get install -y libx11-dev libxcursor-dev libxrandr-dev \
  libxinerama-dev libxi-dev libgl1-mesa-dev libxkbcommon-dev
```

## Quick Install

### macOS

```bash
# Install build tools (if not already present)
brew install cmake ninja

# Clone and build
git clone https://github.com/victorl2/iron-lang.git
cd iron-lang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build

# Verify
./build/iron --version
```

### Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build clang

git clone https://github.com/victorl2/iron-lang.git
cd iron-lang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -G Ninja
cmake --build build

./build/iron --version
```

### Linux (Fedora/RHEL)

```bash
sudo dnf install cmake ninja-build clang

git clone https://github.com/victorl2/iron-lang.git
cd iron-lang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -G Ninja
cmake --build build

./build/iron --version
```

### Windows

```powershell
# Install CMake (if not already present)
choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System' -y

git clone https://github.com/victorl2/iron-lang.git
cd iron-lang
cmake -B build -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022"
cmake --build build --config Release

.\build\Release\iron.exe --version
```

## Add to PATH

To use `iron` from anywhere, add the build directory to your PATH.

### macOS / Linux

```bash
# Add to your shell profile (~/.bashrc, ~/.zshrc, etc.)
export PATH="/path/to/iron-lang/build:$PATH"
```

### Windows

```powershell
# Add to user PATH (PowerShell)
[Environment]::SetEnvironmentVariable("Path", "$env:Path;C:\path\to\iron-lang\build\Release", "User")
```

## Verify Installation

```bash
iron --version       # Print version
iron run docs/examples/hello.iron   # Compile and run a program
```

## Install the editor extension

After building `ironls` (the Iron Language Server — built automatically
by the `cmake --build build` step above), install the extension for
your editor to get syntax highlighting + LSP integration (diagnostics,
go-to-definition, rename, hover, formatting, quickfixes).

The three extensions share a single language-intelligence source: all
semantic answers flow through `ironls` and stay byte-for-byte
consistent with the `ironc` compiler. See
[docs/dev/editor-extensions.md](docs/dev/editor-extensions.md) for the
architecture + dev-reload steps.

### VSCode

See [editors/vscode/README.md](editors/vscode/README.md).

Quick start:

- **From the VSCode Marketplace** (recommended — coming soon): search
  **Iron LSP** (publisher `iron-lang`).
- **From source** (until Marketplace publish lands):
  ```bash
  cd editors/vscode
  npm install
  npm run package      # produces iron-lsp-4.0.0-alpha.vsix
  code --install-extension iron-lsp-4.0.0-alpha.vsix
  ```

Minimum VSCode: 1.92+.

### Neovim (0.11.3+)

See [editors/neovim/README.md](editors/neovim/README.md).

Quick start — ensure `editors/neovim/{lsp,ftdetect,plugin}` is on your
runtimepath, then in your `init.lua`:

```lua
vim.lsp.enable('ironls')
```

Minimum Neovim: **0.11.3** (for the native `vim.lsp.config()` API).
Earlier versions are not supported; `editors/neovim/lsp/ironls.lua`
emits a clear `vim.notify` error if launched on an older Neovim.

The plugin manager snippets for `lazy.nvim` + `pckr.nvim` are in
[editors/neovim/README.md](editors/neovim/README.md).

### Zed

See [editors/zed/README.md](editors/zed/README.md).

Quick start:

- **From the Zed extensions registry** (recommended — coming soon):
  search **Iron LSP** (publisher `iron-lang`). The extension downloads
  a SHA-256-verified `ironls` binary from the matching GitHub Release
  automatically — no manual install.
- **From source** (dev-loaded until registry publish lands):
  ```bash
  cd editors/zed
  cargo build --target wasm32-wasip2 --release
  zed --dev-extension .
  ```

Minimum Zed: **0.200+**. Linux support is best-effort in v1 per the
caveat in `editors/zed/README.md`. macOS `ironls` release binaries are
Developer-ID signed, notarized, and stapled by the release workflow
(HARD-21), so the extension's binary download runs without any
Gatekeeper override.

### Architecture + contributing

See [docs/dev/editor-extensions.md](docs/dev/editor-extensions.md) for
the extension → LSP client → `ironls` → compiler pipeline, per-editor
dev-reload steps, and LSP wire tracing tips.

## Running Tests

```bash
# Unit tests
ctest --test-dir build --output-on-failure

# Integration tests (macOS/Linux only)
./tests/integration/run_integration.sh ./build/iron
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | Debug | `Debug` enables sanitizers, `Release` enables `-O3` |
| `CMAKE_C_COMPILER` | system default | Set to `clang` for best results |
| `-G Ninja` | — | Use Ninja instead of Make (faster) |

## How Iron Compilation Works

```
your_program.iron
       |
       v
 [Iron compiler]   ← this is what you just built
       |
       v
  generated .c file
       |
       v
 [Iron toolchain]  ← pinned clang, downloaded on first use
       |
       v
 native binary     ← standalone, no Iron runtime needed
```

Iron programs are compiled by a pinned C toolchain that Iron ships: clang,
lld and compiler-rt from one LLVM release, built by the Toolchain workflow
and published as release assets. `ironc` looks for it at `$IRON_TOOLCHAIN`,
then `<prefix>/lib/iron/toolchain/`, then `~/.iron/toolchain/<version>/`,
downloading the per-user copy on first use and checking its SHA-256
against the pins in `src/cli/toolchain_pins.h`. It never uses the clang
on `PATH`. `iron --version` prints the toolchain in use and
`iron toolchain install` fetches it ahead of time. The resulting binaries
are self-contained and can be distributed without any Iron or C toolchain.

Until the runtime ships precompiled, macOS still needs the Xcode command
line tools (for the SDK headers the runtime sources are compiled against),
Linux the C library headers (`libc6-dev` or equivalent) and Windows the
Visual Studio Build Tools. When one is missing, `ironc` says so instead of
failing with a compiler error about `stdio.h`, and from a terminal offers
to run the installer (`xcode-select --install`, the distribution's package
manager, `winget install Microsoft.VisualStudio.2022.BuildTools`).
`iron toolchain check` runs that probe on demand.

## Troubleshooting

**CMake version too old**: Install a newer version from https://cmake.org/download/ or via your package manager.

**No C compiler found**: for building `ironc`, install clang (`brew install llvm` on macOS, `apt install clang` on Ubuntu) or gcc.

**The Iron toolchain is not installed**: run `iron toolchain install`, or
unpack the bundle for your host from the `toolchain-*` release into
`~/.iron/toolchain/<version>/`. Developers with their own LLVM can run
`scripts/toolchain/pack.sh` on it and point `IRON_TOOLCHAIN` at the result
(a version mismatch warns instead of failing).

**Tests fail with sanitizer errors**: This is expected in Debug mode on some platforms. Build with `-DCMAKE_BUILD_TYPE=Release` to disable sanitizers.

**Windows**: the release archive is `iron-<version>-windows-x86_64.zip` (installed by `irm https://ironlang.dev/install.ps1 | iex`). To build from source, install the Visual Studio Build Tools with the "Desktop development with C++" workload plus clang, open a Developer PowerShell (or run `VsDevCmd.bat -arch=x64`) and configure with `cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DIRON_BUILD_LSP=OFF`. The language server does not build on Windows yet.
