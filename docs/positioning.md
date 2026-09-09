# Describing Iron

## Core message

**Native performance. Explicit control. Readable code.**

Iron is a general-purpose native programming language for command-line tools,
network services, data processing, simulations, and games. It combines explicit
memory management and first-class concurrency with a clear, strongly typed syntax.

Use this positioning in the README, website, repository description, and other
introductions to the project. Keep the Iron name and existing visual identity.

## What to emphasize

- Native compilation through C, without an Iron VM or tracing garbage collector.
- Explicit memory and resource management, with compiler and runtime checks.
- Concurrency features and networking libraries for work beyond graphics.
- Readable code and visible control flow.
- An open-source project that welcomes contributors and questions.

Iron is in alpha. Describe available capabilities without promising production
readiness, universal memory safety, guaranteed zero overhead, or the fastest
possible code. Performance depends on the program, compiler options, and platform.

## Examples and documentation

Lead with a small runnable native program. Give tools, services, data processing,
simulations, and games a place in the documentation. Keep dedicated Raylib and
game examples: broadening Iron's audience does not remove graphics support.

Do not present ECS performance as the identity of the language or as a universal
guarantee. Explain static dispatch and data-layout optimizations where relevant,
with their conditions and tradeoffs.

Preserve historical release notes, changelog entries, and third-party material.
Their original terminology describes a specific release or project, not Iron's
current positioning. Label older examples and link to current guides when needed.

## Checking changes

Run the lightweight checks without building the compiler:

```sh
python3 scripts/test_branding.py
python3 scripts/check_site.py docs/site
```

The homepage and reference page mark runnable snippets with `data-example`.
Keep these snippets identical to their source in `docs/examples/native_*.iron`.
To compile and run all four featured examples and verify their output:

```sh
python3 scripts/check_branding.py --compiler ./build/iron
```

Documentation checks run on documentation PRs. The existing Doc Test build also
executes these examples when their source or the compiler changes; Pages validates
the site before publishing. Prose-only changes do not need a compiler for the new
static checks.
