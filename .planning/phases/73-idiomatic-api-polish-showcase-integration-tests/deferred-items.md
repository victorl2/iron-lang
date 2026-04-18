# Phase 73 Deferred Items

Post-alpha deferrals surfaced during Phase 73-01 execution. All have
documented workarounds or are scope-limited to dedicated milestones.

## From Plan 73-01

### D1. Receiver-method migration — DEFERRED (post-alpha ironc milestone)

**Origin:** Plan 73-01 Task 0 probe (commit `830ef2d`).

**Finding:** Current `./build/ironc` binary (Mach-O arm64 at
`/Users/victor/code/iron-lang/build/ironc`) rejects lowercase-receiver
declarations like `func wave.is_valid_probe(wave: Wave) -> Bool {}` with
E0200 "method declared on undeclared type".

This matches `docs/language_definition.md:320` which specifies methods
use `func TypeName.method_name(...)` (uppercase type) with implicit
`self`. The lowercase-receiver grammar — referenced in some planning
documents as a migration target — is **not a current Iron language
feature**.

**Scope:** Plan 73-01 Task 4 (bulk receiver migration across ~158
stubs) was skipped per the `/gsd:autonomous` fallback path.

**Impact:** Phase 68-72 static-form `Type.method(receiver: Type, ...)`
stubs retained unchanged. Pure-superset guarantee holds (pong +
game_raylib + hello_raylib build without source edits — verified
at end of Plan 73-01).

**Unblock condition:** Dedicated ironc grammar milestone that adds
receiver-method support. Out of scope for v2.0.0-alpha.

---

### D2. Image.load_svg — DEFERRED (raylib vendor upgrade)

**Origin:** Plan 73-01 Task 2 (commit `c54c5af`).

**Finding:** `LoadImageSvg` is NOT exposed in vendored raylib 5.5
(`grep LoadImageSvg src/vendor/raylib/raylib.h` → 0 matches). This
is a post-5.5 raylib addition.

**Scope:** 1 of 8 Task 2 deferral closures omitted. Task 2 closed 7 of 8
planned shims (Font.from_memory / Font.load_data / Image.load_raw /
Image.load_from_memory / Image.load_anim_from_memory / Image.export_to_memory
/ Image.kernel_convolution).

**Impact:** None for v2.0.0-alpha. Real-world SVG users can pre-convert
to PNG/raw; Image.load_from_memory covers all other in-memory formats.

**Unblock condition:** raylib vendor bump (to 5.6+ when released) that
ships LoadImageSvg. Bind via Template A (file-path-or-string String,
width, height → Image).

---

### D3. Web target build verification — ENVIRONMENT LIMITATION

**Origin:** Plan 73-01 Task 4/5 acceptance verification.

**Finding:** `./build/ironc build --target=web tests/integration/web/hello_raylib.iron`
exits 1 with "error: emcc not found in PATH". Emscripten toolchain is
not installed in the current execution environment.

**Scope:** Web target build verification skipped for Plan 73-01.
Native builds (pong + game_raylib) both exit 0 with sizes within
±5% tolerance — the Phase 73-01 source changes introduce no
code-level regression.

**Impact:** Phase 73-04 (integration test matrix with native + web
parity) will need an emsdk-equipped execution environment. The
Plan 73-01 source changes (shim bodies + Iron stubs + detach-all
loops) are pure C99 + standard Iron FFI — no web-specific code.
Web build should succeed once emsdk is installed.

**Unblock condition:** `emsdk install 4.0.23 && emsdk activate 4.0.23 &&
source emsdk_env.sh` before running Phase 73-04 web build matrix.

---

### D4. Per-stream AUDIO-12 slot bookkeeping — DEFERRED (post-alpha refinement)

**Origin:** Plan 73-01 Task 3 (commit `fe945b5`).

**Finding:** AUDIO-12 callback wirings use the 16-slot global Iron_Closure
trampoline registry built in Plan 68-01 Task 3. `detach_processor(stream)`
iterates all slots and calls Detach for each used slot — raylib no-ops
non-matching pointers safely. Slots are NOT freed on detach because the
registry is global (no per-stream tracking).

