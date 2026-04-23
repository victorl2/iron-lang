# Requirements: Iron v3.0 — Method Ergonomics

**Defined:** 2026-04-20
**Core Value:** Methods live where behavior belongs — inside the object that owns the data — with explicit mutation tiers, mandatory construction, open extension, and a single visibility keyword. v3.0 is the first non-superset release; pre-v3 Iron code must migrate via the supplied codemod.

## Design Principles (Locked Decisions)

1. **Methods inside objects.** Receiver-method syntax `func (recv: T) name()` is removed. Methods live inside `object X { ... }` or `patch object X { ... }` blocks. Implicit `self` with required `self.field` prefix for field access.
2. **Mutation is explicit.** Three tiers: default (mutating), `readonly` (cannot write self), `pure` (no observable side effects). Contracts live in the signature; `readonly` transitively blocks calling mutating methods on `self`'s fields.
3. **Visibility is one keyword.** Everything defaults to private. `pub` opts into exposure. No `private` keyword — default IS private.
4. **Construction is mandatory.** Every object declares at least one `init`; inline defaults (`var x: Int = 0`) are forbidden; init must definitely-assign every field.
5. **Open extension via `patch`.** Types you don't own (including primitives) are extended with `patch object T { ... }`. Patches add methods and inits but never fields. Collisions are compile errors.
6. **No magic.** No operator overloading, no default argument values, no `static` keyword (type-level operations are module-qualified free functions), no custom getter/setter logic (use private backing field + explicit methods).
7. **Hard break.** v3.0 is the first release to break the pure-superset guard. Codemod handles all pre-v3 source migration.

## v3.0 Requirements

### BREAK — Hard removals

- **BREAK-01**: Parser rejects `func (recv: T) name()` with `E0101: receiver-method syntax removed in v3.0`, help text points to in-block or patch form and the codemod.
- **BREAK-02**: Parser rejects `func (mut recv: T) name()` with the same diagnostic; mut-receivers migrate to default-mutating in-block methods.
- **BREAK-03**: Parser rejects inline-default field declarations `var x: Int = 0` with `E03XC: inline default forbidden; assign in init`.
- **BREAK-04**: Parser rejects `mut` as a keyword in any position (receiver binding, param, local).
- **BREAK-05**: v3.0 is the first Iron release without the pure-superset guard. Release notes explicitly name the break.

### GRAMMAR — Methods-in-block + `self` mechanics

- **GRAMMAR-01**: Parser accepts `object X { func foo() { ... } }` — method declarations permitted directly inside object blocks.
- **GRAMMAR-02**: Inside a method body, `self` is implicit and refers to the enclosing object instance.
- **GRAMMAR-03**: Field access requires explicit `self.field` prefix. Bare identifiers always resolve to the innermost binding (parameter, local), never a field.
- **GRAMMAR-04**: Method-call syntax `obj.method(args)` dispatches to the method declared inside `obj`'s type or any patch of that type.
- **GRAMMAR-05**: `object` keyword retained (no rename to `definition`, `struct`, etc.).

### ACCESS — Visibility (single keyword)

- **ACCESS-01**: Declarations default to private visibility. Private fields/methods are accessible from within the enclosing object block and the containing module only.
- **ACCESS-02**: `pub` modifier exposes a field or method across module boundaries.
- **ACCESS-03**: `pub val field: T` synthesizes a `pub readonly func field() -> T` accessor.
- **ACCESS-04**: `pub var field: T` synthesizes both `pub readonly func field() -> T` and `pub func set_field(v: T)`.
- **ACCESS-05**: Property syntax `obj.field` (read) and `obj.field = v` (write) is reserved for `pub` fields — dispatches to the synthesized accessor. Manual accessor methods call explicitly.
- **ACCESS-06**: Synthesized accessors reserve the field name in the method namespace. Declaring `pub val x: T` plus `func x() -> T` is `E03XE: name 'x' reserved by synthesized accessor`.
- **ACCESS-07**: `priv` / `private` are not keywords in v3.0. Private is the default — no keyword is needed.
- **ACCESS-08**: No custom getter/setter syntax in v3.0. Authors needing logic use a private backing field plus explicit methods.

