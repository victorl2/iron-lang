# Roadmap: Iron v3.0 — Method Ergonomics

**Created:** 2026-04-20
**Granularity:** standard
**Total phases:** 10 (82 through 91)
**Total requirements:** 87 (100% mapped)

v3.0 is Iron's first non-superset release. Receiver-method syntax is removed; methods move inside `object T { ... }` blocks with implicit `self`. The milestone lands grammar first, then typechecker enforcement, then open extension, then the hard break, then stdlib migration via codemod, then byte-identity verification, then docs. Phase order is load-bearing: stdlib cannot compile on an in-progress compiler, so every pre-MIGR phase preserves a compilable state against the pre-v3 grammar (superset-within-milestone held until Phase 88).

## Phases

- [x] **Phase 82: GRAMMAR** - Methods-in-object-block parsing, implicit self, required self.field
- [x] **Phase 83: ACCESS** - `pub` keyword, default-private, synthesized accessors for `pub val`/`pub var` (completed 2026-04-21)
- [x] **Phase 84: MUTTIER** - `readonly` / `pure` tier enforcement with strict transitive rules (completed 2026-04-21)
- [x] **Phase 85: INIT** - Mandatory init, anonymous + named forms, definite-assignment analysis (completed 2026-04-21)
- [x] **Phase 86: PATCH** - `patch object T` grammar + program-wide dispatch + retroactive interface conformance (completed 2026-04-21)
- [x] **Phase 87: IFACE + SELF** - Interface `readonly`/`pure` modifiers, default implementations, `Self` return type (completed 2026-04-23)
- [x] **Phase 88: BREAK** - Hard rejection of v2 receiver-method syntax with migration hints (completed 2026-04-23)
- [x] **Phase 89: MIGR** - Codemod implementation, atomic stdlib + examples + tests migration (completed 2026-04-23)
- [ ] **Phase 90: IDENT** - Golden-output byte-identity verification, `scripts/verify-v3-migration.sh` release blocker
- [ ] **Phase 91: DOCS** - Release notes, site guide, migration guide, language spec refresh

## Phase Details

### Phase 82: GRAMMAR - Methods-in-object-block

**Goal:** Parser accepts method declarations inside `object X { ... }` blocks with implicit `self` and required `self.field` prefix, preserving the existing method-call dispatch path in typechecker and HIR lowering.

**Depends on:** Nothing (first v3 phase; builds on v2.2 parser)

**Requirements:** GRAMMAR-01, GRAMMAR-02, GRAMMAR-03, GRAMMAR-04, GRAMMAR-05, TEST-01

**Success Criteria** (what must be TRUE):
1. A user can declare `object Player { func take_damage(n: Int) { self.health -= n } }` and the file parses successfully.
2. Inside a method body, writing a bare identifier matching a field name resolves to the innermost binding (param/local), never the field - accessing the field requires `self.field`.
3. A call `player.take_damage(5)` dispatches to the in-block method via the existing method-call resolver.
4. `tests/integration/v3_methods_in_block.iron` compiles and runs with expected output.
5. The `object` keyword is retained unchanged; no rename to `definition` / `struct` / etc.

**Plans:** 2/2 plans complete

Plans:
- [x] 82-01-PLAN.md — Parser extension: in-block `func` branch in iron_parse_object_decl + `self` keyword-reservation unit tests (GRAMMAR-01..05)
- [x] 82-02-PLAN.md — v3_methods_in_block.iron fixture + pure-superset guard verification (TEST-01)

---

### Phase 83: ACCESS - Visibility (single `pub` keyword)

**Goal:** Declarations default to private across the codebase; `pub` opts into external visibility; `pub val field`/`pub var field` synthesize accessor methods that dispatch under property syntax `obj.field` and `obj.field = v`.

**Depends on:** Phase 82

**Requirements:** ACCESS-01, ACCESS-02, ACCESS-03, ACCESS-04, ACCESS-05, ACCESS-06, ACCESS-07, ACCESS-08, TEST-04

**Success Criteria**:
1. A non-`pub` field or method declared in module A is not visible from module B; a `pub` field or method is visible.
2. Declaring `pub var health: Int` synthesizes both a `pub readonly func health() -> Int` accessor and a `pub func set_health(v: Int)` mutator.
3. Reading `player.health` and writing `player.health = 5` dispatch to the synthesized accessors - property syntax works on `pub` fields only.
4. Declaring `pub val x: T` alongside `func x() -> T` produces `E03XE: name 'x' reserved by synthesized accessor`.
5. Neither `priv` nor `private` parse as keywords - private is the implicit default.

