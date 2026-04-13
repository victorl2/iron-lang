# Requirements: Iron v0.1.4-alpha Compiler Correctness & Maintenance

**Defined:** 2026-04-11
**Core Value:** Iron's compiler and runtime are correct, cross-platform, and protected from repeat regressions — measured by eliminated UB classes, restored CI coverage, and a ratcheting test/coverage baseline.

> **Motivating incident.** Phase 59 execution surfaced a latent SIGSEGV in `src/hir/hir_to_lir.c :: collect_mono_enums_node` that had been living in the tree since Phase 37-02 (commit `9e259435`, 2026-04-04). The function used **local struct typedefs guessing the layout** of `Iron_IfStmt` — the guess was missing the `elif_conds[]`/`elif_bodies[]` fields between `body` and `else_body`, so it read the `else_body` pointer at the `elif_conds` offset and recursed into an stb_ds array header as if it were an `Iron_Node*`. Benign on macOS arm64; deterministic SIGSEGV on Linux x86_64 when compiling any program with an `if/elif/else` chain. The bug was invisible to the CI pipeline because the only Linux Release path (the Benchmark workflow) reported compile crashes as "benchmark compilation failed" and swallowed the signal. This milestone is the response: systematic audit + structural protections + regression nets so the same class of bug cannot re-land undetected.

