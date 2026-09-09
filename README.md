# The Iron Programming Language

**Native performance. Explicit control. Readable code.**

[Iron] is a general-purpose native programming language for command-line tools,
network services, data processing, simulations, and games. It combines explicit
memory management and first-class concurrency with a clear, strongly typed syntax.

This repository contains the compiler, standard library, and documentation.

> **Alpha** — early release, expect breaking changes.

[Iron]: https://github.com/victorl2/iron-lang

## Install

```sh
curl --proto '=https' --tlsv1.2 -sSfL https://ironlang.dev/install.sh | sh
```

Pre-built binaries are available for **macOS** (arm64, x86_64) and **Linux** (x86_64) on the [releases page](https://github.com/victorl2/iron-lang/releases).

## Why Iron?

- **Performance:** Iron compiles to C and produces native binaries with the runtime statically linked. No tracing garbage collector or Iron VM.

- **Control:** Choose stack allocation, explicit heap lifetimes, or reference-counted shared ownership, with compiler and runtime checks.

- **Concurrency:** Thread pools, parallel loops, and concurrency primitives are first-class language features, not library afterthoughts.

- **Legibility:** No operator overloading, no implicit conversions, no hidden control flow. When you read Iron code, you know what it does.

## Quick Start

From a checkout of this repository, try the runnable [native summary example](docs/examples/native_summary.iron):

```sh
iron run docs/examples/native_summary.iron
```

Read the [language overview](docs/language_definition.md) for a tour of the
language and its features.

For TCP, UDP, DNS, HTTP/HTTPS, REST servers, webpages, WebSocket/WSS, and
binary-safe file examples, see the [networking guide](docs/networking.md).

For graphics, interactive applications, and games, explore the
[Raylib guide](https://ironlang.dev/raylib/) and [Pong example](examples/pong/README.md).
These are part of what you can build with Iron, not a requirement for using it.

## Building from Source

If you prefer to build from source, see [INSTALL.md](INSTALL.md) for instructions.

## Getting Help

This project is in early alpha. If you run into problems, please
[open an issue](https://github.com/victorl2/iron-lang/issues) on GitHub.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Iron is distributed under the terms of the Apache License (Version 2.0).

See [LICENSE](LICENSE) for details.