### MUTTIER — Mutation tiers

- **MUTTIER-01**: Methods default to *mutating*: body may write `self.field` and call any method on `self`.
- **MUTTIER-02**: `readonly func` prefix forbids writes to `self.field` and forbids calling mutating methods on any member of `self`. Violation: `E03F1: cannot write self.field in readonly method` or `E03F2: cannot call mutating method from readonly context`.
- **MUTTIER-03**: `pure func` prefix forbids writes to `self`, writes to any parameter, calls to non-pure functions, reads or writes of mutable globals, and I/O. Heap allocation IS allowed (GC-managed; not observable). Violations are `E03F3..E03F7` keyed per category.
- **MUTTIER-04**: Calling a mutating method requires the receiver binding to be `var`. `val`-bound callers receive `E0235: cannot call mutating method on immutable binding`.
- **MUTTIER-05**: `readonly` methods callable on both `val` and `var` bindings. `pure` methods callable from any context, including other `pure` methods.
- **MUTTIER-06**: Transitive enforcement: a `readonly` method may call only `readonly` or `pure` methods; a `pure` method may call only `pure` methods.
- **MUTTIER-07**: Init cannot be `readonly` or `pure` — init writes fields by definition.

### INIT — Mandatory construction

- **INIT-01**: Fields declared without defaults: `var name: T` and `val name: T` are legal (inline defaults are forbidden per BREAK-03).
- **INIT-02**: Every object declaration must contain at least one `init`. Object with zero inits: `E03XA: object has no init; cannot be constructed`.
- **INIT-03**: Init syntax: `init <name>(params) { body }` or anonymous `init(params) { body }`. Return type is implicit `Self`.
- **INIT-04**: Inside init, `self` is a partially-constructed instance. Fields are write-only until definitely-assigned, then readable.
- **INIT-05**: Reading an unassigned field in init: `E03XY: field 'health' read before assignment in init`.
- **INIT-06**: Init must definitely-assign every field on every exit path. Violation: `E03XZ: init 'new' leaves field 'health' unassigned on some path`.
- **INIT-07**: Multiple inits per object with unique names. Anonymous init allowed at most once per block. Duplicate anonymous inits: `E03XB: duplicate anonymous init`.
- **INIT-08**: Call syntax: anonymous init called as `Type(args)`; named init called as `Type.name(args)`.
- **INIT-09**: Init cannot call self-methods until all fields are definitely-assigned. Violation: `E03YX: method call on partially-constructed self`.
- **INIT-10**: Init cannot early-return unless all fields are definitely-assigned on that path.
- **INIT-11**: Init always succeeds. No `throws`, no `Result` init return. Fallible construction uses module-level free functions returning `Result[Self, E]`.
- **INIT-12**: `val` fields are single-assignment in init. Multiple writes: `E03YY: val field 'id' assigned more than once in init`.
- **INIT-13**: Field-less objects (`object Marker {}`) get a synthesized `init() {}` automatically.
- **INIT-14**: Init cannot delegate to another init (no Swift-style `self.init(...)`). Shared construction logic lives in module-level free functions.
- **INIT-15**: Interface declarations may not declare inits. Factory-pattern interfaces use `Self`-returning methods.
- **INIT-16**: Method bodies may call any init via `TypeName(args)` or `TypeName.name(args)`.

### PATCH — Open extension

- **PATCH-01**: `patch object MyType { ... }` syntax parses; body contains method and init declarations following the same rules as in-object definitions.
- **PATCH-02**: Patches apply program-wide once the defining module is linked — there is no file-local or import-scoped patching.
- **PATCH-03**: Duplicate method or init signatures across patches of the same type: `E03XX: conflicting patch definitions for MyType.foo`. No silent override, no last-write-wins.
- **PATCH-04**: Patch targets include user-defined `object` types and primitives (`Int`, `Int32`, `Float`, `String`, `Bool`).
- **PATCH-05**: Patches cannot add fields. Attempting to: `E03X1: patches may only add methods and inits`.
- **PATCH-06**: Patches cannot change visibility of existing members. Attempting to re-declare a member: `E03X2: member 'foo' already defined on 'MyType'`.
- **PATCH-07**: Patched inits follow definite-assignment identically to in-object inits — they must initialize all fields of the patched type on every exit path.
- **PATCH-08**: Patches may satisfy interface conformance retroactively (Haskell-style): `patch object Int implements Comparable { ... }` extends `Int` with interface methods.
- **PATCH-09**: Methods added via `patch` follow the same mutation-tier rules (default / `readonly` / `pure`) and visibility rules (`pub` opt-in) as in-object methods.