> **Paused milestone.** v0.2.0-alpha Networking Standard Library is paused at 25/89 requirements shipped (Phase 59 delivered INFRA-04..10 partial, NET-01..13, URL-01..07, cut as public release v1.2.0-alpha via PR #17). The 64 remaining v0.2.0-alpha requirements are archived at `.planning/REQUIREMENTS-v0.2.0.md`. Phases 60–64 (HTTP/TLS/JSON/WebSocket) stay drafted in ROADMAP.md and resume after v0.1.4-alpha ships.

## v0.1.4-alpha Requirements

35 requirements across 7 categories. Each maps to exactly one roadmap phase.

### Audit & Discovery

Comprehensive correctness audit across compiler + runtime + stdlib C code. Output is `.planning/research/CORRECTNESS-AUDIT.md`: a ranked issue list with severity (high / medium / low), affected file:line, suggested fix, and target regression fixture name.

- [x] **AUDIT-01**: Every compiler pass (parser, resolver, typechecker, escape, capture, init_check, concurrency, iface_collect, HIR lowering, HIR-to-LIR, LIR optimization passes, emit_c and sub-modules) systematically read for blind casts — any `(Iron_XXX *)node` / `(IronHIR_XXX *)expr` / `(IronLIR_XXX *)instr` cast without a preceding `kind`/`op` check is documented with file:line and severity
- [x] **AUDIT-02**: Every `switch` over an Iron enum (`Iron_NodeKind`, `IronHIR_ExprKind`, `IronHIR_StmtKind`, `IronLIR_OpKind`, `Iron_TypeKind`, `Iron_OpKind`, `Iron_DiagCode`) audited for exhaustiveness — silent `default:` fall-throughs are documented with the specific enum values they miss
- [x] **AUDIT-03**: Null-pointer safety audit — every pointer deref in compiler / runtime / stdlib C code catalogued as "provably non-null by construction" or "unguarded; risk level H/M/L"
- [x] **AUDIT-04**: Arena lifetime audit — any cross-arena pointer storage (a long-lived arena holding a pointer from a short-lived arena, or a value stored by reference into stb_ds arrays that outlive the source arena) is documented with file:line and an ownership-transfer recommendation
- [x] **AUDIT-05**: Integer safety audit — `size_t` / `int` / `int64_t` mixing in size calculations, stb_ds `arrlen` casts, array indexing in hot paths, shift counts, signed-vs-unsigned comparisons; all flagged with severity
- [x] **AUDIT-06**: System-call and allocation error handling audit — every `malloc`/`calloc`/`realloc`/`strdup`, every libc/POSIX I/O syscall, every `pthread_*`/`iron_mutex_*`, every `posix_spawn`/`CreateProcess`/`fork`/`exec` — return value check status documented with user-visible consequences of failure
- [x] **AUDIT-07**: Runtime + stdlib C code audit — same six dimensions applied to `src/runtime/*.c` (`iron_string.c`, `iron_threads.c`, `iron_net_init.c`, `iron_collections.c`, `iron_builtins.c`, `iron_rc.c`) and `src/stdlib/*.c` (`iron_net.c`, `iron_math.c`, `iron_io.c`, `iron_time.c`, `iron_log.c`, `iron_hint.c`)
- [x] **AUDIT-08**: Cross-platform correctness scan — flag all code that assumes 64-bit pointers, little-endian byte order, POSIX-only headers, GCC-only attributes, specific struct padding rules, or ASLR-insensitive heap layouts; logged to `.planning/research/CROSS-PLATFORM-DEBT.md` for a future Windows-compat milestone (only trivial fixes apply here)
- [x] **AUDIT-09**: `CORRECTNESS-AUDIT.md` produced with one table per audit dimension, each row containing `file:line`, severity, description, suggested fix, target regression fixture name; the top-20 high-severity issues flagged as "must-fix in this milestone"

### Structural Protections

Changes that make the blind-cast bug class structurally impossible to reintroduce.

- [x] **PROT-01**: `Iron_ExprNode` moved from `src/hir/hir_lower.c:101` into `src/parser/ast.h` as a blessed prefix typedef next to `Iron_Node`, with compile-time `_Static_assert`s enforcing the `{Iron_Span span; Iron_NodeKind kind; struct Iron_Type *resolved_type;}` prefix on every expression AST type — `Iron_IntLit`, `Iron_FloatLit`, `Iron_StringLit`, `Iron_BoolLit`, `Iron_NullLit`, `Iron_Ident`, `Iron_BinaryExpr`, `Iron_UnaryExpr`, `Iron_CallExpr`, `Iron_MethodCallExpr`, `Iron_FieldAccess`, `Iron_Index`, `Iron_HeapExpr`, `Iron_ConstructExpr`, `Iron_ArrayLit`, `Iron_EnumConstruct`, `Iron_Closure`, and any others that `expr_type()` might see
- [x] **PROT-02**: `-Werror=switch-enum` enabled on `iron_compiler`, `iron_runtime`, `iron_stdlib`, and `ironc` targets in `CMakeLists.txt`; every existing `switch` over an `Iron_*Kind` / `IronHIR_*Kind` / `IronLIR_OpKind` enum either handles every case explicitly or opts out with a `default: break;` that has a comment explaining why the unhandled kinds are genuinely don't-care
- [x] **PROT-03**: Debug-build `iron_node_assert_kind(node, expected_kind, __FILE__, __LINE__)` helper added in `src/parser/ast.h` and called at the top of every AST-node cast site, gated by `#ifndef NDEBUG` so Release builds pay zero cost; catches wrong-kind casts at runtime in CI even when compile-time guards miss one
- [x] **PROT-04**: All high-severity blind-cast sites from AUDIT-01 are rewritten to use the real struct from `parser/ast.h` (not a local shadow) with a preceding kind-check; any fix that touches a walker function adds a minimal regression fixture in `tests/integration/` following the `hir_to_lir_elif_mono_walker.iron` reference pattern

### Correctness Fixes

Concrete fixes for known and audit-discovered high-severity issues.

- [ ] **FIX-01**: All top-20 high-severity issues from `CORRECTNESS-AUDIT.md` are resolved — each fix lands with a regression fixture (integration test, unit test, or C-level assertion) that would have failed before the fix and passes after
- [ ] **FIX-02**: Every unchecked `malloc`/`calloc`/`realloc`/`strdup` call that could produce a user-visible crash is either fixed with a guarded error path (returning a typed error) or explicitly annotated `/* SAFETY: cannot fail because ... */` with the invariant documented at the call site
- [ ] **FIX-03**: All cross-arena pointer storage issues flagged by AUDIT-04 are resolved by either migrating ownership to the correct arena or duplicating the value into the long-lived arena with `iron_arena_strdup` / `iron_arena_alloc` + `memcpy`
- [ ] **FIX-04**: All silent `default:` fall-throughs in Iron enum switches flagged by AUDIT-02 are replaced with either explicit `case` entries (when the intent is "handle this kind") or a commented `default:` that explains why the fall-through is intentional

### Fuzzing Infrastructure

Standing fuzzer targets in CI. One-time setup cost; catches new bug classes forever.

- [ ] **FUZZ-01**: libFuzzer target `tests/fuzz/fuzz_parser.c` drives `iron_parse()` on random byte inputs, asserts on crash or hang (>10s timeout), and minimizes reproducible crashes into `tests/fuzz/corpus/parser/`
- [ ] **FUZZ-02**: libFuzzer target `tests/fuzz/fuzz_typecheck.c` drives `iron_typecheck()` on randomly-generated well-formed-parsing Iron programs (generator seeded from existing `tests/integration/` fixtures)
- [ ] **FUZZ-03**: libFuzzer target `tests/fuzz/fuzz_hir_to_lir.c` drives `iron_hir_to_lir()` on randomly-generated HIR modules — this is the target that would have caught `collect_mono_enums_node`
- [ ] **FUZZ-04**: Seed corpora for each fuzz target initialised from the `tests/integration/*.iron` fixtures so fuzzing starts with already-exercised code paths instead of random bytes
- [ ] **FUZZ-05**: Scheduled nightly CI job `.github/workflows/fuzz.yml` runs each libFuzzer target against `main` for 10 minutes with a deterministic seed, uploads any new crashes as artifacts, and opens a GitHub issue tagged `fuzz-crash` when a regression is found
- [ ] **FUZZ-06**: Crash-to-fixture harness — when a fuzzer finds a crash, `scripts/fuzz_crash_to_fixture.sh` auto-generates a minimal integration fixture at `tests/integration/fuzz_crash_NNN.iron` with a `.expected` file derived from the crash context, so every fuzz-found crash accumulates as a permanent regression test

### Coverage Tooling

Track compiler / runtime coverage over time; identify untested code paths.

- [ ] **COV-01**: CMake option `IRON_ENABLE_COVERAGE=ON` adds `-fprofile-instr-generate -fcoverage-mapping` to `iron_compiler`, `iron_runtime`, `iron_stdlib`, and `ironc` targets (clang-only) and makes `llvm-profdata` and `llvm-cov` available to subsequent tooling steps
- [ ] **COV-02**: `scripts/coverage.sh` runs the full ctest suite with coverage instrumentation, merges `.profraw` outputs via `llvm-profdata`, and produces both `build/coverage/index.html` (human-readable report) and `build/coverage/summary.json` (machine-readable per-file line / branch percentages)
- [ ] **COV-03**: CI job `.github/workflows/coverage.yml` runs `scripts/coverage.sh` on push to `main`, uploads the HTML report as an artifact, diffs `summary.json` against the previous main run, and comments the per-file coverage delta on the PR that landed the changes
- [ ] **COV-04**: Baseline coverage report committed as `.planning/research/COVERAGE-BASELINE.md` — per-file line and branch coverage for all files in `src/parser/`, `src/analyzer/`, `src/hir/`, `src/lir/`, `src/runtime/`, `src/stdlib/`, with files below 50% line coverage flagged for targeted test additions
- [ ] **COV-05**: Every file flagged in COV-04 as below 50% receives at least one additional targeted test raising its coverage above 50% (100% is not the goal — 50% is the realistic floor for a maintenance milestone)

### Regression Nets

Changes that would have caught the bugs we just found, and that pay off over time.

- [x] **REG-01**: New CI job `build-and-test-release (ubuntu-latest)` in `.github/workflows/ci.yml` builds `ironc` in `Release` mode and runs the integration test suite against a curated set of "compilation canary" fixtures — this is the missing job that would have caught the `find_first_last` SIGSEGV months earlier (the existing Benchmark workflow runs Release but reports compile crashes as "benchmark failed", swallowing the signal)
- [ ] **REG-02**: Crash-canary fixtures for every AST node kind the HIR-to-LIR walker handles — minimal `.iron` files under `tests/integration/hir_canary_*.iron` covering `if / elif / else`, `match / elif-match`, `while`, `for`, `assign / compound-assign`, nested `if`, `spawn`, `parallel for`, tuple destructure, enum construct, method call, static call (`Type.method`), lambda, closure capture, heap expr — each is a minimal regression test that must compile cleanly in both Debug and Release and must round-trip its expected output
- [ ] **REG-03**: Cross-platform benchmark threshold calibration documented at `docs/benchmark-calibration.md` explaining the macOS-arm64-vs-Linux-x86_64 variance we observed during Phase 59 (sieve_of_eratosthenes 0.73x → 1.70x, merge_k_sorted_lists 0.77x → 1.50x, find_first_last 1.10x → 1.40x, matrix_chain_mult 0.77x → 1.40x); `scripts/bench_audit.sh` extended with a `--platform linux-x86_64` flag that re-audits thresholds from observed data so the next drift is a 5-minute fix instead of a 30-minute debug session
- [x] **REG-04**: `tests/integration/hir_to_lir_elif_mono_walker.iron` (already landed in the Phase 59 fix) kept as the reference pattern for "class of bug" regression fixtures, and its doc comment style (motivating incident + layout diagram + fix summary + severity) adopted as the template for REG-02 crash-canary fixtures and any future AUDIT-derived regression tests

### Version Reconciliation

Housekeeping: the internal planning-doc version scheme and the public GitHub release version scheme have drifted. Pick one canonical scheme, sync everything, and close the gap that allowed the drift to recur.

- [ ] **VER-01**: Canonical scheme decision recorded at `docs/versioning.md` (new file) documenting (a) which scheme is canonical — internal `v0.x.y-alpha` or public `v1.x.y-alpha` — (b) the rationale, (c) how the drift happened (the `v0.1-alpha → v1.0.0-alpha` jump in commit `e8b9f7a` without planning-doc update), (d) the rules for future releases so the drift cannot silently recur
- [ ] **VER-02**: `.planning/PROJECT.md`, `.planning/ROADMAP.md`, `.planning/REQUIREMENTS.md`, and every existing phase folder name in `.planning/phases/` all updated to match the canonical scheme from VER-01; archived requirements files (`REQUIREMENTS-v0.1.0.md`, `REQUIREMENTS-v0.1.x.md`, `REQUIREMENTS-v0.2.0.md`) renamed if the canonical scheme dictates
- [ ] **VER-03**: `.github/workflows/changelog-on-release.yml` + `CMakeLists.txt` `IRON_VERSION_FULL` + `scripts/update_changelog_from_release.py` flow updated so a release tag that diverges from the canonical scheme is rejected at CI time (not silently accepted); PR #16's release-tag-to-CHANGELOG flow is documented with a failure test case

## v2 Requirements (deferred to later milestones)

### Windows Compatibility (deferred — dedicated milestone after v0.1.4-alpha)

- **WIN-01**: `src/util/strbuf.h` `__attribute__((format(...)))` guarded behind `#ifdef __GNUC__` so MSVC accepts the header
- **WIN-02**: `src/diagnostics/diagnostics.c` `<unistd.h>` ifdef-guarded with MSVC alternatives (`_isatty`, `_fileno`)
- **WIN-03**: `src/pkg/*.c` and `src/cli/toml.c` POSIX-deprecation warnings silenced via `_CRT_NONSTDC_NO_DEPRECATE` (complements the `_CRT_SECURE_NO_WARNINGS` already added during Phase 59)
- **WIN-04**: `windows-latest` re-added to the CI matrix as a required check (not `continue-on-error`)

### Networking Stack (paused v0.2.0-alpha — resumes after v0.1.4-alpha)

See `.planning/REQUIREMENTS-v0.2.0.md` for the full list. 64 requirements across HTTP Client (HCLI-01..12), HTTP Server (HSRV-01..13+), TLS Integration, JSON (parse / stringify / codec / schema), WebSocket Client, HTTP Advanced, Net Extensions, WebSocket Advanced, JSON Advanced, URL Advanced. INFRA-01..03 (mbedTLS + cJSON + CA bundle vendoring) also paused.

## Out of Scope

| Feature | Reason |
|---|---|
| Windows CI re-enablement in this milestone | Deferred to its own dedicated milestone — scope is too big to bundle and depends on fixes to `src/util/`, `src/diagnostics/`, `src/pkg/` that warrant their own review cycle |
| HTTP client / HTTP server / TLS / JSON / WebSocket | Paused with v0.2.0-alpha; resumes after v0.1.4-alpha ships |
| Full formal verification of the compiler | Unbounded scope; targeted audit + structural protections + fuzzing is the pragmatic alternative |
| 100% test coverage | Diminishing returns past 50%; the 50% threshold in COV-05 is the realistic maintenance-milestone goal |
| Fuzzer-as-required-check on every PR | libFuzzer runs are slow (10 min × 3 targets) and flaky on shared CI; nightly on `main` is the pragmatic tradeoff for this milestone. Per-PR gating is a future call |
| Rewriting `src/cli/toml.c` to use a different TOML parser | Vendored code with its own upstream; out of scope for correctness audit |
| Changes to Iron language syntax or semantics | Maintenance milestone; no new features |
| Refactor to a visitor / vtable dispatch model for AST walking | Fundamental architectural change; out of scope — `_Static_assert` + `-Wswitch-enum` + `iron_node_assert_kind` are the pragmatic alternatives |
| Rewriting arena allocator to a tracked / ref-counted model | Big change; audit will document lifetime issues but the fix is to move pointers to the right arena, not to change the arena model |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|---|---|---|
| AUDIT-01 | Phase 65 | Complete |
| AUDIT-02 | Phase 65 | Complete |
| AUDIT-03 | Phase 65 | Complete |
| AUDIT-04 | Phase 65 | Complete |
| AUDIT-05 | Phase 65 | Complete |
| AUDIT-06 | Phase 65 | Complete |
| AUDIT-07 | Phase 65 | Complete |
| AUDIT-08 | Phase 65 | Complete |
| AUDIT-09 | Phase 65 | Complete |
| PROT-01 | Phase 66 | Complete |
| PROT-02 | Phase 66 | Complete |
| PROT-03 | Phase 66 | Complete |
| PROT-04 | Phase 66 | Complete |
| FIX-01 | Phase 67 | Pending |
| FIX-02 | Phase 67 | Pending |
| FIX-03 | Phase 67 | Pending |
| FIX-04 | Phase 67 | Pending |
| FUZZ-01 | Phase 68 | Pending |
| FUZZ-02 | Phase 68 | Pending |
| FUZZ-03 | Phase 68 | Pending |
| FUZZ-04 | Phase 68 | Pending |
| FUZZ-05 | Phase 68 | Pending |
| FUZZ-06 | Phase 68 | Pending |
| COV-01 | Phase 69 | Pending |
| COV-02 | Phase 69 | Pending |
| COV-03 | Phase 69 | Pending |
| COV-04 | Phase 69 | Pending |
| COV-05 | Phase 69 | Pending |
| REG-01 | Phase 66 | Complete |
| REG-02 | Phase 67 | Pending |
| REG-03 | Phase 69 | Pending |
| REG-04 | Phase 66 | Complete |
| VER-01 | Phase 70 | Pending |
| VER-02 | Phase 70 | Pending |
| VER-03 | Phase 70 | Pending |

**Coverage:**
- v0.1.4-alpha requirements: 35 total
- Mapped to phases: 35/35
- Unmapped: 0

---
*Requirements defined: 2026-04-11*
*Last updated: 2026-04-12 after roadmap creation (Phases 65-70)*