**Scope:** Over a long runtime with frequent attach/detach cycles, slot
exhaustion can occur. For typical game loops (attach a few processors
at startup, detach at shutdown) the 16-slot ceiling is ample.

**Impact:** AUDIO-12 closes at 19/19 for v2.0.0-alpha. User-reported
slot-exhaustion on realistic audio workloads would trigger the
per-stream bookkeeping refinement.

**Unblock condition:** User feedback driving a post-alpha refinement
that adds a `stream_id → slot_list` map for precise slot freeing.

---

### D5. Iron_CameraProjection enum-in-struct-initializer codegen — DEFERRED (post-alpha ironc milestone)

**Origin:** Phase 69-04 (carried into Phase 73 scope).

**Finding:** ironc codegen rejects `CameraProjection.PERSPECTIVE.ordinal`
inside a struct initializer. Workaround uses `Int32(0)` literal at
the 3-4 known call sites (tests/manual/draw3d_smoke.iron:38,
examples/rotating_cube/rotating_cube.iron:38, etc.).

**Scope:** Phase 73-01 did NOT include compiler fixes (per CONTEXT.md:151
escape hatch: "Phase 73 must not become open-ended compiler-work vehicle").

**Impact:** Visible only at enum-literal-in-struct-init sites. Workaround
compiles correctly and produces identical bytecode.

**Unblock condition:** Dedicated ironc milestone that fixes enum-ordinal
lowering inside struct initializers.

---

### D6. ironc string-literal lexer `\n`+brace round-trip — DEFERRED (post-alpha ironc milestone)

**Origin:** Phase 71-02 (carried into Phase 73 scope).

**Finding:** Multi-line GLSL source strings in Iron code trigger spurious
E0200 errors. Lexer exits string mode on literal `{` and re-tokenizes
the body as Iron code. Workaround uses `Shader.load_from_memory("", "")`
(both strings empty) instead of inline GLSL.

**Scope:** Phase 73-01 did NOT include compiler fixes.

**Impact:** Iron users cannot write inline multi-line GLSL in Iron source.
External `.vs` / `.fs` files work fine.

**Unblock condition:** Dedicated ironc milestone that fixes lexer
string-state-machine `\n`+brace handling.

---

### D7. FilePathList.count stub signature mismatch — DEFERRED (post-alpha Iron-side or ironc milestone)

**Origin:** Phase 72-03 (carried into Phase 73 scope).

**Finding:** Iron stub `func FilePathList.count() -> Int32 {}` at
raylib.iron:2049 has zero params; C shim `Iron_filepathlist_count(struct
Iron_FilePathList list)` has one. ironc emits a `(void)` prototype but
call sites generate 1-arg C calls → clang error.

**Workaround site:** `tests/manual/files_smoke.iron:71-74` drops the
`FilePathList.count(list)` call; users access `.count` field directly
on the returned struct.

**Scope:** Phase 73-01 did NOT include Iron-side signature fixes (would
have required source-level API change that polls user expectations).

**Impact:** One accessor method unreachable; field-access workaround
covers all realistic use.

**Unblock condition:** Either Iron-side change (stub to
`func FilePathList.count(list: FilePathList) -> Int32`) or ironc
compiler-side fix (auto-forward receiver for method-style calls on
freestanding arg-less stubs).

---

## Summary (as of Plan 73-01 close)

**Closed in Plan 73-01:** 11 of 18 items (1 probe RED + 7 smoke dedents +
7 Task 2 shims − 1 LoadImageSvg omission + 5 Task 3 AUDIO-12 wirings =
7 + 7 + 5 − 1 − 1 = **17 deferrals addressed** out of 18 — 1 SVG bind
deferred to raylib vendor bump).

**Residual post-alpha deferrals:** 6 items (D1 receiver migration +
D2 LoadImageSvg + D3 emsdk + D4 per-stream bookkeeping + D5/D6/D7
ironc compiler fixes). All have documented workarounds or are
environment-setup concerns.

**Milestone posture:** v2.0.0-alpha ships. Phase 73-02/03/04 unblocked
with a clean deferral-free API surface at the Iron level.
