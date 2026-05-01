# Installing from Source
**Note: This document describes building Iron from source. This is not recommended if you don't know what you're doing.**

**Tracks:** Iron v3.1.1-alpha and newer (main branch). Older v1.2.x source tarballs follow their own per-release INSTALL.md.

Iron compiles to C and produces native binaries. You need a C compiler and CMake to build the Iron compiler itself. Programs compiled with Iron are standalone executables.

After a successful build, `./build/iron --version` and `./build/ironc --version` will both print `3.1.1-alpha (<git-sha>, <utc-date>)`. If the version line does not start with `3.1.`, your checkout is out of date or on a stale branch — `git pull` and rebuild.

## Requirements

| Tool | Version | Notes |
|------|---------|-------|
| CMake | 3.25+ | Build system |
| C compiler | C17 support | clang |
| Ninja | any | Recommended (faster builds) |

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
  npm run package      # produces iron-lsp-0.1.0.vsix
  code --install-extension iron-lsp-0.1.0.vsix
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
caveat in `editors/zed/README.md` (Phase 7 HARD-21 adds macOS code
signing + notarization; for now the binary download works on unsigned
macOS with a Gatekeeper allow-override).

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
 [clang / gcc]     ← must be available on the system
       |
       v
 native binary     ← standalone, no Iron runtime needed
```

Iron programs require a C compiler (clang or gcc) on the system at compile time, but the resulting binaries are self-contained and can be distributed without any Iron or C toolchain.

## Troubleshooting

**CMake version too old**: Install a newer version from https://cmake.org/download/ or via your package manager.

**No C compiler found**: Install clang (`brew install llvm` on macOS, `apt install clang` on Ubuntu) or gcc.

**Tests fail with sanitizer errors**: This is expected in Debug mode on some platforms. Build with `-DCMAKE_BUILD_TYPE=Release` to disable sanitizers.

**Windows build errors**: Windows support is experimental. Use Visual Studio 2022 with the C/C++ workload installed.