**Plans:** 3/3 plans complete

Plans:
- [x] 83-01-PLAN.md — Lexer `pub` keyword + parser `pub` modifier threading + AST fields (ACCESS-01, ACCESS-02, ACCESS-07)
- [x] 83-02-PLAN.md — Accessor synthesis (getter + setter) + property-syntax dispatch + name-collision diagnostic (ACCESS-03, ACCESS-04, ACCESS-05, ACCESS-06, ACCESS-08)
- [x] 83-03-PLAN.md — v3_pub_field_synthesis fixture + compile_fail + pure-superset guard (TEST-04)

---

### Phase 84: MUTTIER - Mutation tiers

**Goal:** Typechecker enforces three mutation tiers. Default methods mutate freely; `readonly` methods cannot write `self.field` and cannot transitively call mutating methods on `self`'s fields; `pure` methods add no-I/O, no-global-write, no-non-pure-calls on top of readonly, while still allowing heap allocation.

**Depends on:** Phase 83

**Requirements:** MUTTIER-01, MUTTIER-02, MUTTIER-03, MUTTIER-04, MUTTIER-05, MUTTIER-06, MUTTIER-07, TEST-05, TEST-06, TEST-09, TEST-10, TEST-11

**Success Criteria**:
1. A `readonly func` writing `self.field` is rejected with `E03F1`; calling a mutating method from `readonly` context is rejected with `E03F2`.
2. A `pure func` calling `println` is rejected with `E03F5` (or the equivalent I/O-forbidden diagnostic); a `pure func` allocating on the heap compiles cleanly.
3. Calling a mutating method through a `val`-bound receiver produces `E0235: cannot call mutating method on immutable binding`; the same call through a `var`-bound receiver succeeds.
4. A `readonly` method calling only `readonly` or `pure` methods on self's fields compiles; transitively reaching a mutating method is rejected.
5. `init` declared as `readonly` or `pure` is a compile error - inits write fields by definition.

**Plans:** 3/3 plans complete

Plans:
- [x] 84-01-PLAN.md — Lexer readonly/pure keywords + parser modifier threading + AST is_readonly/is_pure flags + synth accessor retrofit (MUTTIER-01, MUTTIER-07)
- [x] 84-02-PLAN.md — Typechecker tier enforcement (E0238..E0244) + TypeCtx flag tracking + transitive rules (MUTTIER-02..06)
- [x] 84-03-PLAN.md — TEST-05/06/09/10/11 fixtures + Phase 84 close-out pure-superset guard

---

### Phase 85: INIT - Mandatory construction

**Goal:** Every object has at least one `init`. Inline field defaults are rejected. Inits definitely-assign every field on every exit path, support anonymous `Type(args)` and named `Type.name(args)` forms, and field-less objects receive a synthesized empty init automatically.

**Depends on:** Phase 84

**Requirements:** INIT-01, INIT-03, INIT-04, INIT-05, INIT-06, INIT-07, INIT-08, INIT-09, INIT-10, INIT-11, INIT-12, INIT-13, INIT-14, INIT-15, INIT-16, TEST-03, TEST-08, TEST-15 (INIT-02 + BREAK-03 + TEST-07 deferred to Phase 88 BREAK per 85-CONTEXT lock to preserve pure-superset through Phase 87)

**Success Criteria**:
1. Declaring an object with fields but no init produces `E03XA: object has no init; cannot be constructed`; a field-less object `object Marker {}` compiles and is constructed via `Marker()`.
2. An init leaving a field unassigned on any exit path is rejected with `E03XZ`; reading a field before it is assigned in that init is rejected with `E03XY`.
3. An object with an anonymous `init(...)` plus `init from_string(s)` can be constructed as both `Type(args)` and `Type.from_string(s)`; duplicate anonymous inits produce `E03XB`.
4. Inline defaults `var x: Int = 0` inside object blocks produce `E03XC: inline default forbidden; assign in init`.
5. A `val` field assigned twice in a single init produces `E03YY`; method calls on `self` before all fields are assigned produce `E03YX`.

**Plans:** 3/3 plans complete