### IFACE — Interface integration

- **IFACE-01**: Interface method signatures accept `readonly` and `pure` modifiers.
- **IFACE-02**: Implementations must match or strengthen the modifier — `readonly` method implementable by `readonly` or `pure`, but not mutating. `pure` method implementable only by `pure`.
- **IFACE-03**: Interface default implementations allowed: `interface T { readonly func foo() -> Int { return 42 } }`. Implementers may override or inherit.
- **IFACE-04**: Interfaces declare methods only — no fields, no inits (INIT-15).
- **IFACE-05**: `Self` return type allowed in interface method signatures (see SELF-02).

### SELF — `Self` type

- **SELF-01**: `Self` is a legal type in return position inside method bodies, referring to the enclosing object type.
- **SELF-02**: `Self` is legal in interface method signatures, bound to the implementing type.
- **SELF-03**: `Self` as a type-constructor in expressions: `return Self(args)` constructs a value of the enclosing type via its anonymous init. Named inits use `Self.name(args)`.

### MIGR — Migration + codemod

- **MIGR-01**: All in-tree `.iron` files (stdlib, tests, examples, docs) migrate to v3 grammar in a single coordinated phase. No incremental migration.
- **MIGR-02**: Codemod `ironc migrate --from v2 --to v3 <path>` rewrites v2 source to v3 grammar. Deterministic, idempotent, emits diff on stderr.
- **MIGR-03**: Codemod coverage: receiver-method declarations → in-block or patch form; `mut` receivers → default in-block methods; inline defaults → init bodies; adds synthesized `init` when missing.
- **MIGR-04**: Codemod ships in the v3.0.0-alpha release binary.
- **MIGR-05**: Parser diagnostics for v2 syntax include migration hints: "help: run `ironc migrate --from v2 --to v3 <file>`".
- **MIGR-06**: Release notes lead with "breaking change" messaging — no silent upgrade, users pin to v2.2 until ready.

### IDENT — Generated-C identity

- **IDENT-01**: Codemod applied to any pre-v3 `.iron` file, then compiled with v3, produces byte-identical generated C to the v2.2 compilation. Lock per stdlib module via `tests/migrate_identity/*.iron`.
- **IDENT-02**: Release-blocker script `scripts/verify-v3-migration.sh` runs codemod → compile → diff across `src/stdlib/`, `examples/`, `tests/integration/`. Zero-diff required before tagging v3.0.0-alpha.
- **IDENT-03**: In-block methods lower to the same C ABI as v2.2 static-form receiver methods for the default-mutating case (`Iron_Type_method(Iron_Type *self, ...)` — pointer receiver for mutating, by-value for `readonly` / `pure`). No ABI-only refactors.

### TEST — Test matrix

