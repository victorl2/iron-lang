# Installing Iron

Iron compiles to C and produces native binaries. You need a C compiler and CMake to build the Iron compiler itself. Programs compiled with Iron have no dependencies — they are standalone executables.

## Requirements

| Tool | Version | Notes |
|------|---------|-------|
| CMake | 3.25+ | Build system |
| C compiler | C17 support | clang (preferred) or gcc |
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
iron run examples/hello.iron   # Compile and run a program
```

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