Plans:
- [ ] 85-01-PLAN.md — Lexer init keyword + parser init-decl (anon + named) + AST is_init/init_name + field-less auto-synth + interface-body rejection + E0246..E0252 constants (INIT-01, INIT-03, INIT-07, INIT-08, INIT-11, INIT-13, INIT-15)
- [ ] 85-02-PLAN.md — Typechecker definite-assignment analysis + delegation/return-value rejection + call-site dispatch for Type(args) / Type.name(args) (INIT-04, INIT-05, INIT-06, INIT-09, INIT-10, INIT-11, INIT-12, INIT-14, INIT-16)
- [x] 85-03-PLAN.md — TEST-03 + INIT-13 positive fixtures + TEST-08 + TEST-15 + readonly-init compile_fail fixtures + Phase 85 close-out (TEST-03, TEST-08, TEST-15) (completed 2026-04-21)

---

### Phase 86: PATCH - Open extension

**Goal:** `patch object T { ... }` syntax parses, applies program-wide, and lets authors add methods and inits to user-defined types and primitives (`Int`, `Int32`, `Float`, `String`, `Bool`). Duplicate signatures across patches are compile errors; patches cannot add fields or change visibility; patched inits follow the same definite-assignment rules.

**Depends on:** Phase 85

**Requirements:** PATCH-01, PATCH-02, PATCH-03, PATCH-04, PATCH-05, PATCH-06, PATCH-07, PATCH-09, TEST-02, TEST-12, TEST-13 (PATCH-08 retroactive interface conformance deferred to Phase 87 IFACE+SELF - depends on interface registry that lands with IFACE-01..05)

**Success Criteria**:
1. `patch object Int { pub readonly func double() -> Int { return self * 2 } }` compiles; calling `5.double()` returns `10`.
2. Two patches of the same type declaring the same method signature produce `E03XX: conflicting patch definitions`; there is no silent override.
3. A patch containing a field declaration is rejected with `E03X1: patches may only add methods and inits`; re-declaring an existing member produces `E03X2`.
4. `patch object Int implements Comparable { ... }` extends `Int` with the interface methods and makes `Int` satisfy `Comparable` from that point forward. (Deferred to Phase 87 - PATCH-08.)
5. A patched init that fails to assign every field on some exit path is rejected by the same definite-assignment analysis as in-object inits.

**Plans:** 3/3 plans complete

Plans:
- [x] 86-01-PLAN.md - Lexer patch keyword + parser top-level patch-object-decl + AST is_patch/target_type_name + E0253/254/255 reservations (PATCH-01, PATCH-02, PATCH-05) (completed 2026-04-21)
- [x] 86-02-PLAN.md - Resolver type-patch registry + typechecker collision scan + call-site dispatch extension + PATCH-07/09 regression locks (PATCH-02, PATCH-03, PATCH-04, PATCH-06, PATCH-07, PATCH-09) (completed 2026-04-21)
- [x] 86-03-PLAN.md - TEST-02 positive integration + TEST-12/TEST-13 compile_fail + Phase 86 close-out pure-superset guard (TEST-02, TEST-12, TEST-13) (completed 2026-04-21)

---

### Phase 87: IFACE + SELF - Interface integration and `Self` type

**Goal:** Interface method signatures accept `readonly` and `pure` modifiers with implementation-must-match-or-strengthen rules; interfaces support default method implementations; `Self` is legal in return position inside methods, in interface signatures, and as a type-constructor expression for `Self(args)` / `Self.name(args)`.

**Depends on:** Phase 86

**Requirements:** IFACE-01, IFACE-02, IFACE-03, IFACE-04, IFACE-05, SELF-01, SELF-02, SELF-03, PATCH-08 (inherited from Phase 86 per 86-CONTEXT lock — retroactive conformance requires the interface registry that lands in Phase 87)

**Success Criteria**:
1. An interface declaring `readonly func foo() -> Int` accepts implementers that are `readonly` or `pure` but rejects mutating implementations; `pure` interface methods accept only `pure` implementations.
2. An interface method with a default implementation is usable by implementers who do not override it; overriding implementers win over the default.
3. An interface declaring an `init` or a field is rejected - interfaces declare methods only.
4. A method with return type `Self` on object `Player` returns `Player` when called on a `Player` instance; the same interface method bound to `Enemy` returns `Enemy`.
5. `return Self(args)` inside a method constructs the enclosing type via its anonymous init; `Self.named(args)` uses the named init.