- **TEST-01**: Integration test `tests/integration/v3_methods_in_block.iron` — declares object with in-block methods, calls through `obj.method()`, confirms expected output.
- **TEST-02**: Integration test `tests/integration/v3_patch_primitive.iron` — patches `Int` with `pub readonly func double() -> Int`, calls `5.double()`, confirms 10.
- **TEST-03**: Integration test `tests/integration/v3_init_anonymous_and_named.iron` — object with anonymous + named inits, constructs via `Type(args)` and `Type.name(args)`.
- **TEST-04**: Integration test `tests/integration/v3_pub_field_synthesis.iron` — `pub var health` on object, reads via `obj.health`, writes via `obj.health = 5`, confirms synthesized accessors work.
- **TEST-05**: Integration test `tests/integration/v3_readonly_transitive.iron` — readonly method calling only readonly methods on self's fields compiles cleanly.
- **TEST-06**: Integration test `tests/integration/v3_pure_method.iron` — pure method with heap allocation compiles; pure method with I/O or global write is rejected.
- **TEST-07**: Compile-fail fixture `tests/compile_fail/v3_missing_init.iron` — object without init triggers E03XA.
- **TEST-08**: Compile-fail fixture `tests/compile_fail/v3_init_unassigned_field.iron` — init leaving field unassigned triggers E03XZ.
- **TEST-09**: Compile-fail fixture `tests/compile_fail/v3_readonly_write_self.iron` — readonly method writing `self.field` triggers E03F1.
- **TEST-10**: Compile-fail fixture `tests/compile_fail/v3_readonly_calls_mutating.iron` — readonly method calling mutating method triggers E03F2.
- **TEST-11**: Compile-fail fixture `tests/compile_fail/v3_pure_io.iron` — pure method calling `println` triggers E03F5 (or equivalent I/O-forbidden diagnostic).
- **TEST-12**: Compile-fail fixture `tests/compile_fail/v3_patch_conflict.iron` — two patches of same type declaring same method triggers E03XX.
- **TEST-13**: Compile-fail fixture `tests/compile_fail/v3_patch_adds_field.iron` — patch containing `var field: T` triggers E03X1.
- **TEST-14**: Compile-fail fixture `tests/compile_fail/v3_receiver_method_removed.iron` — pre-v3 `func (recv: T) name()` triggers E0101 with codemod hint.
- **TEST-15**: Compile-fail fixture `tests/compile_fail/v3_pub_var_lookup.iron` — variations covering synthesized accessor name collisions, inline defaults, etc.

### DOCS — Documentation

- **DOCS-01**: `docs/release-notes/v3.md` — long-form release body covering break rationale, migration path, every new feature with code examples.
- **DOCS-02**: `CHANGELOG.md` — v3.0.0-alpha entry in Keep-a-Changelog shape.
- **DOCS-03**: `docs/site/` updates — landing page v3 banner, guide rewrites for methods/init/patch, API reference refresh.
- **DOCS-04**: `docs/site/migration-v2-to-v3.md` — step-by-step migration guide with codemod usage.
- **DOCS-05**: Iron language spec document updated to reflect v3 grammar.

## v3.1+ Requirements (Deferred)

Acknowledged but not in v3.0 scope:

- **CUSTOM-01**: Custom getter/setter logic (`pub var x: Int { get { ... } set(v) { ... } }`).
- **INIT-DELEGATE-01**: Init-to-init delegation (`self.init(...)`).
- **OP-OVERLOAD-01**: Operator overloading for arithmetic / comparison (explicitly rejected for v3; reconsider).
- **DEFAULT-ARGS-01**: Default parameter values (explicitly rejected for v3; reconsider if ergonomic complaints warrant).
- **PURE-STRICT-01**: Strict `pure` variant forbidding heap allocation (for real-time / embedded contexts).
- **PATCH-SCOPED-01**: File-scoped or import-scoped patching (current choice: program-wide only).
- **GEN-RECV-01**: Generic receivers `patch object List[T] { func push(item: T) }` — deferred from v2.3 backlog.

## Success Criteria

- [ ] `./tests/run_tests.sh integration` — full v3 test matrix passes
- [ ] `./tests/run_tests.sh manual` — manual test suite passes unchanged after migration
- [ ] `./scripts/test-raylib-integration.sh` — raylib examples compile and run after migration
- [ ] `./scripts/verify-v3-migration.sh` — zero-diff generated C across stdlib + examples (IDENT-02)
- [ ] `ironc migrate --from v2 --to v3` produces byte-identical output on re-run (idempotence)
- [ ] All compile_fail fixtures TEST-07..TEST-15 match locked `.expected` via `grep -qF`
- [ ] `examples/pong/pong.iron` and `examples/raylib_showcase/raylib_showcase.iron` build and run post-migration
- [ ] Documentation site rebuilds cleanly with v3 guide and API references
- [ ] CHANGELOG and release notes reviewed before tagging v3.0.0-alpha

## Traceability

