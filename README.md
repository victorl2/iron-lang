# The Iron Programming Language

This is the main source code repository for [Iron]. It contains the compiler,
standard library, and documentation.

> **Alpha v0.0.1** — early release, expect breaking changes.

[Iron]: https://github.com/victorl2/iron-lang

## Why Iron?

- **Performance:** Iron compiles to C and produces native binaries with the runtime statically linked. No garbage collector, no VM, no interpreter overhead.

- **Control:** You manage memory explicitly — stack, heap, reference counting — with compiler-assisted safety nets. No borrow checker, no hidden allocations.

- **Game-dev first:** Designed for game development from day one. Thread pools, parallel loops, `draw {}` blocks, and concurrency primitives are all first-class features.

- **Legibility:** No operator overloading, no implicit conversions, no hidden control flow. When you read Iron code, you know what it does.

## Quick Start

Read the [language overview](docs/language_definition.md) for a tour of the
language and its features.

## Installing from Source

If you want to build Iron from source, see [INSTALL.md](INSTALL.md) for
step-by-step instructions on macOS, Linux, and Windows.

## Getting Help

This project is in early alpha. If you run into issues, please
[open an issue](https://github.com/victorl2/iron-lang/issues) on GitHub.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Iron is distributed under the terms of the Apache License (Version 2.0).

See [LICENSE](LICENSE) for details.