**Plans:** 3/3 plans complete

Plans:
- [x] 87-01-PLAN.md — Interface body tier modifiers + default implementations + IFACE-04 init rejection + E0256/E0257 (IFACE-01, IFACE-02, IFACE-03, IFACE-04)
- [x] 87-02-PLAN.md — Self return type + Self(args)/Self.name(args) expressions + patch implements clause + retroactive conformance (IFACE-05, SELF-01, SELF-02, SELF-03, PATCH-08)
- [x] 87-03-PLAN.md — 3 positive integration fixtures + 4 compile_fail fixtures + Phase 87 close-out pure-superset guard (lock all 9 Phase 87 requirement IDs end-to-end)

---

### Phase 88: BREAK - Hard rejection of v2 grammar

**Goal:** Parser rejects receiver-method syntax (`func (recv: T) name()`), mut-receiver syntax (`func (mut recv: T) name()`), and the `mut` keyword in any position, with diagnostics that point users to the codemod. This phase ends the pure-superset guard that held from v2.0 through Phase 87.

**Depends on:** Phase 87

**Requirements:** BREAK-01, BREAK-02, BREAK-04, BREAK-05, MIGR-05, TEST-14

**Success Criteria**:
1. A file containing `func (p: Player) take_damage(n: Int)` is rejected with `E0101: receiver-method syntax removed in v3.0` and the diagnostic includes `help: run ironc migrate --from v2 --to v3 <file>`.
2. A file containing `func (mut p: Player) heal(n: Int)` produces the same `E0101` diagnostic; mut-receivers migrate to default-mutating in-block methods.
3. The `mut` keyword is rejected in param position, local position, and receiver binding position with a consistent migration-hint diagnostic.
4. `tests/compile_fail/v3_receiver_method_removed.iron` locks the exact diagnostic text including the codemod hint.
5. Release notes explicitly name v3.0 as the first Iron release without the pure-superset guard.

**Plans:** 3/3 plans complete

Plans:
- [x] 88-01-PLAN.md — E0260..E0264 error codes + Iron_Parser v3_strict_mode + 5 rejection sites + --strict-v3 CLI flag
- [x] 88-02-PLAN.md — TEST-14 compile_fail fixture + BREAK-03/04 fixtures + gate-OFF regression
- [x] 88-03-PLAN.md — Phase 88 close-out + server test evidence + SUMMARY

---

### Phase 89: MIGR - Codemod + atomic stdlib migration

**Goal:** Ship `ironc migrate --from v2 --to v3 <path>` as a deterministic, idempotent transform covering receiver-methods to in-block or patch form, mut-receivers to default in-block, inline defaults to init bodies, and synthesized-init insertion. In a single atomic commit set, migrate all in-tree `.iron` files (stdlib, examples, tests, docs) so the tree compiles on v3.

**Depends on:** Phase 88

**Requirements:** MIGR-01, MIGR-02, MIGR-03, MIGR-04

**Success Criteria**:
1. `ironc migrate --from v2 --to v3 <file>` on any v2 source produces a v3-grammar rewrite; running it a second time on the output produces byte-identical output (idempotence).
2. After the atomic migration commit, `./tests/run_tests.sh integration` and `./tests/run_tests.sh manual` pass without regression; `./scripts/test-raylib-integration.sh` passes natively.
3. `src/stdlib/`, `examples/`, `tests/integration/`, and `docs/` all round-trip through the codemod as a single coordinated commit - no partial state where some files use v2 grammar and others use v3.
4. `ironc migrate` is invocable from the v3.0.0-alpha release binary (not a separate tool).
5. The codemod emits a unified diff on stderr, so users can review changes before accepting.

**Plans:** 3/3 plans complete

