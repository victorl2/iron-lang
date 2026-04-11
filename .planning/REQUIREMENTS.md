# Requirements: Iron WebAssembly Target

**Defined:** 2026-04-10
**Core Value:** A user with an Iron + raylib game runs one command and gets a runnable HTML/JS/WASM bundle that plays in a modern browser with performance indistinguishable from a native build.

## v1 Requirements

Requirements for the initial shipping WASM target. Derived from `.planning/research/SUMMARY.md` P1 scope. Each maps to exactly one roadmap phase.

### Bootstrap & Guardrails (WEB-BOOT)

- [x] **WEB-BOOT-01**: Repo contains `.emsdk-version` file at root pinning emsdk to `4.0.23`
- [x] **WEB-BOOT-03**: Linux/macOS CI workflow (`.github/workflows/web.yml`) installs emsdk 4.0.23 on `ubuntu-latest` and `macos-latest` runners and runs a placeholder `emcc hello.c` smoke test asserting that `hello.js` and `hello.wasm` are produced. (Windows CI is deferred until Iron gains base Windows support, gated on PR #17.)

### CLI (WEB-CLI)

- [x] **WEB-CLI-01**: User can run `iron build --target=web main.iron` and get a bundle in `dist/web/`
- [x] **WEB-CLI-02**: User can run `iron build --target=native main.iron` (explicit native target, same as default)
- [x] **WEB-CLI-03**: `iron build` with no `--target` defaults to native (backward compatible)
- [x] **WEB-CLI-04**: Unknown `--target=` values produce a clear error listing valid values
- [x] **WEB-CLI-05**: `iron build --target=web --release` uses optimized flags (`-Oz -flto -sASSERTIONS=0`)
- [x] **WEB-CLI-06**: `iron build --target=web` (debug) uses `-O0 -g -sASSERTIONS=1`
- [x] **WEB-CLI-07**: `iron run --target=web` builds then shells to `emrun` on the output HTML
- [x] **WEB-CLI-08**: Missing `emcc` produces a friendly error with install instructions and the pinned emsdk version
- [x] **WEB-CLI-09**: Build log prints `using emcc <version> from <path>` before compilation starts

### iron.toml `[web]` Section (WEB-MANIFEST)

- [x] **WEB-MANIFEST-01**: `iron.toml` `[web]` section is parsed into a new `IronWebConfig` struct
- [x] **WEB-MANIFEST-02**: `[web].assets` accepts a string or array of strings; values are passed as `--preload-file <path>@<path>` identity mappings
- [x] **WEB-MANIFEST-03**: `[web].title` substitutes into the HTML shell `<title>` element
- [x] **WEB-MANIFEST-04**: `[web].shell` points to a custom HTML template; default is embedded in `src/cli/web_shell_template.h`
- [x] **WEB-MANIFEST-05**: `[web].initial_memory` overrides the default `134217728` (128 MB)
- [x] **WEB-MANIFEST-06**: `[web].stack_size` overrides the default `1048576` (1 MB)
- [x] **WEB-MANIFEST-07**: `[web].pthread_pool_size` overrides the default `4`
- [x] **WEB-MANIFEST-08**: Unknown `[web].*` keys produce a warning but do not fail the build

### Runtime Hardening (WEB-RUNTIME)

- [x] **WEB-RUNTIME-01**: `iron_string_intern()` is race-free under SharedArrayBuffer (double-checked lock fix in `src/runtime/iron_string.c`)
- [x] **WEB-RUNTIME-02**: `iron_threads_shutdown()` is a `#ifdef __EMSCRIPTEN__` no-op (cannot block on browser main thread)
- [x] **WEB-RUNTIME-03**: On web, thread pool size is capped at 4 regardless of `hardwareConcurrency`
- [ ] **WEB-RUNTIME-04**: Analyzer emits an error if `await` is reachable from `Iron_main()` when `--target=web`
- [ ] **WEB-RUNTIME-05**: New file `src/stdlib/iron_time_web.c` provides `Iron_time_now()`, `Iron_time_now_ms()`, `Iron_time_now_ns()` via `emscripten_get_now()`
- [ ] **WEB-RUNTIME-06**: Web build links `iron_time_web.c` instead of `iron_time.c` (no touches to `iron_time.c`)
- [x] **WEB-RUNTIME-07**: All existing runtime files (`iron_rc.c`, `iron_builtins.c`, `iron_collections.c`) compile cleanly under emcc with zero modifications

### LIR Main-Loop Transform (WEB-EMIT)

- [ ] **WEB-EMIT-01**: New LIR pass `src/lir/web_main_loop_split.c` detects the canonical `while (!WindowShouldClose()) { body }` pattern in any function containing `InitWindow`
- [ ] **WEB-EMIT-02**: Pass reuses Iron's existing closure-capture machinery (from v1.0.0-alpha collection methods) to lift outer-scope locals into a heap-allocated frame state struct
- [ ] **WEB-EMIT-03**: Writes from within the loop body to captured locals persist correctly across frames (capture-by-reference semantics)
- [ ] **WEB-EMIT-04**: Non-canonical loop shapes (nested, for-loop, do-while, multiple top-level loops) produce a clear error naming the required pattern
- [ ] **WEB-EMIT-05**: New file `src/lir/emit_web.c` emits the web-specific `main()` wrapper using `emit_helpers.h` only (zero touches to `emit_c.c`)
- [ ] **WEB-EMIT-06**: Emitted C includes `<emscripten/emscripten.h>` at the top when target is web
- [ ] **WEB-EMIT-07**: `main()` calls `emscripten_set_main_loop_arg(frame_cb, state, 0, 0)` with `simulate_infinite_loop=0`
- [ ] **WEB-EMIT-08**: Cleanup code (`CloseWindow()`, `iron_runtime_shutdown()`, `free(state)`) runs inside the frame callback's shutdown branch before `emscripten_cancel_main_loop()`
- [ ] **WEB-EMIT-09**: `src/cli/build.c` step 12 dispatches to `emit_web_module()` when `target == WEB`, `iron_lir_emit_c()` otherwise

### Build Orchestration (WEB-BUILD)

- [ ] **WEB-BUILD-01**: New file `src/cli/build_web.c` orchestrates the full web build
- [ ] **WEB-BUILD-02**: `find_emcc()` probes PATH for `emcc`, `emcc.bat`, `emcc.cmd` (Windows compatibility)
- [ ] **WEB-BUILD-03**: emcc is invoked with the full canonical flag set including `-pthread`, `-sUSE_PTHREADS=1`, `-sPTHREAD_POOL_SIZE=4`, `-sINITIAL_MEMORY=134217728`, `-sALLOW_MEMORY_GROWTH=1`, `-sMAXIMUM_MEMORY=268435456`, `-sSTACK_SIZE=1048576`, `-sUSE_GLFW=3`, `-sFORCE_FILESYSTEM=1`, `-sGL_ENABLE_GET_PROC_ADDRESS=1`, `-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPF32,HEAP32,HEAPU8`, `-sASYNCIFY=0`
- [ ] **WEB-BUILD-04**: Every translation unit is compiled with `-pthread` (raylib amalgamation, Iron runtime, emitted C, all stdlib shims)
- [ ] **WEB-BUILD-05**: Raylib is built from the amalgamation driver `src/vendor/raylib/raylib.c` with `-DPLATFORM_WEB -DGRAPHICS_API_OPENGL_ES2` (NOT via raylib's Makefile)
- [ ] **WEB-BUILD-06**: `mkdir_p("dist/web")` is called before emcc runs; works cross-platform
- [ ] **WEB-BUILD-07**: Build NEVER passes any of these forbidden flags: `ASYNCIFY=1`, `MINIMAL_RUNTIME`, `PROXY_TO_PTHREAD`, `SAFE_HEAP`, `ALLOW_BLOCKING_ON_MAIN_THREAD`, `ERROR_ON_UNDEFINED_SYMBOLS=0`, `MODULARIZE`, `EXPORT_ES6`, `-fwasm-exceptions`
- [ ] **WEB-BUILD-08**: Build fails with a clear diagnostic if any forbidden flag is requested via user config

### Assets (WEB-ASSET)

- [ ] **WEB-ASSET-01**: `[web].assets = "assets/"` produces `--preload-file assets@assets`, mounting the directory at `/assets` in MEMFS
- [ ] **WEB-ASSET-02**: `LoadTexture("assets/foo.png")` in Iron code resolves identically on native and web
- [ ] **WEB-ASSET-03**: Analyzer emits an error if `LoadTexture`, `LoadSound`, `LoadFont`, or `LoadModel` is called at module-level (top-level) when target is web (would race with async `--preload-file`)
- [ ] **WEB-ASSET-04**: Asset directory path is resolved relative to `iron.toml`'s directory, not cwd
- [ ] **WEB-ASSET-05**: Missing `assets` directory in `iron.toml` produces a warning but does not fail the build (games can be asset-free)

### Shell Template (WEB-SHELL)

- [ ] **WEB-SHELL-01**: New file `src/cli/web_shell_template.h` embeds the default shell as a C string literal
- [ ] **WEB-SHELL-02**: Default shell is derived from `src/vendor/raylib/minshell.html` with the COOP/COEP preflight and audio-autoplay patches added
- [ ] **WEB-SHELL-03**: Shell includes a `<canvas id="canvas">` element (required by `rcore_web.c`)
- [ ] **WEB-SHELL-04**: Shell includes a preflight `<script>` that checks `self.crossOriginIsolated` on load and shows a loud visible error if false (pthread build cannot run without it)
- [ ] **WEB-SHELL-05**: Shell preserves the `{{{ SCRIPT }}}` substitution slot that emcc fills with its glue loader
- [ ] **WEB-SHELL-06**: Custom `[web].shell` templates are validated to contain `{{{ SCRIPT }}}` at build time
- [ ] **WEB-SHELL-07**: Shell handles `webglcontextlost` by showing a reload prompt

### Audio Autoplay Unlock (WEB-AUDIO)

- [ ] **WEB-AUDIO-01**: Default shell installs a one-shot `pointerdown`/`keydown` listener that calls `resume()` on all suspended `AudioContext` instances
- [ ] **WEB-AUDIO-02**: Listener is removed after first activation (no continuous overhead)
- [ ] **WEB-AUDIO-03**: Audio works correctly in Chrome, Firefox, and Safari after the first user gesture
- [ ] **WEB-AUDIO-04**: Miniaudio is NOT compiled with `MA_ENABLE_AUDIO_WORKLETS` (worklet path not validated)

### Output Layout (WEB-OUT)

- [ ] **WEB-OUT-01**: Web build output lands in `dist/web/` (created on demand)
- [ ] **WEB-OUT-02**: Primary HTML is named `index.html` regardless of source filename (itch.io / static host convention)
- [ ] **WEB-OUT-03**: Emitted siblings include `index.js`, `index.wasm`, and `index.data` (when assets present)
- [ ] **WEB-OUT-04**: Debug builds additionally emit `index.wasm.map` sourcemap
- [ ] **WEB-OUT-05**: Building from a fresh clone with no prior `dist/` succeeds without manual `mkdir`

### Pong Reference Game (WEB-VALIDATE)

- [ ] **WEB-VALIDATE-01**: `examples/pong/pong.iron` implements a two-paddle Pong game with ball, score, win condition, and state machine (title → play → game over → restart)
- [ ] **WEB-VALIDATE-02**: `examples/pong/iron.toml` includes a `[web]` section with `assets = "assets/"`, `title = "Pong"`
- [ ] **WEB-VALIDATE-03**: `examples/pong/assets/` contains at minimum a paddle-hit sound effect (WAV or OGG)
- [ ] **WEB-VALIDATE-04**: Pong source compiles and runs identically on native (`iron run --target=native`) and web (`iron run --target=web`)
- [ ] **WEB-VALIDATE-05**: Pong exercises the LIR main-loop split with at least 4 captured locals (ball position, ball velocity, paddle positions, score)
- [ ] **WEB-VALIDATE-06**: Pong web build `index.wasm` is under 2 MB gzipped in release mode
- [ ] **WEB-VALIDATE-07**: First paddle-hit sound plays correctly after the user clicks to start the game
- [ ] **WEB-VALIDATE-08**: Page reload of Pong does not leak heap allocations (no growing memory across reloads)

### Integration Tests (WEB-TEST)

- [ ] **WEB-TEST-01**: `tests/integration/web/hello.iron` — minimal "println + exit" program builds to web successfully
- [ ] **WEB-TEST-02**: `tests/integration/web/test_cli_parse.c` — `--target=web`, `--target=native`, unknown value, missing value all parse correctly
- [ ] **WEB-TEST-03**: `tests/integration/web/test_toml_parse.c` — `[web]` section with every field parses into `IronWebConfig` correctly
- [ ] **WEB-TEST-04**: `tests/integration/web/test_loop_split.c` — LIR pass correctly handles canonical shape, rejects 4 non-canonical shapes with precise errors
- [ ] **WEB-TEST-05**: `tests/integration/web/test_asset_path.c` — asset path resolution round-trip (iron.toml dir → emcc flag → MEMFS mount → `fopen`)
- [ ] **WEB-TEST-06**: `tests/integration/web/test_shell_subst.c` — shell template substitution fills `{{{ SCRIPT }}}`, `{{IRON_TITLE}}`, canvas size tokens
- [ ] **WEB-TEST-07**: `tests/integration/web/test_analyzer_loaders.c` — top-level `LoadTexture` on web produces the expected error; on native produces no error
- [ ] **WEB-TEST-08**: Headless browser smoke test asserts `crossOriginIsolated === true` and `SharedArrayBuffer` is defined for a Pong test build
- [ ] **WEB-TEST-09**: Size budget check runs on Pong in CI and fails if `.wasm` exceeds 2 MB gzipped
- [ ] **WEB-TEST-10**: Windows CI runs the full hello-world web build end-to-end *(Deferred — blocked on Iron gaining base Windows support, same rationale as Phase 14 / WEB-SERVER-01. Not built in this worker.)*
- [ ] **WEB-TEST-11**: All existing native integration tests (333+ at `main` @ c517aef / v1.1.0-alpha baseline) still pass with zero regressions

### Iron-Native Dev Server (WEB-SERVER — DEFERRED, GATED)

- [ ] **WEB-SERVER-01**: `iron serve --target=web` starts an Iron-native HTTP server using the networking milestone's HTTP server (gated on that landing on main)
- [ ] **WEB-SERVER-02**: The dev server sends `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` on every response by default
- [ ] **WEB-SERVER-03**: Documentation replaces the interim `emrun` recommendation with `iron serve` once this lands

## v2 Requirements (Deferred)

Acknowledged but NOT in this milestone's scope. Tracked so they don't get lost.

### Polish

- **WEB-POLISH-01**: `[web].single_file = true` (mutually exclusive with `[web].assets`)
- **WEB-POLISH-02**: `[web].coi_serviceworker = true` drops in MIT `coi-serviceworker.js` for GitHub Pages hosting without server header support
- **WEB-POLISH-03**: Build emits `_headers` (Netlify/Cloudflare), `vercel.json`, `netlify.toml` alongside output
- **WEB-POLISH-04**: Build-time hosting warning explaining COOP/COEP requirement when release flag is set
- **WEB-POLISH-05**: Cache-busting hashed filenames (`index.abc123.wasm`)

### Debug UX

- **WEB-DEBUG-01**: Source maps from `.iron` to browser DevTools (not just Iron→C→WASM)
- **WEB-DEBUG-02**: Hot reload on file change (likely requires separate dev loop milestone)

### Input & Mobile

- **WEB-INPUT-01**: Gamepad API bindings in `raylib.iron`
- **WEB-INPUT-02**: Touch event bindings for mobile
- **WEB-MOBILE-01**: Mobile browser certification (iOS Safari, Chrome Android)

### Async / Concurrency

- **WEB-ASYNC-01**: `await` reachable from `Iron_main()` via Asyncify (v1 errors on this)
- **WEB-ASYNC-02**: Threads-off single-threaded build variant for simpler hosting (no COOP/COEP required)

## Out of Scope

Explicitly excluded from v1 with reasoning. Rejected on sight.

| Feature | Reason |
|---------|--------|
| Direct LIR → WASM backend | ~5x more work than emscripten path; would require re-porting raylib to WebGL manually. Revisit only if toolchain friction blocks shipping. |
| Asyncify for the main loop | 20-40% runtime overhead, up to 10x WASM size blowup in some reports; defeats the point of going native-performance. |
| PWA manifest / service worker as default | Scope explosion; users who need this can drop into `[web].shell`. |
| Custom loading screen spinner | Vendored `shell.html` already has one; opt-in via `[web].shell` override. |
| Hot reload | WASM modules cannot be swapped without full reload anyway. Separate milestone if ever. |
| Mobile browser hardening | Desktop Chrome/Firefox/Safari only in v1. |
| Source maps .iron → DevTools | Huge scope; C-level `-g` is "good enough" for v1. |
| Closure Compiler (`--closure 1`) | Saves ~30-50% of JS glue (1-5 KB on a 1+ MB WASM binary) and frequently breaks `Module.ccall` / `Module.FS` name renaming. Not worth it. |
| `MODULARIZE` / `EXPORT_ES6` | Breaks the `{{{ SCRIPT }}}` template path in raylib-style shells. |
| `MINIMAL_RUNTIME=1` | Incompatible with `-pthread` in practice. |
| `PROXY_TO_PTHREAD` | Breaks `emscripten_set_main_loop` registration from the main thread. |
| `SAFE_HEAP` | Runtime cost, not needed once LIR verifier is trusted. |
| `ALLOW_BLOCKING_ON_MAIN_THREAD=1` | Papers over the underlying deadlock; fix the code instead. |
| `ERROR_ON_UNDEFINED_SYMBOLS=0` | Silences the #1 signal that something is linked wrong. Never acceptable. |
| `-fwasm-exceptions` | Breaks `simulate_infinite_loop=1` semantics; we use `=0` so this is moot but still off-limits. |
| Multi-threaded WASM without COOP/COEP | Can't exist. Locked into COOP/COEP for v1 per locked pthread decision. |
| Gamepad / touch input bindings | Stdlib gap, not web-target scope. Deferred to v2. |
| Iron HTTP server before networking milestone lands | Blocked on that milestone. Interim dev loop uses `emrun`. |
| Binary size < 500 KB | Not a realistic target with raylib + pthread + SharedArrayBuffer. 2 MB gzipped is the v1 bar. |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| WEB-BOOT-01 | Phase 1 — Bootstrap & Guardrails | Complete |
| WEB-BOOT-03 | Phase 1 — Bootstrap & Guardrails | Complete |
| WEB-CLI-01 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-02 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-03 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-04 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-05 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-06 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-07 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-08 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-CLI-09 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-01 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-02 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-03 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-04 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-05 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-06 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-07 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-MANIFEST-08 | Phase 2 — CLI + TOML Scaffold | Complete |
| WEB-RUNTIME-01 | Phase 3 — Runtime Audit | Complete |
| WEB-RUNTIME-02 | Phase 3 — Runtime Audit | Complete |
| WEB-RUNTIME-03 | Phase 3 — Runtime Audit | Complete |
| WEB-RUNTIME-04 | Phase 3 — Runtime Audit | Pending |
| WEB-RUNTIME-05 | Phase 4 — WASM-Safe Time Shim | Pending |
| WEB-RUNTIME-06 | Phase 4 — WASM-Safe Time Shim | Pending |
| WEB-RUNTIME-07 | Phase 3 — Runtime Audit | Complete |
| WEB-EMIT-01 | Phase 5 — LIR Main-Loop Split Pass | Pending |
| WEB-EMIT-02 | Phase 5 — LIR Main-Loop Split Pass | Pending |
| WEB-EMIT-03 | Phase 5 — LIR Main-Loop Split Pass | Pending |
| WEB-EMIT-04 | Phase 5 — LIR Main-Loop Split Pass | Pending |
| WEB-EMIT-05 | Phase 6 — emit_web.c Wrapper | Pending |
| WEB-EMIT-06 | Phase 6 — emit_web.c Wrapper | Pending |
| WEB-EMIT-07 | Phase 6 — emit_web.c Wrapper | Pending |
| WEB-EMIT-08 | Phase 6 — emit_web.c Wrapper | Pending |
| WEB-EMIT-09 | Phase 6 — emit_web.c Wrapper | Pending |
| WEB-BUILD-01 | Phase 7 — build_web.c emcc Orchestration | Pending |
| WEB-BUILD-02 | Phase 7 — build_web.c emcc Orchestration | Pending |
| WEB-BUILD-03 | Phase 7 — build_web.c emcc Orchestration | Pending |
| WEB-BUILD-04 | Phase 7 — build_web.c emcc Orchestration | Pending |
| WEB-BUILD-05 | Phase 8 — Raylib Web Integration | Pending |
| WEB-BUILD-06 | Phase 7 — build_web.c emcc Orchestration | Pending |
| WEB-BUILD-07 | Phase 7 — build_web.c emcc Orchestration | Pending |
| WEB-BUILD-08 | Phase 7 — build_web.c emcc Orchestration | Pending |
| WEB-ASSET-01 | Phase 10 — Asset Preload + Loader Guard | Pending |
| WEB-ASSET-02 | Phase 10 — Asset Preload + Loader Guard | Pending |
| WEB-ASSET-03 | Phase 10 — Asset Preload + Loader Guard | Pending |
| WEB-ASSET-04 | Phase 10 — Asset Preload + Loader Guard | Pending |
| WEB-ASSET-05 | Phase 10 — Asset Preload + Loader Guard | Pending |
| WEB-SHELL-01 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-SHELL-02 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-SHELL-03 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-SHELL-04 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-SHELL-05 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-SHELL-06 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-SHELL-07 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-AUDIO-01 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-AUDIO-02 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-AUDIO-03 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-AUDIO-04 | Phase 9 — Shell Template + Audio Unlock | Pending |
| WEB-OUT-01 | Phase 11 — dist/web/ Output Layout | Pending |
| WEB-OUT-02 | Phase 11 — dist/web/ Output Layout | Pending |
| WEB-OUT-03 | Phase 11 — dist/web/ Output Layout | Pending |
| WEB-OUT-04 | Phase 11 — dist/web/ Output Layout | Pending |
| WEB-OUT-05 | Phase 11 — dist/web/ Output Layout | Pending |
| WEB-VALIDATE-01 | Phase 12 — Pong Reference Game | Pending |
| WEB-VALIDATE-02 | Phase 12 — Pong Reference Game | Pending |
| WEB-VALIDATE-03 | Phase 12 — Pong Reference Game | Pending |
| WEB-VALIDATE-04 | Phase 12 — Pong Reference Game | Pending |
| WEB-VALIDATE-05 | Phase 12 — Pong Reference Game | Pending |
| WEB-VALIDATE-06 | Phase 12 — Pong Reference Game | Pending |
| WEB-VALIDATE-07 | Phase 12 — Pong Reference Game | Pending |
| WEB-VALIDATE-08 | Phase 12 — Pong Reference Game | Pending |
| WEB-TEST-01 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-02 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-03 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-04 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-05 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-06 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-07 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-08 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-09 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-10 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-TEST-11 | Phase 13 — Integration Tests + Regression | Pending |
| WEB-SERVER-01 | Phase 14 — Iron-Native Dev Server | Deferred (gated on networking milestone) |
| WEB-SERVER-02 | Phase 14 — Iron-Native Dev Server | Deferred (gated on networking milestone) |
| WEB-SERVER-03 | Phase 14 — Iron-Native Dev Server | Deferred (gated on networking milestone) |

**Coverage:**
- v1 requirements: 86 (83 active + 3 gated/deferred)
- Mapped to phases: 86 / 86 (100%)
- Unmapped: 0
- Active phases (1–13): 83 requirements (WEB-BOOT-02 dropped 2026-04-10)
- Deferred phase (14, gated on networking milestone): 3 requirements

## Open Questions Flagged by Research

These need answers before or during specific phase planning:

1. **Exact emsdk version pin** — STACK recommends `4.0.23`, PITFALLS says "verify in Phase 1". Locked at `4.0.23` unless CI demonstrates a blocker.
2. **Does Iron's analyzer have infrastructure to emit `"top-level LoadTexture forbidden"` errors?** Required for `WEB-ASSET-03` (Phase 10) and `WEB-RUNTIME-04` (Phase 3). Spike in Phase 10 / Phase 3 planning.
3. **Does `emit_helpers.h` expose enough surface for `emit_web.c` without touching `emit_c.c`?** Clean-boundary guarantee depends on this. Spike in Phase 6 planning.
4. **Canonical loop shape detection heuristic** — restricting to "top-level `while` in a function containing `InitWindow`" is safest for v1. Confirm during Phase 5 planning.
5. **Default shell `AudioContext` tracking** — `minshell.html` does NOT track contexts; `shell.html:306-335` does. Either port the tracking or patch miniaudio (invasive). Spike in Phase 9 planning.

---
*Requirements defined: 2026-04-10*
*Based on: `.planning/research/SUMMARY.md` (4 parallel researcher outputs synthesized)*
*Last updated: 2026-04-10 after `main` sync to `c517aef` / v1.1.0-alpha (PR #13 merged) — WEB-TEST-11 baseline bumped from 293 to 333 tests. All other REQ-IDs unchanged. WEB-BOOT-02 (forbidden-files hook) dropped entirely per user decision 2026-04-10; WEB-BOOT-03 retargeted to Linux/macOS web CI only (Windows CI deferred until Iron gains Windows support, gated on PR #17).*
