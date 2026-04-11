---
gsd_state_version: 1.0
milestone: v1.1
milestone_name: milestone
status: planning
stopped_at: Completed 02-cli-toml-scaffold plan-05 dispatch-and-wiring
last_updated: "2026-04-11T15:32:18.251Z"
last_activity: "2026-04-10 — ROADMAP.md created from research synthesis (87/87 v1 requirements mapped across 14 phases, Phase 14 deferred/gated), then synced main from 4de97c8 → c517aef (v1.1.0-alpha, PR #13 merged), refocused WEB-BOOT-02 from PR #13 to PR #17 conflict zone, bumped WEB-TEST-11 baseline from 293 → 333 tests."
progress:
  total_phases: 14
  completed_phases: 1
  total_plans: 7
  completed_plans: 6
  percent: 0
---

# Project State

## Project Reference

See: `.planning/PROJECT.md` (updated 2026-04-10)

**Core value:** A user with an Iron + raylib game runs one command and gets a runnable HTML/JS/WASM bundle that plays in a modern browser with performance indistinguishable from a native build.

**Current focus:** Roadmap complete. Main synced to `c517aef` / **v1.1.0-alpha** (PR #13 merged). Ready to plan Phase 1 (Bootstrap & Guardrails). Parallel conflict zone is now PR #17 (networking foundation, draft open).

## Current Position

Phase: Not started (Phase 1 next — Bootstrap & Guardrails)
Plan: —
Status: Roadmap approved → ready to plan Phase 1
Last activity: 2026-04-10 — ROADMAP.md created from research synthesis (87/87 v1 requirements mapped across 14 phases, Phase 14 deferred/gated), then synced main from 4de97c8 → c517aef (v1.1.0-alpha, PR #13 merged), refocused WEB-BOOT-02 from PR #13 to PR #17 conflict zone, bumped WEB-TEST-11 baseline from 293 → 333 tests.

Progress: [░░░░░░░░░░] 0% (0 of TBD plans)

## Performance Metrics

**Velocity:**
- Total plans completed: 0
- Average duration: —
- Total execution time: —

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| — | — | — | — |

**Recent Trend:**
- Last 5 plans: —
- Trend: —

*Updated after each plan completion*
| Phase 01-bootstrap-guardrails P01 | 25 | 5 tasks | 5 files |
| Phase 02-cli-toml-scaffold P01 | 2 | 3 tasks | 3 files |
| Phase 02-cli-toml-scaffold P02 | 126 | 3 tasks | 1 files |
| Phase 02-cli-toml-scaffold P03 | 2 | 2 tasks | 1 files |
| Phase 02-cli-toml-scaffold P04 | 8m | 2 tasks | 2 files |
| Phase 02-cli-toml-scaffold P05 | 2m | 3 tasks | 2 files |

## Accumulated Context

### Decisions

Decisions are logged in `.planning/PROJECT.md` Key Decisions table. Recent decisions affecting current work:

- **Locked:** C → Emscripten → WASM path (not direct LIR→WASM backend) — preserves 5.5k-line C emitter investment, raylib web for free
- **Locked:** Auto-transform `while(!WindowShouldClose())` at LIR emit time for `--target=web` (not Asyncify)
- **Locked:** Full pthread via Emscripten `-pthread` + SharedArrayBuffer (requires COOP/COEP hosting)
- **Locked:** Reference validation via Pong game with sound + specific tests
- **Locked:** `[web]` section in `iron.toml` for scoped target config
- **Locked:** Preload `assets/` via `--preload-file` identity mapping
- **Locked:** Iron-native dev server is Phase 14 — gated on networking milestone HTTP server landing; interim dev uses `emrun`
- **Locked:** emsdk 4.0.23 (last stable 4.x) via `.emsdk-version` file
- **Locked:** ALL new web logic lands in NEW files only — the "new-files-only" rule is now enforced against PR #17's open-diff zone (PR #13 merged 2026-04-10 so its file list is no longer a concern): avoid `src/lir/emit_c.c`, `src/lir/emit_helpers.c/h`, `src/runtime/iron_runtime.h`, `src/analyzer/resolve.c`, `src/analyzer/types.c/h`, `src/hir/hir_lower.c`, `src/parser/parser.c`, `src/parser/ast.h`, `src/stdlib/time.iron`, `src/stdlib/string.iron`. Files touched by BOTH milestones (`iron_string.c`, `iron_threads.c`, `typecheck.c`, `cli/build.c`) are WARN-only in Phase 1's forbidden-files hook.
- [Phase 01-bootstrap-guardrails]: mymindstorm/setup-emsdk@v14 has no version-file input; version read via shell step into GITHUB_OUTPUT; 4.0.23 does not appear in web.yml YAML
- [Phase 01-bootstrap-guardrails]: WEB-BOOT-02 (forbidden-files hook) dropped permanently — enforced by discipline + 333 integration tests, not by tooling
- [Phase 01-bootstrap-guardrails]: WEB-BOOT-03 retargeted from Windows CI to Linux/macOS CI only — Windows support gated on PR #17 landing
- [Phase 02-cli-toml-scaffold]: IronWebConfig embedded by value on IronProject; zero-init from calloc gives NULL/0 == not-set semantics
- [Phase 02-cli-toml-scaffold]: int used for numeric web fields matching existing plain-int style in IronProject.dep_count
- [Phase 02-cli-toml-scaffold]: Levenshtein cap at 64 chars returns INT_MAX; stack arrays v0[66]/v1[66] — no heap, no VLA
- [Phase 02-cli-toml-scaffold]: parse_toml_string_array kept static/unexported — single consumer in assets branch
- [Phase 02-cli-toml-scaffold]: No short form -t for --target — reserved for future use (CONTEXT.md decision)
- [Phase 02-cli-toml-scaffold]: iron run --target=web parses and stores flags only; emrun shell-out deferred to Phase 7
- [Phase 02-cli-toml-scaffold]: popen() used for emcc --version capture (simpler than posix_spawnp + pipe for one-shot read)
- [Phase 02-cli-toml-scaffold]: Interpretation A for -O2: append after -O3 (clang last-wins) rather than conditional — smaller diff, matches CONTEXT.md verbatim
- [Phase 02-cli-toml-scaffold]: Web dispatch placed as absolute first statement in iron_build() before base_dir resolution — web path fully owns its execution

### Pending Todos

None yet.

### Blockers/Concerns

- **Phase 14 (Iron-Native Dev Server)** — BLOCKED on parallel networking milestone's HTTP server landing on `main`. Phases 1–13 ship independently of this block.
- **Phase 5 (LIR main-loop split)** — HIGH risk; needs `/gsd:research-phase` spike during planning to verify closure-capture machinery and `emit_helpers.h` surface.
- **Phase 6 (emit_web.c)** — MEDIUM risk; depends on `emit_helpers.h` exposing enough surface without touching `emit_c.c`.
- **Phase 9 (shell template AudioContext tracking)** — LOW-MEDIUM risk; may require porting Proxy tracking from `shell.html:306-335` into the default shell.
- **Phase 10 (analyzer top-level loader error)** — MEDIUM risk; depends on Iron analyzer having "forbidden call at top-level" diagnostic infrastructure.

## Session Continuity

Last session: 2026-04-11T15:32:18.248Z
Stopped at: Completed 02-cli-toml-scaffold plan-05 dispatch-and-wiring
Resume file: None