Plans:
- [x] 89-01-PLAN.md — Python codemod script (scripts/migrate_v2_to_v3.py) + ironc migrate subcommand in main.c (MIGR-02, MIGR-03, MIGR-04)
- [x] 89-02-PLAN.md — Apply codemod to src/stdlib/*.iron + delete 6 obsolete v2-syntax fixtures + strict-v3 verify per file (MIGR-01, MIGR-03)
- [x] 89-03-PLAN.md — Flip v3_strict_mode default to true + server unit+integration verify + Phase 89 close-out SUMMARY (MIGR-01, MIGR-02, MIGR-04)

---

### Phase 90: IDENT - Generated-C byte identity

**Goal:** Lock the guarantee that codemod output compiled with v3 produces byte-identical generated C to the pre-migration v2.2 compilation for every stdlib module, example, and integration test. Ship `scripts/verify-v3-migration.sh` as a release blocker that fails if any diff appears.

**Depends on:** Phase 89

**Requirements:** IDENT-01, IDENT-02, IDENT-03

**Success Criteria**:
1. `scripts/verify-v3-migration.sh` runs codemod then v3 compile then diff against a locked v2.2 golden-C snapshot across `src/stdlib/`, `examples/`, and `tests/integration/`, and exits non-zero on any diff.
2. `tests/migrate_identity/*.iron` fixtures lock per-stdlib-module byte-identity at CI granularity; each generated `.c` file matches the v2.2 golden exactly.
3. In-block default-mutating methods lower to `Iron_Type_method(Iron_Type *self, ...)` (pointer receiver); `readonly` and `pure` methods lower to by-value receivers - matching v2.2 ABI with zero refactors.
4. The script is wired into the release checklist (not just CI) so v3.0.0-alpha cannot be tagged while any diff exists.
5. Re-running the codemod on already-migrated source produces zero changes and zero diff in downstream C output.

**Plans:** 1/2 plans executed

Plans:
- [ ] 90-01-PLAN.md — Verify script + tests/migrate_identity/ fixture infrastructure (IDENT-01, IDENT-02)
- [ ] 90-02-PLAN.md — Generate goldens + verify end-to-end + architectural guarantee SUMMARY (IDENT-01, IDENT-02, IDENT-03)

---

### Phase 91: DOCS - Release documentation

**Goal:** Author the long-form v3.0.0-alpha release notes, CHANGELOG entry, migration guide, guide/API-reference rewrites, and language spec refresh. Land release artifacts needed for the v3.0.0-alpha tag.

**Depends on:** Phase 90

**Requirements:** DOCS-01, DOCS-02, DOCS-03, DOCS-04, DOCS-05, MIGR-06

**Success Criteria**:
1. `docs/release-notes/v3.md` covers the break rationale, lists every new feature with code examples, and includes the migration path.
2. `CHANGELOG.md` has a v3.0.0-alpha entry in Keep-a-Changelog format with Added / Changed / Removed sections.
3. `docs/site/migration-v2-to-v3.md` walks a user through codemod usage step-by-step with before/after code samples.
4. `docs/site/` landing page carries the v3 banner; the guide's methods / init / patch pages are rewritten for v3 grammar; the API reference reflects synthesized accessors and patch extensions.
5. The Iron language spec document is updated to describe v3 grammar, mutation tiers, init rules, and patch semantics - replacing the v2.2 receiver-method sections.

**Plans:** TBD

## Progress Table

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 82. GRAMMAR | 1/2 | Complete    | 2026-04-21 |
| 83. ACCESS | 3/3 | Complete    | 2026-04-21 |
| 84. MUTTIER | 3/3 | Complete    | 2026-04-21 |
| 85. INIT | 3/3 | Complete    | 2026-04-21 |
| 86. PATCH | 3/3 | Complete    | 2026-04-21 |
| 87. IFACE + SELF | 3/3 | Complete    | 2026-04-23 |
| 88. BREAK | 3/3 | Complete    | 2026-04-23 |
| 89. MIGR | 3/3 | Complete    | 2026-04-23 |
| 90. IDENT | 1/2 | In Progress|  |
| 91. DOCS | 0/0 | Not started | - |

## Coverage Summary

- **Total v3 requirements:** 87
- **Mapped:** 87
- **Orphaned:** 0
- **Duplicated:** 0

All BREAK / GRAMMAR / ACCESS / MUTTIER / INIT / PATCH / IFACE / SELF / MIGR / IDENT / TEST / DOCS requirements are assigned to exactly one phase. TEST-01..TEST-15 are distributed into the phase that introduces the feature each test locks. BREAK-03 (inline-default rejection) lives in Phase 85 (INIT) rather than Phase 88 (BREAK) because it is inseparable from definite-assignment and lands with the init work. MIGR-05 (migration-hint diagnostics) lives in Phase 88 alongside the v2-syntax rejections it annotates; MIGR-06 (breaking-change messaging) lives in Phase 91 alongside the release notes that carry it.

---
*Generated by roadmapper 2026-04-20. Starts at Phase 82, continuing from v2.2's Phase 81.*