Phase mappings assigned by roadmapper on 2026-04-20. Every v3.0 requirement maps to exactly one phase (87 total, 100% coverage).

| Requirement | Phase | Status |
|-------------|-------|--------|
| BREAK-01 | 88 | Complete |
| BREAK-02 | 88 | Complete |
| BREAK-03 | 85 | Complete |
| BREAK-04 | 88 | Complete |
| BREAK-05 | 88 | Complete |
| GRAMMAR-01 | 82 | Complete |
| GRAMMAR-02 | 82 | Complete |
| GRAMMAR-03 | 82 | Complete |
| GRAMMAR-04 | 82 | Complete |
| GRAMMAR-05 | 82 | Complete |
| ACCESS-01 | 83 | Complete |
| ACCESS-02 | 83 | Complete |
| ACCESS-03 | 83 | Complete |
| ACCESS-04 | 83 | Complete |
| ACCESS-05 | 83 | Complete |
| ACCESS-06 | 83 | Complete |
| ACCESS-07 | 83 | Complete |
| ACCESS-08 | 83 | Complete |
| MUTTIER-01 | 84 | Complete |
| MUTTIER-02 | 84 | Complete |
| MUTTIER-03 | 84 | Complete |
| MUTTIER-04 | 84 | Complete |
| MUTTIER-05 | 84 | Complete |
| MUTTIER-06 | 84 | Complete |
| MUTTIER-07 | 84 | Complete |
| INIT-01 | 85 | Complete |
| INIT-02 | 85 | Complete |
| INIT-03 | 85 | Complete |
| INIT-04 | 85 | Complete |
| INIT-05 | 85 | Complete |
| INIT-06 | 85 | Complete |
| INIT-07 | 85 | Complete |
| INIT-08 | 85 | Complete |
| INIT-09 | 85 | Complete |
| INIT-10 | 85 | Complete |
| INIT-11 | 85 | Complete |
| INIT-12 | 85 | Complete |
| INIT-13 | 85 | Complete |
| INIT-14 | 85 | Complete |
| INIT-15 | 85 | Complete |
| INIT-16 | 85 | Complete |
| PATCH-01 | 86 | Complete |
| PATCH-02 | 86 | Complete |
| PATCH-03 | 86 | Complete |
| PATCH-04 | 86 | Complete |
| PATCH-05 | 86 | Complete |
| PATCH-06 | 86 | Complete |
| PATCH-07 | 86 | Complete |
| PATCH-08 | 86 | Complete |
| PATCH-09 | 86 | Complete |
| IFACE-01 | 87 | Complete |
| IFACE-02 | 87 | Complete |
| IFACE-03 | 87 | Complete |
| IFACE-04 | 87 | Complete |
| IFACE-05 | 87 | Complete |
| SELF-01 | 87 | Complete |
| SELF-02 | 87 | Complete |
| SELF-03 | 87 | Complete |
| MIGR-01 | 89 | Complete |
| MIGR-02 | 89 | Complete |
| MIGR-03 | 89 | Complete |
| MIGR-04 | 89 | Complete |
| MIGR-05 | 88 | Complete |
| MIGR-06 | 91 | Pending |
| IDENT-01 | 90 | Pending |
| IDENT-02 | 90 | Pending |
| IDENT-03 | 90 | Pending |
| TEST-01 | 82 | Complete |
| TEST-02 | 86 | Complete |
| TEST-03 | 85 | Complete |
| TEST-04 | 83 | Complete |
| TEST-05 | 84 | Complete |
| TEST-06 | 84 | Complete |
| TEST-07 | 85 | Complete |
| TEST-08 | 85 | Complete |
| TEST-09 | 84 | Complete |
| TEST-10 | 84 | Complete |
| TEST-11 | 84 | Complete |
| TEST-12 | 86 | Complete |
| TEST-13 | 86 | Complete |
| TEST-14 | 88 | Complete |
| TEST-15 | 85 | Complete |
| DOCS-01 | 91 | Pending |
| DOCS-02 | 91 | Pending |
| DOCS-03 | 91 | Pending |
| DOCS-04 | 91 | Pending |
| DOCS-05 | 91 | Pending |
